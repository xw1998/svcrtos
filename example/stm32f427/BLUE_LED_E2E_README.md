# SVCrtOS 端到端示例：外部 Driver + 外部 App 操作蓝灯

本示例演示 SVCrtOS 的完整分态架构：**外部用户态驱动**注册硬件设备，
**外部用户态应用**通过设备框架操作该硬件，三者（内核 + 驱动 + 应用）以独立固件形式协作。

> **本文不写任何物理地址。** 全部布局只在
> [`config/svcrt_partition.h`](../../config/svcrt_partition.h) 定义一次，
> 由 [`tools/gen_scatter.py`](../../tools/gen_scatter.py) 生成各工程的 `.sct`。
> 完整操作步骤见 [`docs/SVCrtOS应用安装与调试指南.md`](../../docs/SVCrtOS应用安装与调试指南.md)。

## 架构链路

```
+------------------+   svcrt_dev_open("BLED")    +------------------+
|   BLED_APP        | --------------------------> |   SVCrtOS 内核    |
| (用户态应用固件)  |   svcrt_dev_write(...)       |   设备框架        |
|  烧在 App 槽位    | <-------------------------- |   (设备表/句柄)   |
+------------------+                              +--------+---------+
                                                            | 调用驱动接口
                                              SVC 0x14 注册  |
+------------------+   svcrt_drv_register("BLED")          v
|   BLED_DRV        | --------------------------> +------------------+
| (用户态驱动固件)  |                              |  bled_drv 接口    |
|  烧进镜像池       |   直接操作 PC2 寄存器 ----->|  GPIOC->BSRR      |
+------------------+                              +------------------+
                                                            |
                                                            v
                                                      RGB 灯珠蓝色(PC2)
```

## 文件组成

```
example/stm32f427/
+-- driver_sdk/BLED_DRV/          # 外部蓝灯驱动
|   +-- MDK-ARM/bled_drv.uvprojx  # scatter 由脚本生成到 build/driver.sct
|   +-- Src/bled_drv.c            # 裸寄存器驱动 PC2，注册 "BLED"
|
+-- app_sdk/BLED_APP/             # 外部蓝灯应用
    +-- MDK-ARM/bled_app.uvprojx  # scatter 由脚本生成到 build/app.sct
    +-- Src/bled_app.c            # open("BLED") + 周期闪烁
```

两个工程都不再需要各自的 `.sct` 与 `app_config.c`：地址只在
`config/svcrt_partition.h` 维护，栈由内核按分区推导。

## 分区与加载（内核负责，本示例不写地址）

| 固件 | 所在分区 | 由谁加载 |
|------|----------|----------|
| 内核 | KERNEL | 复位向量直接运行 |
| BLED_DRV | IMAGE_POOL 开发槽位 2（驱动，单元 2） | 内核启动扫描 / 安装任务 |
| BLED_APP | IMAGE_POOL 开发槽位 3（App，单元 3） | 内核启动扫描 / 安装任务 |

> App 与驱动现在**共用一个统一镜像池** `IMAGE_POOL`（F427 上从 0x08040000 起 768KB，
> 按「分配单元 = 一个物理擦除扇区」分配，共 6 个 128KB 单元），
> 不再有 `DRIVER_POOL` / `APP_USER` 两块，也没有 `DRIVER_MAX_COUNT` / `APP_MAX_COUNT`。
> 本示例走的是**开发槽位表**（`config/svcrt_partition.h` 的 `SVCRT_DEV_SLOTn_*`，
> 当前 4 条：单元 0/1/2/3，类型依次为 驱动/App/驱动/App）：裸镜像没有镜像头，
> 扫描器读不出它的类型与占几个单元，只能由这张表显式声明。
> 两个工程各自在 Before Make 钩子里生成 `.sct`，槽位写进命令参数——
> `gen_app_sct.py --project bled_drv.uvprojx --type driver --dev-slot 2 --ram-size 4096`、
> `gen_app_sct.py --project bled_app.uvprojx --type app --dev-slot 3 --ram-size 8192`；
> 打包用 `tools/pack_app.py ... --dev-slot <n>`。
> **「尚未支持指定槽位、只能用 0 号槽」是早期工具的限制，已经过时。**

内核在启动时对镜像池做**镜像识别**（`svcrt_loader_scan`），识别方式二选一：

- **带 256 字节镜像头 + CRC 的 `.svcapp`**（安装路径，正式形态）；
- **裸镜像**（开发期直接烧录，需 `APP_ALLOW_RAW_IMAGE=1`）。

识别通过后按 `DRIVER_AUTO_START` / `APP_AUTO_START` 自动建任务启动。
运行期也可由应用通过 `svcrt_app_load/start/stop/status` 与
`svcrt_driver_load` 主动管理。

> 早期版本靠 `svcrt_register_tasks()` 里写死的 `0x08xxxxxx | 1u` 入口宏注册外部分区。
> 该写法已删除——**它是把地址抄第二遍**，正是要消灭的地雷。当前入口一律从分区表推导。

## 关键设计

### 1. 外部驱动直接操作硬件（bled_drv.c）

驱动用**裸寄存器地址**操作 PC2，不依赖任何芯片 HAL 头文件，保持 SDK 零依赖：

```c
#define GPIOC_BSRR  (*(volatile uint32 *)0x40020818u)
#define RCC_AHB1ENR (*(volatile uint32 *)0x40023830u)

RCC_AHB1ENR |= (1u << 2);              /* 使能 GPIOC 时钟 */
GPIOC_MODER  |= (0x1u << (2*2));       /* PC2 输出 */
GPIOC_BSRR = (1u << (16 + 2));         /* PC2 低 -> 蓝灯亮（共阳） */
```

