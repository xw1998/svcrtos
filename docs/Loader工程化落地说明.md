# SVCrtOS Loader 工程化落地说明（灵活分区版）

> 落地范围：按《SVCRTOS Loader 工程化落地指导（灵活分区版）》实现分区配置、分散加载自动生成、
> 共享分区表、镜像格式、Flash 驱动、分区内加载器、SVC 0x18 用户接口与 MDK 工程挂接。
> 采用**单区间最小闭环**（`APP_MAX_COUNT = 1`）与 **Loader 内核内服务**（不新建独立固件工程）。

## 1. 新增文件

| 文件 | 作用 |
|---|---|
| `config/svcrt_partition.h` | 全工程唯一地址源头：芯片 4 参数 + 分区策略 + 全部派生地址 |
| `tools/gen_scatter.py` | 由配置头生成各工程 `.sct`，支持 `--target/--output/--check/--dump/--header` |
| `tools/pack_app.py` | 把 Keil 的 `.axf`/`.bin` 打包为 `.svcapp`（256 字节头 + CRC32），支持 `--info/--verify` |
| `kernelsrc/include/svcrt_share.h` | 共享内存分区表结构（运行期接口，运行于 Loader/App 亦可读） |
| `kernelsrc/include/svcrt_app_image.h` | App 镜像 256 字节头 + CRC32 声明 |
| `kernelsrc/include/svcrt_ptable.h` / `src/svcrt_ptable.c` | 分区表运行期初始化与槽位状态维护 |
| `kernelsrc/include/svcrt_loader.h` / `src/svcrt_loader.c` | 镜像校验、擦写、流式加载、任务启动/停止 |
| `board/stm32f427/drvflash.h` / `drvflash.c` | 片内 Flash 擦除/编程（HAL 实现，支持 1MB/2MB 扇区划分） |
| `build/*.sct` | 由脚本产出的分散加载文件（构建产物，禁止手工编辑） |

## 2. 当前 Flash / RAM 布局

Flash（1MB，`CHIP_FLASH_SIZE`）：

| 分区 | 基址 | 大小 |
|---|---|---|
| KERNEL | `0x08000000` | 256 KB |
| DRIVER_POOL | `0x08040000` | 256 KB |
| APP_SLOT0 | `0x08080000` | 512 KB |

RAM（128KB，`CHIP_RAM_SIZE`）：

| 分区 | 基址 | 大小 |
|---|---|---|
| SHARE（分区表） | `0x20000000` | 8 KB |
| KERNEL | `0x20002000` | 88 KB |
| DRIVER | `0x20018000` | 16 KB |
| APP | `0x2001C000` | 16 KB |

**修改芯片容量只需改 `CHIP_FLASH_SIZE` / `CHIP_RAM_SIZE`**，其余地址与 `.sct` 均由脚本推导。
验收测试：把 `CHIP_FLASH_SIZE` 改为 2MB 后，`APP_SLOT0` 自动扩展到 1536KB，其他文件无需改动。

## 3. 运行期接口

### 3.1 SVC 0x18（APP_MGR）

| 子命令 | 功能 | 参数（`args[]`） |
|---|---|---|
| 1 | 获取分区表地址 | 无 |
| 2 | 从设备加载 App 镜像 | `args[1]=dev`，`args[2]=image_len` |
| 3 | 启动 App | `args[1]=slot` |
| 4 | 停止 App | `args[1]=slot` |
| 5 | 查询槽位状态 | `args[1]=slot` |

### 3.2 用户 API

`svcrt_partition_table_addr()`、`svcrt_app_load(dev, image_len)`、`svcrt_app_start(slot)`、
`svcrt_app_stop(slot)`、`svcrt_app_status(slot)`（`svcrt.h` 声明，`oslib.c` / `svcrt_oslib.c` 实现）。

返回错误码：`0..`=槽位号；负数见 `svcrt_loader.h`（-1 参数、-2 魔数、-3 兼容签名、-4 长度、
-5 CRC、-6 Flash、-7 无空闲槽位、-8 任务注册失败、-9 状态不允许）。

## 4. 加载流程（`svcrt_loader_load_*`）

1. 校验镜像头：`magic == 'SVCA'`、`hw_compat_id == SVCRT_HW_COMPAT_ID`、`image_size != 0`；
2. 整镜像 CRC32 与头中 `crc32` 比对（`crc32` 字段按 0 参与，算法 IEEE 802.3 反射多项式，与打包工具一致）；
3. 查找空闲槽位，擦除槽位所在 Flash 扇区；
4. 写入镜像（256 字节头 + 负载）；设备路径按 512 字节分块流式读写，不要求整镜像驻留 RAM；
5. 从 Flash 回读复算 CRC 确认；
6. 槽位状态置 `LOADED`，记录入口 `slot_base + entry_offset`（ARM 自动置 Thumb 位）；
7. `svcrt_loader_start()` 用 `APP_TASK_PRIORITY` / `APP_TASK_STACK_SIZE` 注册任务并触发调度。

## 5. MDK 工程挂接

5 个工程统一改为：

