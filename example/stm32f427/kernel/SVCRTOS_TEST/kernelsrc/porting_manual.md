# SVCrtOS 移植手册

## 1. 概述

SVCrtOS 是一个面向ARM Cortex-M系列MCU的实时操作系统，支持任务调度、事件驱动、内存保护（MPU）和设备驱动框架。通过 `svcrt_config.h` 配置文件，SVCrtOS 可以灵活地适配不同架构的MCU，包括不带MPU的Cortex-M3设备。

### 1.1 架构解耦设计

SVCrtOS 的内核代码与具体MCU完全解耦，所有硬件依赖通过 `svcrt_port.h` 移植接口抽象：

```
┌─────────────────────────────────┐
│         应用程序 (App SDK)       │
├─────────────────────────────────┤
│       SVCrtOS 内核 (src/)        │
├─────────────────────────────────┤
│    svcrt_port.h (移植接口层)     │  ← 唯一耦合点
├─────────────────────────────────┤
│   libcpu/arm/cortex-m?/         │  ← 架构实现
├─────────────────────────────────┤
│         目标MCU硬件              │
└─────────────────────────────────┘
```

### 1.2 支持的CPU架构

| 架构 | FPU | MPU | 典型MCU |
|------|-----|-----|---------|
| Cortex-M3 | - | - | STM32F1xx, LPC17xx |
| Cortex-M4 | ? | ? | STM32F4xx, Kinetis K |
| Cortex-M7 | ? | ? | STM32F7xx, STM32H7xx |

---

## 2. 目录结构

```
kernelsrc/
├── include/                    # 内核头文件
│   ├── svcrt.h                 # 应用层API（App SDK入口）
│   ├── svcrt_config.h          # ★ 集中配置文件（功能开关）
│   ├── svcrt_port.h            # ★ 移植接口（需用户实现）
│   ├── svcrt_types.h           # 基础类型定义
│   ├── svcrt_def.h             # 内核公共定义（句柄宏/SVC编号）
│   ├── svcrt_task.h            # 任务管理模块（内核内部）
│   ├── svcrt_event.h           # 事件模块（内核内部）
│   ├── svcrt_fifo.h            # FIFO模块（内核内部）
│   ├── svcrt_mpu.h             # MPU模块（内核内部，条件编译）
│   ├── svcrt_dev.h             # 设备驱动框架（内核内部）
│   └── svcrt_cfg.h             # 任务配置加载（内核内部）
├── src/                        # 内核源文件
│   ├── svcrt_init.c            # 系统启动与初始化
│   ├── svcrt_task.c            # 任务管理与调度器
│   ├── svcrt_event.c           # 事件模块实现
│   ├── svcrt_fifo.c            # FIFO环形缓冲实现
│   ├── svcrt_mpu.c             # MPU实现（条件编译）
│   ├── svcrt_dev.c             # 设备驱动框架实现
│   └── svcrt_cfg.c             # 任务配置加载
├── libcpu/                     # CPU架构实现
│   └── arm/
│       ├── cortex-m3/          # M3移植（无FPU/MPU）
│       │   ├── context_rvds.S  # 上下文切换（无浮点）
│       │   └── cpuport.c       # M3移植层实现
│       └── cortex-m4/          # M4移植（有FPU/MPU）
│           ├── context_rvds.S  # 上下文切换（含浮点）
│           └── cpuport.c       # M4移植层实现
├── drivers/                    # 内置驱动示例
│   ├── devsio.c                # 板级设备注册表
│   ├── drvuart.c/h             # UART驱动
│   └── drvled.c/h              # LED驱动
├── app/                        # 应用模板
│   ├── appconfig.c             # 应用分区配置
│   ├── appstart.s              # 应用启动汇编
│   └── oslib.c                 # OS接口封装
└── sdk/                        # SDK开发包
    ├── app_sdk/                # App SDK
    │   ├── svcrt.h             # 应用API头文件
    │   ├── svcrt_types.h       # 基础类型
    │   ├── svcrt_app_config.h  # 应用配置
    │   ├── svcrt_app_main.c    # 应用入口模板
    │   ├── svcrt_oslib.c       # SVC接口封装
    │   ├── svcrt_app_start.s   # 应用启动汇编
    │   └── examples/           # 示例应用
    └── driver_sdk/             # Driver SDK
        ├── svcrt_driver_sdk.h  # 驱动开发接口
        ├── svcrt_driver_bridge.c # 驱动注册桥接
        └── examples/           # 示例驱动
```

---

## 3. 移植步骤

### 3.1 第一步：修改配置文件 `svcrt_config.h`

根据目标MCU修改配置文件中的关键参数：

