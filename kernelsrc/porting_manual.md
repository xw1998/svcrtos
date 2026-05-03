# AnOs 移植手册

## 1. 概述

AnOs 是一个面向ARM Cortex-M系列MCU的实时操作系统，支持任务调度、事件驱动、内存保护（MPU）和设备驱动框架。通过 `anOs_config.h` 配置文件，AnOs 可以灵活地适配不同架构的MCU，包括不带MPU的Cortex-M3设备。

### 1.1 架构解耦设计

AnOs 的内核代码与具体MCU完全解耦，所有硬件依赖通过 `anOs_port.h` 移植接口抽象：

```
┌─────────────────────────────────┐
│         应用程序 (App SDK)       │
├─────────────────────────────────┤
│         AnOs 内核 (src/)         │
├─────────────────────────────────┤
│    anOs_port.h (移植接口层)      │  ← 唯一耦合点
├─────────────────────────────────┤
│   libcpu/arm/cortex-m?/         │  ← 架构实现
├─────────────────────────────────┤
│         目标MCU硬件              │
└─────────────────────────────────┘
```

### 1.2 支持的CPU架构

| 架构 | FPU | MPU | 典型MCU |
|------|-----|-----|---------|
| Cortex-M3 | ? | ? | STM32F1xx, LPC17xx |
| Cortex-M4 | ? | ? | STM32F4xx, Kinetis K |
| Cortex-M7 | ? | ? | STM32F7xx, STM32H7xx |

---

## 2. 目录结构

```
kernelsrc/
├── include/                    # 内核头文件
│   ├── anOs.h                  # 应用层API（App SDK入口）
│   ├── anOs_config.h           # ★ 集中配置文件（功能开关）
│   ├── anOs_port.h             # ★ 移植接口（需用户实现）
│   ├── anOs_types.h            # 基础类型定义
│   ├── kernel.h                # 内核核心定义
│   ├── kerOs.h                 # OS基础定义
│   ├── kerevent.h              # 事件模块
│   ├── kermpu.h                # MPU模块（条件编译）
│   ├── devsio.h                # 设备IO框架
│   ├── osconfigure.h           # 任务配置接口
│   └── appconfig.h             # 应用分区配置
├── src/                        # 内核源文件
│   ├── kernel.c                # 调度器与SVC服务
│   ├── kerevent.c              # 事件模块实现
│   ├── kermpu.c                # MPU实现（条件编译）
│   ├── devsio.c                # 设备IO框架实现
│   ├── osconfigure.c           # 任务配置
│   └── boot.c                  # 系统启动
├── libcpu/                     # CPU架构实现
│   └── arm/
│       ├── cortex-m3/          # M3移植（无FPU/MPU）
│       │   ├── context_rvds.S  # 上下文切换（无浮点）
│       │   └── cpuport.c       # M3移植层实现
│       └── cortex-m4/          # M4移植（有FPU/MPU）
│           ├── context_rvds.S  # 上下文切换（含浮点）
│           └── cpuport.c       # M4移植层实现
├── drivers/                    # 内置驱动示例
│   ├── drvuart.c/h             # UART驱动
│   └── drvled.c/h              # LED驱动
├── app/                        # 应用模板
│   ├── appconfig.c             # 应用分区配置
│   ├── appstart.s              # 应用启动汇编
│   └── oslib.c                 # OS接口封装
└── sdk/                        # SDK开发包
    ├── app_sdk/                # App SDK
    │   ├── anOs.h              # 应用API头文件
    │   ├── anOs_types.h        # 基础类型
    │   ├── anOs_app_config.h   # 应用配置
    │   ├── anOs_app_main.c     # 应用入口模板
    │   ├── anOs_oslib.c        # SVC接口封装
    │   ├── anOs_app_start.s    # 应用启动汇编
    │   └── examples/           # 示例应用
    └── driver_sdk/             # Driver SDK
        ├── anOs_driver_sdk.h   # 驱动开发接口
        ├── anOs_driver_bridge.c # 驱动注册桥接
        └── examples/           # 示例驱动
```