驱动在 `DrvMain()` 中通过 SVC 0x14 注册设备：

```c
svcrt_drv_register("BLED", &bled_drv, 0);
```

> 注意：这里是**外设寄存器地址**（0x4002_xxxx），不是分区地址，不违反
> “地址只在 config 定义一次”的原则——分区地址才是需要唯一源头的东西。

### 2. 外部应用通过设备框架操作（bled_app.c）

应用**不碰硬件**，只通过设备 API：

```c
int32 bled = svcrt_dev_open("BLED", 0);   /* 打开蓝灯设备 */
svcrt_dev_write(bled, &on, 1);            /* 点亮（len>0） */
svcrt_dev_write(bled, &on, 0);            /* 熄灭（len==0） */
```

应用启动时轮询打开，等待驱动注册完成：

```c
do {
    bled = svcrt_dev_open("BLED", 0);
    if(bled < 0) svcrt_task_wait(100);
} while(bled < 0);
```

驱动任务优先级默认高于应用任务（`DRIVER_TASK_PRIORITY=9` < `APP_TASK_PRIORITY=10`，
数值越小优先级越高），保证 "BLED" 设备先注册完成。

### 3. 外部固件必须经 `__main` 初始化（踩坑要点）

外部固件启动汇编（`svcrt_drv_start.s` / `svcrt_app_start.s`）必须跳转到 C 库入口 `__main`：

```asm
    IMPORT  __main
    LDR R0, = __main
    BX  R0
```

**原因**：`bled_drv` 是带初值的全局结构体（`.data` 段），其 5 个函数指针的初值存在 Flash，
需由 C 运行时 scatter loading 拷贝到 RAM。若启动汇编直接 `LDR R0,=main; BLX R0` 跳过
`__main`，`bled_drv` 在 RAM 中是随机值，注册的驱动接口全是垃圾指针，App 调用
`svcrt_dev_write` 时跳到非法地址 -> HardFault / 复位。

> 本示例最初正是因为缺少这一步导致外部固件一运行就崩溃，改用 `__main` 后端到端打通。
>
> 与之配套：`gen_scatter.py` 生成的 App / 驱动 `.sct` 里 `ARM_LIB_STACK` 指向
> 内核推导出的同一栈顶，所以“保留 `__main` 初始化”和“独占一份栈”两者不冲突。

### 4. 故障围栏（本示例也能受益）

内核为每个 App 槽位与每个驱动槽位维护故障计数，连续崩溃达到
`APP_CRASH_RESTART_MAX`（默认 3）次后禁用该镜像，避免"崩溃 → 重启 → 再崩溃"死循环。
驱动崩溃同样受每槽的 `driver_crash_cnt` 保护（计数按槽位独立）。

## 编译与烧录

### 编译（各自独立）

1. Keil 打开 `BLED_DRV/MDK-ARM/bled_drv.uvprojx` -> Build
   （Before Make 自动生成 `build/driver.sct`）-> 产物 `bled_drv.axf`
2. Keil 打开 `BLED_APP/MDK-ARM/bled_app.uvprojx` -> Build -> 产物 `bled_app.axf`

### 打包

```bash
python tools/pack_app.py --project BLED_DRV/MDK-ARM/bled_drv.uvprojx \
       --type driver --name BLED_DRV --out bled_drv.svcapp
python tools/pack_app.py --project BLED_APP/MDK-ARM/bled_app.uvprojx \
       --type app    --name BLED_APP --out bled_app.svcapp
```

`pack_app.py` 会校验 ELF 的链接基址与 `config/svcrt_partition.h` 是否一致，
并算出 `.svcapp` 需要的 CRC，避免"烧错分区"这类低级事故。

### 烧录 / 安装

| 方式 | 做法 |
|------|------|
| 安装路径（推荐） | 通过内核安装任务（串口）依次下发 `bled_drv.svcapp`、`bled_app.svcapp`；内核按镜像头 `type` 落进统一镜像池（两者共用同一个池，类型只决定启动优先级与权限） |
| 开发期烧录 | 直接用下载器把 `bled_drv.bin`、`bled_app.bin` 烧到各自分区基址（基址见 `config/svcrt_partition.h`），内核按裸镜像识别 |

### 运行现象

- 内核启动 -> 扫描镜像池 -> BLED_DRV 注册 "BLED" 设备 -> 驱动任务运行
- 内核扫描 App 槽位 -> BLED_APP 打开 "BLED" -> 周期点亮 / 熄灭
- **现象**：RGB 灯珠**蓝色**以 0.8 秒节奏闪烁（同时红 / 绿由内核任务驱动）

## 数据流总结

App 的一次 `svcrt_dev_write(bled, &on, 1)` 完整路径：

```
App: svcrt_dev_write()
  -> SVC 0x10 (DEV_IO, 子功能 3=write)
  -> 内核 SVC 服务 -> svcrt_dev_write_internal()
  -> 查设备表找到 "BLED" -> 调用 bled_drv.drv_write()
  -> bled_drv_write() -> bled_set(1)
  -> GPIOC_BSRR = (1<<(16+2)) -> PC2 拉低 -> 蓝灯亮
```

整个过程跨越了**应用固件 -> 内核 -> 驱动固件 -> 硬件**四层，
体现 SVCrtOS 用户态 / 内核态分离与设备框架解耦的设计。