```c
// 选择CPU架构
#define SVCRT_CPU_ARCH    SVCRT_ARCH_CORTEX_M3  // M3设备选此项

// 以下会自动适配：
// - M3: SVCRT_USE_FPU=0, SVCRT_USE_MPU=0, SVCRT_USE_PRIV=0
// - M4: SVCRT_USE_FPU=1, SVCRT_USE_MPU=1, SVCRT_USE_PRIV=1
```

**Cortex-M3 典型配置：**
```c
#define SVCRT_CPU_ARCH         SVCRT_ARCH_CORTEX_M3
#define SVCRT_TASK_MAX_NUM     (5)
#define SVCRT_TICK_PERIOD_US   (1000)
#define SVCRT_EVENT_NUM        (8)
#define SVCRT_DEV_MAX_NUM      (6)
```

**Cortex-M4 典型配置：**
```c
#define SVCRT_CPU_ARCH         SVCRT_ARCH_CORTEX_M4
#define SVCRT_TASK_MAX_NUM     (7)
#define SVCRT_TICK_PERIOD_US   (500)
#define SVCRT_EVENT_NUM        (10)
#define SVCRT_DEV_MAX_NUM      (8)
```

### 3.2 第二步：实现移植接口 `svcrt_port.h`

将 `svcrt_port.h` 复制到用户工程目录，修改以下内容：

#### 3.2.1 包含MCU头文件
```c
// STM32F1xx
#include "stm32f10x.h"

// STM32F4xx
#include "stm32f4xx.h"

// 其他MCU
#include "your_mcu_header.h"
```

#### 3.2.2 实现移植回调函数

**必须实现的函数：**

| 函数 | 说明 |
|------|------|
| `svcrt_port_board_init()` | 硬件板卡初始化（FPU使能等） |
| `svcrt_port_irq_init()` | NVIC优先级配置 |
| `svcrt_port_start_timer()` | 启动SysTick定时器 |
| `svcrt_port_enable_fpu()` | FPU使能（仅M4/M7需要） |
| `svcrt_port_set_idle_mpu()` | 后台任务MPU设置（仅M4/M7需要） |

**Cortex-M3 实现示例：**
```c
void svcrt_port_board_init(void)
{
    // M3无需FPU初始化
}

void svcrt_port_irq_init(void)
{
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_4);
    NVIC_SetPriority(PendSV_IRQn, 0xFF);
    NVIC_SetPriority(SysTick_IRQn, 0x00);
}

void svcrt_port_start_timer(uint32 tick_period_us)
{
    uint32 ticks = SystemCoreClock / 1000000 * tick_period_us / 8;
    SysTick_Config(ticks);
}

void svcrt_port_enable_fpu(void) {}
void svcrt_port_set_idle_mpu(uint32 a, uint32 b, uint32 c) {}
```

**Cortex-M4 实现示例：**
```c
void svcrt_port_board_init(void)
{
    SCB->CPACR |= (3 << 20) | (3 << 22);  // 使能CP10/CP11
}

void svcrt_port_irq_init(void)
{
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_4);
    NVIC_SetPriority(PendSV_IRQn, 0xFF);
    NVIC_SetPriority(SysTick_IRQn, 0x00);
    NVIC_SetPriority(SVCall_IRQn, 0x01);
}

void svcrt_port_start_timer(uint32 tick_period_us)
{
    uint32 ticks = SystemCoreClock / 1000000 * tick_period_us / 8;
    SysTick_Config(ticks);
}

void svcrt_port_enable_fpu(void)
{
    FPU->FPCCR = FPU_FPCCR_ASPEN_Msk | FPU_FPCCR_LSPEN_Msk;
}

void svcrt_port_set_idle_mpu(uint32 task_func, uint32 stack_addr, uint32 stack_size)
{
    svcrt_mpu_set(task_func, 0x1000, stack_addr, stack_size);
}
```

### 3.3 第三步：选择上下文切换文件

根据CPU架构选择对应的汇编文件：

| 架构 | 汇编文件 |
|------|----------|
| Cortex-M3 | `libcpu/arm/cortex-m3/context_rvds.S` |
| Cortex-M4 | `libcpu/arm/cortex-m4/context_rvds.S` |

**注意：** M4汇编文件通过 `IF :DEF:SVCRT_USE_FPU` 条件汇编控制浮点上下文保存。编译时需定义 `SVCRT_USE_FPU` 预处理宏。

### 3.4 第四步：配置工程

1. 添加内核源文件到工程（`src/` 目录下所有 `.c` 文件）
2. 添加对应架构的 `cpuport.c` 和 `context_rvds.S`
3. 添加头文件搜索路径 `include/`
4. 根据配置定义预处理宏：
   - M3: 无需额外宏
   - M4: 定义 `SVCRT_USE_FPU`