---

## 3. 移植步骤

### 3.1 第一步：修改配置文件 `anOs_config.h`

根据目标MCU修改配置文件中的关键参数：

```c
// 选择CPU架构
#define ANOS_CPU_ARCH    ANOS_ARCH_CORTEX_M3  // M3设备选此项

// 以下会自动适配：
// - M3: ANOS_USE_FPU=0, ANOS_USE_MPU=0, ANOS_USE_PRIV=0
// - M4: ANOS_USE_FPU=1, ANOS_USE_MPU=1, ANOS_USE_PRIV=1
```

**Cortex-M3 典型配置：**
```c
#define ANOS_CPU_ARCH         ANOS_ARCH_CORTEX_M3
#define ANOS_TASK_MAX_NUM     (5)
#define ANOS_TICK_PERIOD_US   (1000)
#define ANOS_EVENT_NUM        (8)
#define ANOS_DEV_MAX_NUM      (6)
```

**Cortex-M4 典型配置：**
```c
#define ANOS_CPU_ARCH         ANOS_ARCH_CORTEX_M4
#define ANOS_TASK_MAX_NUM     (7)
#define ANOS_TICK_PERIOD_US   (500)
#define ANOS_EVENT_NUM        (10)
#define ANOS_DEV_MAX_NUM      (8)
```

### 3.2 第二步：实现移植接口 `anOs_port.h`

将 `anOs_port.h` 复制到用户工程目录，修改以下内容：

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
| `anOsPortBoardInit()` | 硬件板卡初始化（FPU使能等） |
| `anOsPortIrqInit()` | NVIC优先级配置 |
| `anOsPortStartTimer()` | 启动SysTick定时器 |
| `anOsPortEnableFpu()` | FPU使能（仅M4/M7需要） |
| `anOsPortSetIdleMpu()` | 后台任务MPU设置（仅M4/M7需要） |

**Cortex-M3 实现示例：**
```c
void anOsPortBoardInit(void)
{
    // M3无需FPU初始化
}

void anOsPortIrqInit(void)
{
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_4);
    NVIC_SetPriority(PendSV_IRQn, 0xFF);
    NVIC_SetPriority(SysTick_IRQn, 0x00);
}

void anOsPortStartTimer(uint32 tick_period_us)
{
    uint32 ticks = SystemCoreClock / 1000000 * tick_period_us / 8;
    SysTick_Config(ticks);
}

void anOsPortEnableFpu(void) {}
void anOsPortSetIdleMpu(uint32 a, uint32 b, uint32 c) {}
```

**Cortex-M4 实现示例：**
```c
void anOsPortBoardInit(void)
{
    SCB->CPACR |= (3 << 20) | (3 << 22);  // 使能CP10/CP11
}

void anOsPortIrqInit(void)
{
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_4);
    NVIC_SetPriority(PendSV_IRQn, 0xFF);
    NVIC_SetPriority(SysTick_IRQn, 0x00);
    NVIC_SetPriority(SVCall_IRQn, 0x01);
}

void anOsPortStartTimer(uint32 tick_period_us)
{
    uint32 ticks = SystemCoreClock / 1000000 * tick_period_us / 8;
    SysTick_Config(ticks);
}

void anOsPortEnableFpu(void)
{
    FPU->FPCCR = FPU_FPCCR_ASPEN_Msk | FPU_FPCCR_LSPEN_Msk;
}

void anOsPortSetIdleMpu(uint32 task_func, uint32 stack_addr, uint32 stack_size)
{
    kerMpuSet(task_func, 0x1000, stack_addr, stack_size);
}
```

### 3.3 第三步：选择上下文切换文件

根据CPU架构选择对应的汇编文件：

| 架构 | 汇编文件 |
|------|----------|
| Cortex-M3 | `libcpu/arm/cortex-m3/context_rvds.S` |
| Cortex-M4 | `libcpu/arm/cortex-m4/context_rvds.S` |

**注意：** M4汇编文件通过 `IF :DEF:ANOS_USE_FPU` 条件汇编控制浮点上下文保存。编译时需定义 `ANOS_USE_FPU` 预处理宏。

