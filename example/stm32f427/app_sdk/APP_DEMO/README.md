# SVCrtOS App SDK 用户态应用示例工程

本示例展示如何使用 **App SDK** 开发一个可独立编译、可独立调试的用户态应用程序。
应用以 `.svcapp`（带 256 字节镜像头 + CRC）形式安装，开发期也可直接烧录裸镜像。

> 完整操作流程见 [`docs/SVCrtOS应用安装与调试指南.md`](../../../../docs/SVCrtOS应用安装与调试指南.md)。
> 本文只说明本示例工程自身的结构与用法。

## 目录结构

```
APP_DEMO/
├── MDK-ARM/
│   └── app_demo.uvprojx     # Keil MDK 工程文件（scatter 由脚本生成，见下）
└── Src/
    └── app_demo.c           # 应用主体，含 AppMain() 入口
```

工程引用的 SDK 文件位于 `kernelsrc/sdk/app_sdk/`：

| 文件 | 作用 |
|------|------|
| `svcrt_app_main.c` | 提供 `main()` 入口，调用用户实现的 `AppMain()` |
| `svcrt_oslib.c`    | SVC 系统调用封装（唯一实现，全工程共用） |
| `svcrt_app_start.s`| RESET 向量，导出 `APPSTART` 入口点 |

## 地址从哪来（重要）

**本工程不含任何物理地址。** 所有 ROM / RAM / 栈的大小与位置只在
[`config/svcrt_partition.h`](../../../../config/svcrt_partition.h) 定义一次，
编译前由 [`tools/gen_scatter.py`](../../../../tools/gen_scatter.py) 自动生成
`build/app.sct`，并由 MDK 的 Before Make 钩子自动执行。

因此：

- 改布局 → 只改 `config/svcrt_partition.h`，不要动 `.sct`、更不要在 C 代码里写地址；
- 工程选项里的 scatter file 指向 `build/app.sct`，它是生成物，不入库手工维护；
- 运行期想查询自己的分区信息，用 SDK 的 `svcrt_app_status()` / 内核分区表接口，不要猜地址。

应用栈同样由内核在启动应用任务时按 `config/svcrt_partition.h` 的
`APP_TASK_STACK_SIZE` 推导，应用侧不需要（也不应该）自己声明栈区间。

## 编译步骤

1. 用 Keil MDK 打开 `MDK-ARM/app_demo.uvprojx`；
2. 确认 Device 为 STM32F427VGTx；
3. 直接 Build（Before Make 会先生成 `build/app.sct`）；
4. 产物为 `MDK-ARM/Objects/app_demo.axf`。

## 打包 / 安装 / 调试

三条路径，按需要选：

| 场景 | 做法 |
|------|------|
| 正式安装包 | `python tools/pack_app.py --elf <axf> --type app --out app_demo.svcapp`，再通过内核安装任务（串口）下发 |
| 开发期快速验证 | 直接把裸 `.bin` 烧到本应用分区基址，内核扫描时按裸镜像识别入口（`APP_ALLOW_RAW_IMAGE`） |
| MDK 在线调试 | 固定地址烧录后直接在 MDK 里 Load & Debug，可对 `AppMain()` 下断点 |

> 发布固件时请把 `config/svcrt_partition.h` 里的 `APP_ALLOW_RAW_IMAGE` 置 0，
> 只接受带 CRC 的 `.svcapp`。

## 示例功能概览

`app_demo.c` 中的 `AppMain()` 演示了：

- **设备 IO**：`svcrt_dev_open/write/read`（LED 与串口）
- **任务延时**：`svcrt_task_wait`
- **信号量**：`svcrt_sem_create/post`
- **互斥锁**：`svcrt_mutex_create/lock/unlock`

## 注意事项

1. 应用代码不能直接访问硬件寄存器，必须通过 `svcrt_dev_*` 接口；
2. `AppMain()` 不能返回，必须包含无限循环；
3. 栈大小由 `config/svcrt_partition.h` 的 `APP_TASK_STACK_SIZE` 决定（当前 4KB），
   不要在栈上分配大数组；
4. 仅需包含 `svcrt.h` 即可使用全部用户态 API。