---

## 3.5 MDK (Keil) 工程配置详解

### 3.5.1 需要添加的源文件

在 MDK 工程中创建以下分组（Group），将对应文件添加进去：

**Group: Kernel（内核源码）**

| 文件路径 | 说明 |
|----------|------|
| `src/svcrt_init.c` | 系统启动与初始化 |
| `src/svcrt_task.c` | 任务管理与调度器 + SVC 分发 |
| `src/svcrt_event.c` | 事件模块 |
| `src/svcrt_fifo.c` | FIFO 环形缓冲 |
| `src/svcrt_dev.c` | 设备驱动框架（统一设备表） |
| `src/svcrt_cfg.c` | 任务配置加载 |
| `src/svcrt_mpu.c` | MPU 内存保护（仅 M4/M7，条件编译） |

**Group: Port（移植层）**

| 文件路径 | 说明 |
|----------|------|
| `libcpu/arm/cortex-m4/cpuport.c` | M4 移植层（或 `cortex-m3/` 下对应文件） |
| `libcpu/arm/cortex-m4/context_rvds.S` | M4 上下文切换汇编（或 `cortex-m3/` 下对应文件） |

**Group: Drivers（板级驱动）**

| 文件路径 | 说明 |
|----------|------|
| `drivers/devsio.c` | 板载设备注册（调用 `svcrt_dev_register()`） |
| `drivers/drvuart.c` | UART 驱动（按需添加） |
| `drivers/drvled.c` | LED 驱动（按需添加） |

**Group: App（应用模板）**

| 文件路径 | 说明 |
|----------|------|
| `app/appconfig.c` | 分区资源配置 |
| `app/oslib.c` | 应用层 SVC 接口封装 |
| `app/appstart.s` | 应用启动汇编入口 |

### 3.5.2 头文件搜索路径（Include Paths）

在 MDK 的 Options → C/C++ → Include Paths 中添加：

```
kernelsrc\include
```

如果使用自定义的 `svcrt_port.h`（放在工程目录中），还需添加该文件所在目录。

### 3.5.3 预处理宏定义（Preprocessor Defines）

在 MDK 的 Options → C/C++ → Preprocessor Symbols → Define 中添加：

**Cortex-M4 工程：**
```
SVCRT_USE_FPU
```

**Cortex-M3 工程：**
```
（无需额外宏）
```

### 3.5.4 汇编器设置

- `context_rvds.S` 需要使用 ARM 汇编器（ARMASM），MDK 默认支持
- 确保汇编器中也添加了相同的预处理宏（Options → Asm → Preprocessor Symbols）
- M4 工程汇编器 Define 中也需添加 `SVCRT_USE_FPU`

### 3.5.5 链接器注意事项

- 内核工程需要确保 `main()` 函数位于 Flash 起始地址
- 各应用分区的代码段需要与 `appconfig.c` 中的 `rom_start` 对应
- 共享内存区域 `SVCRT_SHARE_MEM_ADDR` 需要在链接脚本中预留

### 3.5.6 快速检查清单

移植到新工程时，按此清单逐项确认：

- [ ] `svcrt_config.h` 中 `SVCRT_CPU_ARCH` 已选择正确架构
- [ ] `svcrt_port.h` 已包含正确的 MCU 头文件
- [ ] `svcrt_port.h` 中所有移植回调函数已实现
- [ ] 选择了正确架构的 `context_rvds.S` 和 `cpuport.c`
- [ ] MDK 工程中添加了所有 `src/*.c` 文件
- [ ] MDK 工程中添加了 `devsio.c` 和所需驱动
- [ ] MDK 工程中添加了 `app/` 目录文件
- [ ] Include Paths 包含 `include/`
- [ ] M4 工程已定义 `SVCRT_USE_FPU` 预处理宏（C 和汇编均需）
- [ ] `devsio.c` 中注册了板载设备
- [ ] `appconfig.c` 中分区地址与链接脚本匹配

---

## 4. 功能开关详解

### 4.1 svcrt_config.h 配置项

| 配置项 | 默认值 | 说明 |
|--------|--------|------|
| `SVCRT_CPU_ARCH` | `SVCRT_ARCH_CORTEX_M4` | CPU架构选择 |
| `SVCRT_USE_FPU` | 自动 | 浮点单元开关 |
| `SVCRT_USE_MPU` | 自动 | 内存保护单元开关 |
| `SVCRT_USE_PRIV` | 自动 | 特权模式分离开关 |
| `SVCRT_TASK_MAX_NUM` | 7 | 最大任务数 |
| `SVCRT_TICK_PERIOD_US` | 500 | 滴答周期(微秒) |
| `SVCRT_EVENT_NUM` | 10 | 最大事件数 |
| `SVCRT_MAX_EVENT_WAITERS` | 4 | 每事件最大等待任务数 |
| `SVCRT_DEV_MAX_NUM` | 8 | 最大设备驱动数 |
| `SVCRT_USE_CPU_LOAD` | 1 | CPU负载统计开关 |
| `SVCRT_USE_STACK_CHECK` | 1 | 栈溢出检测开关 |