### 3.4 第四步：配置工程

1. 添加内核源文件到工程（`src/` 目录下所有 `.c` 文件）
2. 添加对应架构的 `cpuport.c` 和 `context_rvds.S`
3. 添加头文件搜索路径 `include/`
4. 根据配置定义预处理宏：
   - M3: 无需额外宏
   - M4: 定义 `ANOS_USE_FPU`

---

## 4. 功能开关详解

### 4.1 anOs_config.h 配置项

| 配置项 | 默认值 | 说明 |
|--------|--------|------|
| `ANOS_CPU_ARCH` | `ANOS_ARCH_CORTEX_M4` | CPU架构选择 |
| `ANOS_USE_FPU` | 自动 | 浮点单元开关 |
| `ANOS_USE_MPU` | 自动 | 内存保护单元开关 |
| `ANOS_USE_PRIV` | 自动 | 特权模式分离开关 |
| `ANOS_TASK_MAX_NUM` | 7 | 最大任务数 |
| `ANOS_TICK_PERIOD_US` | 500 | 滴答周期(微秒) |
| `ANOS_EVENT_NUM` | 10 | 最大事件数 |
| `ANOS_MAX_EVENT_WAITERS` | 4 | 每事件最大等待任务数 |
| `ANOS_DEV_MAX_NUM` | 8 | 最大设备驱动数 |
| `ANOS_USE_CPU_LOAD` | 1 | CPU负载统计开关 |
| `ANOS_USE_STACK_CHECK` | 1 | 栈溢出检测开关 |

### 4.2 条件编译效果

当 `ANOS_USE_MPU=0` 时：
- `kermpu.c` 整个文件不会被编译
- `kerMpuMdlInit()` / `kerSetAppMpu()` 等展开为空宏，零开销
- `TASK_CONTEXT` 中不包含 `mpu_bar[8]` / `mpu_asr[8]` 字段，节省RAM
- `boot.c` 中不调用MPU初始化和MPU保护设置

当 `ANOS_USE_FPU=0` 时：
- `EXCEPTION_CONTEXT` 中不包含浮点寄存器字段
- 上下文切换汇编不保存/恢复浮点寄存器
- 任务栈空间需求大幅减少

---

## 5. Cortex-M3 移植注意事项

### 5.1 无MPU时的行为

- 任务之间**没有**内存隔离保护
- 任何任务都可以访问全部RAM空间
- HardFault 仍然会杀死出错任务
- `ANOS_USE_PRIV` 自动关闭，所有任务运行在相同特权级

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

## 6. 驱动SDK使用

### 6.1 注册外部驱动

驱动开发者只需实现 `ANOS_DRV_INTERFACE` 并调用注册函数：

```c
#include "anOs_driver_sdk.h"

// 实现驱动接口
static ANOS_DRV_INTERFACE my_drv = {
    my_open, my_close, my_read, my_write, my_ctrl
};

// 在初始化时注册
void my_driver_init(void)
{
    anOsDrvRegister("MYDEV", &my_drv, 0);
}
```

### 6.2 注销驱动

```c
anOsDrvUnregister("MYDEV");
```

---

## 7. App SDK使用

### 7.1 开发可安装应用

应用开发者只需实现 `AppMain()` 函数：

```c
#include "anOs.h"

void AppMain(void)
{
    int32 led = dev_open("LED", 0);

    while(1)
    {
        uint8 val = 1;
        dev_write(led, &val, 1);
        TaskWait(500);

        val = 0;
        dev_write(led, &val, 1);
        TaskWait(500);
    }
}
```

### 7.2 App SDK 文件清单

| 文件 | 说明 |
|------|------|
| `anOs.h` | 应用API入口 |
| `anOs_types.h` | 基础类型 |
| `anOs_app_config.h` | 分区配置 |
| `anOs_oslib.c` | SVC接口封装 |
| `anOs_app_main.c` | 入口模板 |
| `anOs_app_start.s` | 启动汇编 |
