# SVCrtOS Driver SDK 用户态驱动示例工程

本示例展示如何使用 **Driver SDK 用户态模式** 开发一个可独立编译、可独立调试的
虚拟温度传感器驱动，应用侧通过 `svcrt_dev_open("TEMP", 0)` 使用它。

> 完整操作流程见 [`docs/SVCrtOS应用安装与调试指南.md`](../../../../docs/SVCrtOS应用安装与调试指南.md)。
> 本文只说明本示例工程自身的结构与用法。

## 目录结构

```
DRV_DEMO/
├── MDK-ARM/
│   └── drv_demo.uvprojx     # Keil MDK 工程文件（scatter 由脚本生成，见下）
└── Src/
    └── temp_drv.c           # 驱动实现，含 DrvMain() 入口
```

工程引用的 SDK 文件位于 `kernelsrc/sdk/driver_sdk/`：

| 文件 | 作用 |
|------|------|
| `svcrt_drv_main.c`     | 提供 `main()` 入口，调用用户实现的 `DrvMain()` |
| `svcrt_driver_bridge.c`| 注册桥接层，通过 SVC 0x14 调用内核 |
| `svcrt_drv_oslib.c`    | 用户态 OS 接口封装 |
| `svcrt_drv_start.s`    | RESET 向量，导出 `DRVSTART` 入口点 |

## 编译模式

工程已定义宏 **`SVCRT_DRV_USER_MODE`**，使 `svcrt_driver_bridge.c` 通过 SVC 0x14
陷入内核完成驱动注册。未定义此宏时 SDK 默认为内核态模式（静态库链接）。

## 地址从哪来（重要）

**本工程不含任何物理地址。** App 与驱动共用的统一镜像池 `IMAGE_POOL` 的基址与容量只在
[`config/svcrt_partition.h`](../../../../config/svcrt_partition.h) 定义一次
（`IMAGE_POOL_BASE` / `IMAGE_POOL_SIZE` / `IMAGE_POOL_SECTOR`），编译前由
[`tools/gen_app_sct.py`](../../../../tools/gen_app_sct.py)（内部调
[`tools/gen_scatter.py`](../../../../tools/gen_scatter.py)）自动生成
`build/dev0.sct`，由 MDK 的 Before Make 钩子自动执行。

- 改布局 → 只改 `config/svcrt_partition.h`；
- 池按**分配单元**（一个单元 = 一个物理擦除扇区）切分，每个单元独立擦写、互不覆盖。本工程用的是开发槽位表的 **0 号条目**（`SVCRT_DEV_SLOT0_UNIT = 0`，类型驱动，占 1 个单元），scatter 由 `py -3 tools\gen_app_sct.py --project drv_demo.uvprojx --type driver --dev-slot 0 --ram-size 4096` 生成；
- `gen_app_sct.py` 支持 `--dev-slot`，`gen_scatter.py` 支持 `--dev-slot` / `--unit` / `--units`，`pack_app.py` 支持 `--dev-slot`，都写进命令参数，不再有「只能落在 0 号槽」的限制；
- 驱动栈由内核从**该镜像自己的 RAM 窗口顶部**切出，驱动侧不需要声明。带镜像头的 `.svcapp` 用头部声明的 `ram_size` 走伙伴分配；裸镜像没有头部，窗口由 `SVCRT_DEV_SLOT_RAM_BASE(unit)` 静态给出（本板 `SVCRT_DEV_RAM_WINDOW = 16KB`）。多镜像并存时各占一块 RAM，栈不会互相覆盖。

## 编译步骤

1. 用 Keil MDK 打开 `MDK-ARM/drv_demo.uvprojx`；
2. 确认 Device 为 STM32F427VGTx，宏定义包含 `SVCRT_DRV_USER_MODE`；
3. 直接 Build（Before Make 会先生成 `build/driver.sct`）；
4. 产物为 `MDK-ARM/Objects/drv_demo.axf`。

## 打包 / 安装 / 调试

| 场景 | 做法 |
|------|------|
| 正式安装包 | `python tools/pack_app.py --project MDK-ARM/drv_demo.uvprojx --type driver --name DRV_DEMO --out drv_demo.svcapp`（工具自动编译 A/B/C/D 四遍做差分重定位），由内核安装任务按镜像头 `type` 落进统一镜像池（驱动与 App 共用一个池） |
| 开发期快速验证 | 直接把裸 `.bin` 烧到开发槽位 0 的单元基址（`IMAGE_POOL_BASE + 0 × IMAGE_POOL_SECTOR`），内核扫描时按裸镜像识别入口（需 `APP_ALLOW_RAW_IMAGE=1`） |
| MDK 在线调试 | 固定地址烧录后直接在 MDK 里 Load & Debug，可对 `DrvMain()` 下断点 |

## 驱动实现说明

`temp_drv.c` 实现了一个虚拟温度传感器：

- **5 个标准接口**：open / close / read / write / ctrl
- **虚拟设备**：无硬件依赖，write 返回错误
- **温度单位切换**：通过 `TEMP_CTRL_SET_UNIT` 切换摄氏度 / 华氏度
- **驱动注册**：在 `DrvMain()` 中调用 `svcrt_drv_register("TEMP", ...)`

## 应用侧调用示例

```c
int32 temp = svcrt_dev_open("TEMP", 0);
uint8 buf[2];
svcrt_dev_read(temp, buf, 2);          /* 读取温度值（0.1 度精度） */
int16 t = (int16)(buf[0] | (buf[1] << 8));
svcrt_dev_ctrl(temp, 0x0100, 1);       /* 切换为华氏度 */
```

## 注意事项

1. 设备对象结构体第一个成员必须是 `svcrt_dev_hdr_t`；
2. `DrvMain()` 不能返回，必须包含无限循环，且循环里要用 `svcrt_task_wait()` 让出 CPU ——
   驱动优先级(9)高于 App(10)，空转 `while(1){}` 会把 App 永久饿死；
3. 用户态驱动不能直接操作硬件寄存器，需通过内核代理；
4. 仅需包含 `svcrt_driver_sdk.h` 即可使用全部 API。