### 4.2 条件编译效果

当 `SVCRT_USE_MPU=0` 时：
- `svcrt_mpu.c` 整个文件不会被编译
- `svcrt_mpu_module_init()` / `svcrt_mpu_set_app()` 等展开为空宏，零开销
- `svcrt_task_t` 中不包含 `mpu_bar[8]` / `mpu_asr[8]` 字段，节省RAM
- `svcrt_init.c` 中不调用MPU初始化和MPU保护设置

当 `SVCRT_USE_FPU=0` 时：
- 异常上下文中不包含浮点寄存器字段
- 上下文切换汇编不保存/恢复浮点寄存器
- 任务栈空间需求大幅减少

---

## 5. Cortex-M3 移植注意事项

### 5.1 无MPU时的行为

- 任务之间**没有**内存隔离保护
- 任何任务都可以访问全部RAM空间
- HardFault 仍然会杀死出错任务
- `SVCRT_USE_PRIV` 自动关闭，所有任务运行在相同特权级

### 5.2 无FPU时的行为

- 上下文切换更快（少保存16个浮点寄存器+FPSCR）
- 任务栈需求减少约68字节
- 不支持浮点运算（使用软浮点库）

### 5.3 资源占用对比

| 项目 | Cortex-M3 | Cortex-M4 |
|------|-----------|-----------|
| 单任务栈开销 | ~40字节 | ~108字节 |
| 上下文切换时间 | ~30周期 | ~60周期 |
| 内核RAM占用 | 较少 | 较多（含MPU字段） |

---

## 6. 驱动框架

SVCrtOS 的设备驱动框架统一管理**内置驱动**和**可安装驱动**，两者共用同一套设备表，通过 `svcrt_dev_register()` 注册。

### 6.1 两种驱动形态

| 类型 | 注册时机 | 编译方式 | 头文件 |
|------|----------|----------|--------|
| 内置驱动 | `svcrt_dev_board_init()` 中注册 | 与内核一起编译 | `svcrt_dev.h` |
| 可安装驱动 | 运行时动态注册 | 独立编译 | `svcrt_driver_sdk.h` |

两种驱动实现相同的 `svcrt_dev_drv_t` 接口（5个函数：open/close/read/write/ctrl），结构完全兼容。

### 6.2 内置驱动注册

在 `drivers/devsio.c` 的 `svcrt_dev_board_init()` 中注册板载驱动：

```c
void svcrt_dev_board_init(void)
{
    svcrt_dev_register("COM1", &usart_drv, UART_DEV_COM1);
    svcrt_dev_register("LED",  &led_drv,  0);
}
```

此函数在 `svcrt_init.c` 的 `svcrt_kernel_init()` 中自动调用。

### 6.3 可安装驱动注册（Driver SDK）

驱动开发者使用 `svcrt_driver_sdk.h`，独立编译后通过桥接层注册：

```c
#include "svcrt_driver_sdk.h"

static svcrt_dev_drv_t my_drv = {
    my_open, my_close, my_read, my_write, my_ctrl
};

void my_driver_init(void)
{
    svcrt_drv_register("MYDEV", &my_drv, 0);
}
```

### 6.4 注销驱动

```c
svcrt_drv_unregister("MYDEV");
```

---

## 7. App SDK使用

### 7.1 开发可安装应用

应用开发者只需实现 `AppMain()` 函数：

```c
#include "svcrt.h"

void AppMain(void)
{
    int32 led = svcrt_dev_open("LED", 0);

    while(1)
    {
        uint8 val = 1;
        svcrt_dev_write(led, &val, 1);
        svcrt_task_wait(500);

        val = 0;
        svcrt_dev_write(led, &val, 1);
        svcrt_task_wait(500);
    }
}
```

### 7.2 App SDK 文件清单

| 文件 | 说明 |
|------|------|
| `svcrt.h` | 应用API入口 |
| `svcrt_types.h` | 基础类型 |
| `svcrt_app_config.h` | 分区配置 |
| `svcrt_oslib.c` | SVC接口封装 |
| `svcrt_app_main.c` | 入口模板 |
| `svcrt_app_start.s` | 启动汇编 |
