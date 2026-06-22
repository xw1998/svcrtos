# SVCrtOS 端到端示例：外部 Driver + 外部 App 操作蓝灯

本示例演示 SVCrtOS 的完整分态架构：**外部用户态驱动**注册硬件设备，**外部用户态应用**通过设备框架操作该硬件，三者（内核 + 驱动 + 应用）以独立固件形式协作。

## 架构链路

```
+------------------+   svcrt_dev_open("BLED")    +------------------+
|   BLED_APP        | --------------------------> |   SVCrtOS 内核    |
| (用户态应用固件)  |   svcrt_dev_write(...)       |   设备框架        |
|  0x08080000       | <-------------------------- |   (设备表/句柄)   |
+------------------+                              +--------+---------+
                                                            | 调用驱动接口
                                              SVC 0x14 注册  |
+------------------+   svcrt_drv_register("BLED")          v
|   BLED_DRV        | --------------------------> +------------------+
| (用户态驱动固件)  |                              |  bled_drv 接口    |
|  0x08060000       |   直接操作 PC2 寄存器 ----->|  GPIOC->BSRR      |
+------------------+                              +------------------+
                                                            |
                                                            v
                                                      RGB 灯珠蓝色(PC2)
```

## 文件组成

```
example/stm32f427/
+-- driver_sdk/BLED_DRV/          # 外部蓝灯驱动
|   +-- MDK-ARM/
|   |   +-- bled_drv.uvprojx
|   |   +-- bled_drv.sct          # ROM 0x08060000, RAM 0x2001A000
|   +-- Src/bled_drv.c            # 裸寄存器驱动 PC2，注册 "BLED"
|
+-- app_sdk/BLED_APP/             # 外部蓝灯应用
    +-- MDK-ARM/
    |   +-- bled_app.uvprojx
    |   +-- bled_app.sct          # ROM 0x08080000, RAM 0x2001C000
    +-- Src/
        +-- bled_app.c            # open("BLED") + 周期闪烁
        +-- app_config.c          # 分区配置
```

## 分区地址表

| 固件 | ROM | RAM | 说明 |
|------|-----|-----|------|
| 内核 | 0x08000000 | 0x20000000~ | 含板级 RGB 红/绿驱动 |
| BLED_DRV | 0x08060000 | 0x2001A000 | 蓝灯驱动（注册 BLED） |
| BLED_APP | 0x08080000 | 0x2001C000 | 蓝灯应用（操作 BLED） |

> 各分区 ROM/RAM 互不重叠。蓝灯驱动直接操作 PC2 硬件寄存器
> （本工程 MPU=0，用户态可访问外设）。

## 关键设计

### 1. 外部驱动直接操作硬件（bled_drv.c）

驱动用**裸寄存器地址**操作 PC2，不依赖任何芯片 HAL 头文件，保持 SDK 零依赖：

```c
#define GPIOC_BSRR  (*(volatile uint32 *)0x40020818u)
#define RCC_AHB1ENR (*(volatile uint32 *)0x40023830u)

RCC_AHB1ENR |= (1u << 2);              // 使能 GPIOC 时钟
GPIOC_MODER  |= (0x1u << (2*2));       // PC2 输出
GPIOC_BSRR = (1u << (16 + 2));         // PC2 低 -> 蓝灯亮（共阳）
```

驱动在 `DrvMain()` 中通过 SVC 0x14 注册设备：
```c
svcrt_drv_register("BLED", &bled_drv, 0);
```

### 2. 外部应用通过设备框架操作（bled_app.c）

应用**不碰硬件**，只通过设备 API：
```c
int32 bled = svcrt_dev_open("BLED", 0);   // 打开蓝灯设备
svcrt_dev_write(bled, &on, 1);            // 点亮（len>0）
svcrt_dev_write(bled, &on, 0);            // 熄灭（len==0）
```

应用启动时轮询打开，等待驱动注册完成：
```c
do {
    bled = svcrt_dev_open("BLED", 0);
    if(bled < 0) svcrt_task_wait(100);
} while(bled < 0);
```

### 3. 内核注册外部分区为任务（关键）

内核不会自动加载 ROM 里的外部固件，必须在 `svcrt_register_tasks` 中把外部分区入口地址注册为任务：

```c
#define BLED_DRV_ENTRY  (0x08060000u | 1u)   /* 驱动分区入口 */
#define BLED_APP_ENTRY  (0x08080000u | 1u)   /* 应用分区入口 */

svcrt_task_stack_init(p_task, (void (*)(void))BLED_DRV_ENTRY,
                      ext_drv_stack, sizeof(ext_drv_stack));
```

驱动任务优先级高于应用，保证 "BLED" 设备先于 App 打开前注册。

### 4. 外部固件必须经 `__main` 初始化（踩坑要点）

外部固件启动汇编（`svcrt_drv_start.s`/`svcrt_app_start.s`）必须跳转到 C 库入口 `__main`：

```asm
    IMPORT  __main
    LDR R0, = __main
    BX  R0
```

**原因**：`bled_drv` 是带初值的全局结构体（.data 段），其 5 个函数指针的初值存在 Flash，需由 C 运行时 scatter loading 拷贝到 RAM。若启动汇编直接 `LDR R0,=main; BLX R0` 跳过 `__main`，`bled_drv` 在 RAM 中是随机值，注册的驱动接口全是垃圾指针，App 调用 `svcrt_dev_write` 时跳到非法地址 -> HardFault / 复位。

> 本示例最初正是因为缺少这一步导致外部固件一运行就崩溃，改用 `__main` 后端到端打通。

## 编译与烧录

### 编译（各自独立）

1. Keil 打开 `BLED_DRV/MDK-ARM/bled_drv.uvprojx` -> 编译 -> fromelf 生成 `bled_drv.bin`
2. Keil 打开 `BLED_APP/MDK-ARM/bled_app.uvprojx` -> 编译 -> fromelf 生成 `bled_app.bin`

### 烧录顺序

| 顺序 | 固件 | 地址 |
|------|------|------|
| 1 | 内核 SVCRTOS_TEST | 0x08000000 |
| 2 | bled_drv.bin | 0x08060000 |
| 3 | bled_app.bin | 0x08080000 |

### 运行现象

- 内核启动 -> 加载驱动分区 -> BLED_DRV 注册 "BLED" 设备
- 内核加载应用分区 -> BLED_APP 打开 "BLED" -> 周期点亮/熄灭
- **现象**：RGB 灯珠**蓝色**以 0.8 秒节奏闪烁（同时红/绿由内核任务驱动）

## 数据流总结

App 的一次 `svcrt_dev_write(bled, &on, 1)` 完整路径：

```
App: svcrt_dev_write()
  -> SVC 0x10 (DEV_IO, 子功能3=write)
  -> 内核 SVC_Server -> svcrt_dev_write_internal()
  -> 查设备表找到 "BLED" -> 调用 bled_drv.drv_write()
  -> bled_drv_write() -> bled_set(1)
  -> GPIOC_BSRR = (1<<(16+2)) -> PC2 拉低 -> 蓝灯亮
```

整个过程跨越了**应用固件 -> 内核 -> 驱动固件 -> 硬件**四层，体现 SVCrtOS 用户态/内核态分离与设备框架解耦的设计。