- `<umfTarg>0</umfTarg>`（不使用 Target Dialog 内存布局）
- `<ScatterFile>` 指向 `build\{kernel,driver,app}.sct`
- `Before Build/Rebuild` 自动执行 `python tools/gen_scatter.py --target <t> --output build\<t>.sct`
- 内核工程额外：IncludePath 追加 `config`；纳入 `svcrt_ptable.c`、`svcrt_loader.c`、`drvflash.c`

映射关系：

| 工程 | target | 分散加载文件 |
|---|---|---|
| SVCRTOS_TEST | kernel | `build/kernel.sct` |
| APP_DEMO / BLED_APP | app | `build/app.sct` |
| BLED_DRV / DRV_DEMO | driver | `build/driver.sct` |

## 6. 硬编码清理

`example/.../SVCRTOS_TEST/Core/Src/main.c` 中：

```c
#define BLED_DRV_ENTRY  (DRIVER_POOL_BASE | 1u)   /* 原 0x08060000 */
#define BLED_APP_ENTRY  (APP_SLOT0_BASE   | 1u)   /* 原 0x08080000 */
```

并新增 `svcrt_ptable_init()` 调用（`svcrt_kernel_init` 首行），保证加载器使用前分区表已就绪。
Loader / App 工程**不包含** `config/svcrt_partition.h`，布局经 SVC 0x18 子命令 1 在运行期获取。

## 7. 验证记录

- AC6（armclang）语法校验：`kernelsrc/src/*.c`（含新增 `svcrt_ptable.c`、`svcrt_loader.c`）、`board/stm32f427/drvflash.c`、`main.c` —— 全部零错误零告警；
- AC5（armcc）语法校验：`kernelsrc/app/oslib.c`、`kernelsrc/sdk/app_sdk/svcrt_oslib.c`（含 `__svc(0x18)`）—— 通过；
- 脚本验收：`gen_scatter.py --check/--dump` 通过；改 `CHIP_FLASH_SIZE` 为 2MB 后 `APP_SLOT0` 自动变 1536KB；
- 5 个 `.uvprojx` 经 XML 解析校验合法。

## 8. 镜像打包工具（`tools/pack_app.py`）

把 App / 驱动工程的编译产物打包成 Loader 可加载的 `.svcapp`。地址、槽位容量、
`hw_compat_id` 全部由 `config/svcrt_partition.h` 推导，不硬编码。

```bash
# 由 .axf 打包（推荐，自动从符号表解析入口偏移）
python tools/pack_app.py --axf build/APP_DEMO/APP_DEMO.axf \
    --type app --version 1.0.0 --name "LED 闪烁示例" --out build/APP_DEMO/APP_DEMO.svcapp

# 由 .bin 打包（需显式给出入口偏移）
python tools/pack_app.py --bin build/BLED_DRV/bled_drv.bin \
    --type driver --version 1.0.0 --entry-offset 0x0 --out build/BLED_DRV/BLED_DRV.svcapp

# 查看 / 校验
python tools/pack_app.py --info   build/APP_DEMO/APP_DEMO.svcapp
python tools/pack_app.py --verify build/APP_DEMO/APP_DEMO.svcapp
```

**入口符号约定**：镜像入口是「任务型函数」（内部循环调用 `svcrt_task_wait` 等）。

| 符号 | 说明 |
|---|---|
| `APPSTART` / `DRVSTART`（默认，推荐） | SDK 启动入口，先执行 C 运行库初始化（`.data` 拷贝、`.bss` 清零）再进入 `AppMain`/`DrvMain`。**镜像含初始化全局变量时必须用它** |
| `AppMain` / `DrvMain` | 直接进入业务函数，跳过运行时初始化，仅当镜像无需初始化全局变量时可用 |

工具会自动校验「ELF 镜像基址 == 分区槽位基址」，不一致直接报错并提示检查 `.sct`——
这是防止链接布局与打包布局悄悄错位的关键护栏。

## 9. 遗留待办

1. `board/stm32f427/svcrt_board_config.h` 中 `SVCRT_SHARE_MEM_ADDR = 0x20028000` 为旧值且已超出 128KB RAM，当前内核未引用该宏（分区表现在由 `SHARE_RAM_BASE` 决定），建议后续统一清理；
3. 各工程 `MDK-ARM` 目录下遗留的 `app_demo.sct` / `bled_app.sct` / `bled_drv.sct` / `drv_demo.sct` 已不再被引用，可删除；
4. App 镜像运行在 `APP_RAM_BASE`，但当前单区间闭环未对 App 做 MPU 隔离（`SVCRT_USE_MPU = 0`），后续多区间时需补齐；
5. `example/.../APP_DEMO/Src/app_config.c` 中的 `ram_start/rom_start`（`0x20010000` / `0x08020000`）是旧布局残留，
   当前内核未读取该表（真实布局由 `.sct` 决定），建议后续改为由分区头派生或直接移除；
6. 槽位状态仍只存在于共享 RAM，掉电即丢失——这是走向「像手机一样装应用」的下一道门槛，需要持久化槽位元数据。
