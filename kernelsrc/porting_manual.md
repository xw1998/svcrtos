# SVCrtOS 移植手册

## 1. 架构概述

SVCrtOS 采用与 RT-Thread 类似的分层解耦架构，内核代码（kernelsrc/）与具体芯片完全分离。移植到新芯片时，只需在 `board/` 目录下创建新的移植目录，内核代码无需任何修改。

### 1.1 分层架构图

```
┌──────────────────────────────────────────────────────────────┐
│            应用层 (App SDK)                                   │
├──────────────────────────────────────────────────────────────┤
│         SVCrtOS 内核 (kernelsrc/)         │ ← 纯 C，无芯片依赖│
│    src/ + include/ 纯C代码，不包含任何芯片头文件              │
├──────────────────────────────────────────────────────────────┤
│     svcrt_port.h (硬件抽象接口)            │ ← 纯函数声明     │
├──────────────────────────────────────────────────────────────┤
│      board/<芯片>/ (板级移植层)            │ ← 芯片相关实现   │
│   svcrt_board.c / svcrt_mpu.c / 中断入口 / 驱动             │
├──────────────────────────────────────────────────────────────┤
│            具体MCU硬件                                        │
└──────────────────────────────────────────────────────────────┘
```

**解耦原则：**
- `kernelsrc/` 中的代码不包含任何芯片头文件（如 `stm32f4xx.h`）
- `kernelsrc/` 中的代码不直接操作任何硬件寄存器
- 所有硬件访问均通过 `svcrt_port.h` 中声明的函数抽象
- 中断服务程序（SysTick_Handler、HardFault_Handler）在 `board/` 层实现
- 移植到新芯片只需创建 `board/<芯片>/` 目录

### 1.2 支持的CPU架构

| 架构 | FPU | MPU | 典型MCU |
|------|-----|-----|---------|
| Cortex-M3 | - | - | STM32F1xx, LPC17xx |
| Cortex-M4 | 有 | 有 | STM32F4xx, Kinetis K |
| Cortex-M7 | 有 | 有 | STM32F7xx, STM32H7xx |

---

## 2. 目录结构

```
SVCRTOS/
├── kernelsrc/                      # ★ 内核源码（零芯片依赖）
│   ├── include/                    # 内核头文件
│   │   ├── svcrt.h                 # 应用API头文件（App SDK使用）
│   │   ├── svcrt_config.h          # ★ 集中配置文件（可被板级配置覆盖）
│   │   ├── svcrt_port.h            # ★ 硬件抽象接口（纯声明，无芯片依赖）
│   │   ├── svcrt_types.h           # 基础类型定义
│   │   ├── svcrt_def.h             # 内核公共定义（句柄标志/SVC号）
│   │   ├── svcrt_task.h            # 任务管理块（内核内部）
│   │   ├── svcrt_event.h           # 事件模块（内核内部）
│   │   ├── svcrt_fifo.h            # FIFO模块（内核内部）
│   │   ├── svcrt_mpu.h             # MPU管理接口（内核内部）
│   │   ├── svcrt_dev.h             # 设备驱动框架（内核内部）
│   │   └── svcrt_cfg.h             # 任务配置加载接口（内核内部）
│   ├── src/                        # 内核源文件（纯C，无硬件操作）
│   │   ├── svcrt_init.c            # 内核启动与初始化
│   │   ├── svcrt_task.c            # 任务调度 + SVC分发 + tick/hardfault处理
│   │   ├── svcrt_event.c           # 事件管理
│   │   ├── svcrt_fifo.c            # FIFO环形缓冲区
│   │   ├── svcrt_dev.c             # 设备驱动框架
│   │   └── svcrt_cfg.c             # 任务配置加载
│   ├── app/                        # 应用模板
│   │   ├── appconfig.c             # 分区配置模板
│   │   ├── appstart.s              # 应用启动入口
│   │   └── oslib.c                 # OS接口封装（含SVC调用）
│   ├── sdk/                        # SDK 开发包
│   │   ├── app_sdk/                # App SDK
│   │   └── driver_sdk/             # Driver SDK
│   └── components/                 # 可选组件
│       ├── cm_backtrace/           # 故障回溯
│       └── nr_micro_shell/         # 调试Shell
│
├── board/                          # ★ 板级移植层（芯片相关）
│   └── stm32f427/                  # STM32F427 移植示例
│       ├── svcrt_board.c           # ★ 移植接口实现 + 中断入口 + 板载设备注册
│       ├── svcrt_board_config.h    # ★ 板级配置覆盖（主频、内存地址等）
│       ├── svcrt_mpu.c             # MPU操作实现（直接操作ARM MPU寄存器）
│       ├── context_rvds.S          # 上下文切换汇编
│       ├── drvuart.c/h             # UART驱动（HAL库）
│       └── drvled.c/h              # LED驱动（HAL库）
│
└── example/                        # 工程示例
    └── stm32f427/                  # MDK 工程
```

**关键原则：**
- `kernelsrc/` = 纯内核，不包含任何芯片头文件，不直接操作任何硬件寄存器
- `board/` = 芯片相关，移植到新芯片只需创建新的 `board/<芯片>/` 目录
- `svcrt_port.h` = 内核与硬件的唯一耦合点，纯函数声明

---

## 3. 移植步骤

### 3.1 第一步：创建板级配置文件

在 `board/<芯片>/` 下创建 `svcrt_board_config.h`，覆盖内核默认配置：

```c
/**
* @brief 板级配置 - <你的芯片>
*/
#ifndef __SVCRT_BOARD_CONFIG_H__
#define __SVCRT_BOARD_CONFIG_H__

#define SVCRT_CPU_ARCH            SVCRT_ARCH_CORTEX_M4

#define SVCRT_SYSTEM_CLOCK_HZ     (168000000)

#define SVCRT_SHARE_MEM_ADDR      (0x20028000)
#define SVCRT_SHARE_MEM_SIZE      (0x8000)

#endif
```

在 MDK 编译选项中添加预定义宏：
```
SVCRT_BOARD_CONFIG=\"svcrt_board_config.h\"
```

这样 `svcrt_config.h` 末尾会自动包含此文件，覆盖默认值。

### 3.2 第二步：创建板级移植目录

在 `board/` 下创建新芯片目录，实现以下文件：

```
board/<你的芯片>/
├── svcrt_board.c           # ★ 必须：实现 svcrt_port.h 中的所有接口 + 中断入口
├── svcrt_board_config.h    # ★ 必须：板级配置覆盖
├── svcrt_mpu.c             # 可选：MPU操作（M4/M7需要）
├── context_rvds.S          # 必须：上下文切换汇编
├── drvxxx.c/h              # 可选：板载驱动
└── ...
```

### 3.3 第三步：实现 `svcrt_board.c`

这是移植的核心文件，需要实现 `svcrt_port.h` 中声明的所有函数，以及中断服务程序。

#### 3.3.1 必须实现的移植接口函数

| 函数 | 说明 |
|------|------|
| `svcrt_port_disable_irq()` | 关闭全局中断 |
| `svcrt_port_enable_irq()` | 开启全局中断 |
| `svcrt_port_switch_task()` | 触发任务切换（设置PendSV挂起位） |
| `svcrt_port_wfi()` | 等待中断（WFI指令） |
| `svcrt_port_wfe()` | 等待事件（WFE指令） |
| `svcrt_port_nop()` | 空操作（NOP指令） |
| `svcrt_port_isb()` | 指令同步屏障 |
| `svcrt_port_dsb()` | 数据同步屏障 |
| `svcrt_port_dmb()` | 数据内存屏障 |
| `svcrt_port_set_psp()` | 设置PSP栈指针 |
| `svcrt_port_get_control()` | 读取CONTROL寄存器 |
| `svcrt_port_set_control()` | 写入CONTROL寄存器 |
| `svcrt_port_get_systick_val()` | 读取SysTick当前值 |
| `svcrt_port_get_systick_load()` | 读取SysTick重载值 |
| `svcrt_port_board_init()` | 硬件板卡初始化 |
| `svcrt_port_irq_init()` | NVIC优先级配置 |
| `svcrt_port_start_timer()` | 启动系统定时器 |
| `svcrt_port_enable_fpu()` | FPU使能（M3留空） |
| `svcrt_port_set_idle_mpu()` | 后台任务MPU设置（M3留空） |
| `svcrt_dev_board_init()` | 板载设备注册 |

#### 3.3.2 必须实现的中断服务程序

内核不再直接定义 `SysTick_Handler` 和 `HardFault_Handler`，而是提供内核处理函数供 board 层调用：

| 中断入口（board层定义） | 调用的内核函数 | 说明 |
|--------------------------|----------------|------|
| `SysTick_Handler()` | `svcrt_kernel_tick_handler()` | 系统滴答中断 |
| `HardFault_Handler()` | `svcrt_hardfault_handler()` | 硬件故障中断 |

**示例实现：**
```c
void SysTick_Handler(void)
{
    svcrt_kernel_tick_handler();
}

void HardFault_Handler(void)
{
    svcrt_hardfault_handler();
}
```

> **为什么这样设计？** 不同芯片的中断函数名可能不同（如 RISC-V 的中断入口名完全不同），将中断入口放在 board 层可以让内核完全与芯片中断体系解耦。

#### 3.3.3 Cortex-M4 完整示例（STM32F427 HAL库）

```c
/**
* @brief SVCrtOS 板级移植实现 - STM32F427
*/

#include "svcrt_port.h"
#include "svcrt_dev.h"
#include "svcrt_task.h"
#include "stm32f4xx.h"

#if (SVCRT_USE_MPU == 1)
#include "svcrt_mpu.h"
#endif

/* ---- 中断控制 ---- */
void svcrt_port_disable_irq(void)  { __disable_irq(); }
void svcrt_port_enable_irq(void)   { __enable_irq(); }

/* ---- 任务切换触发 ---- */
void svcrt_port_switch_task(void)  { SCB->ICSR = SCB_ICSR_PENDSVSET_Msk; }

/* ---- CPU 指令封装 ---- */
void svcrt_port_wfi(void)  { __WFI(); }
void svcrt_port_wfe(void)  { __WFE(); }
void svcrt_port_nop(void)  { __NOP(); }
void svcrt_port_isb(void)  { __ISB(); }
void svcrt_port_dsb(void)  { __DSB(); }
void svcrt_port_dmb(void)  { __DMB(); }

/* ---- 栈指针与控制寄存器 ---- */
void svcrt_port_set_psp(uint32 val)    { __set_PSP(val); }
uint32 svcrt_port_get_control(void)    { return __get_CONTROL(); }
void svcrt_port_set_control(uint32 val){ __set_CONTROL(val); }

/* ---- SysTick ---- */
uint32 svcrt_port_get_systick_val(void)  { return SysTick->VAL; }
uint32 svcrt_port_get_systick_load(void) { return SysTick->LOAD; }

/* ---- 移植层回调 ---- */
void svcrt_port_board_init(void)
{
    SCB->CPACR |= (3 << 20) | (3 << 22);
}

void svcrt_port_irq_init(void)
{
    NVIC_SetPriorityGrouping(0);
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
    #if (SVCRT_USE_MPU == 1)
    svcrt_mpu_set(task_func, 0x1000, stack_addr, stack_size);
    #endif
}

/* ---- 板载设备注册 ---- */
extern svcrt_dev_drv_t usart_drv;
extern svcrt_dev_drv_t led_drv;

void svcrt_dev_board_init(void)
{
    svcrt_dev_register("COM1", &usart_drv, 0);
    svcrt_dev_register("LED",  &led_drv,  0);
}

/* ---- 中断服务程序 ---- */
void SysTick_Handler(void)
{
    svcrt_kernel_tick_handler();
}

void HardFault_Handler(void)
{
    svcrt_hardfault_handler();
}
```

#### 3.3.4 Cortex-M3 精简示例

```c
#include "svcrt_port.h"
#include "svcrt_dev.h"
#include "svcrt_task.h"
#include "stm32f10x.h"

void svcrt_port_disable_irq(void)  { __disable_irq(); }
void svcrt_port_enable_irq(void)   { __enable_irq(); }
void svcrt_port_switch_task(void)  { SCB->ICSR = SCB_ICSR_PENDSVSET_Msk; }
void svcrt_port_wfi(void)          { __WFI(); }
void svcrt_port_wfe(void)          { __WFE(); }
void svcrt_port_nop(void)          { __NOP(); }
void svcrt_port_isb(void)          { __ISB(); }
void svcrt_port_dsb(void)          { __DSB(); }
void svcrt_port_dmb(void)          { __DMB(); }
void svcrt_port_set_psp(uint32 v)  { __set_PSP(v); }
uint32 svcrt_port_get_control(void){ return __get_CONTROL(); }
void svcrt_port_set_control(uint32 v){ __set_CONTROL(v); }
uint32 svcrt_port_get_systick_val(void)  { return SysTick->VAL; }
uint32 svcrt_port_get_systick_load(void) { return SysTick->LOAD; }

void svcrt_port_board_init(void) { }
void svcrt_port_enable_fpu(void) { }
void svcrt_port_set_idle_mpu(uint32 a, uint32 b, uint32 c) { }

void svcrt_port_irq_init(void)
{
    NVIC_SetPriorityGrouping(0);
    NVIC_SetPriority(PendSV_IRQn, 0xFF);
    NVIC_SetPriority(SysTick_IRQn, 0x00);
}

void svcrt_port_start_timer(uint32 tick_period_us)
{
    uint32 ticks = SystemCoreClock / 1000000 * tick_period_us / 8;
    SysTick_Config(ticks);
}

void svcrt_dev_board_init(void)
{
    /* 注册板载设备 */
}

void SysTick_Handler(void)
{
    svcrt_kernel_tick_handler();
}

void HardFault_Handler(void)
{
    svcrt_hardfault_handler();
}
```

### 3.4 第四步：实现上下文切换汇编

参考 `board/stm32f427/context_rvds.S` 进行适配：

- **Cortex-M3**：删除 `IF :DEF:SVCRT_USE_FPU` 块中的浮点保存/恢复指令
- **Cortex-M4/M7**：保留浮点保存/恢复指令

### 3.5 第五步：配置工程编译选项

---

## 4. MDK (Keil) 工程配置指南

### 4.1 添加源文件到工程

在 MDK 工程中创建以下分组（Group），并添加对应文件：

**Group: Kernel（内核核心） — 来自 kernelsrc/**

| 文件路径 | 说明 |
|----------|------|
| `kernelsrc/src/svcrt_init.c` | 内核启动与初始化 |
| `kernelsrc/src/svcrt_task.c` | 任务调度 + SVC 分发 |
| `kernelsrc/src/svcrt_event.c` | 事件管理 |
| `kernelsrc/src/svcrt_fifo.c` | FIFO 环形缓冲区 |
| `kernelsrc/src/svcrt_dev.c` | 设备驱动框架 |
| `kernelsrc/src/svcrt_cfg.c` | 任务配置加载 |

**Group: Board（板级移植） — 来自 board/**

| 文件路径 | 说明 |
|----------|------|
| `board/stm32f427/svcrt_board.c` | ★ 移植接口 + 中断入口 + 板载设备注册 |
| `board/stm32f427/svcrt_mpu.c` | MPU操作（M4/M7需） |
| `board/stm32f427/context_rvds.S` | 上下文切换汇编 |
| `board/stm32f427/drvuart.c` | UART驱动（可选） |
| `board/stm32f427/drvled.c` | LED驱动（可选） |

**Group: App（应用层） — 来自 kernelsrc/app/**

| 文件路径 | 说明 |
|----------|------|
| `kernelsrc/app/appconfig.c` | 分区配置表 |
| `kernelsrc/app/oslib.c` | 应用 SVC 接口 |
| `kernelsrc/app/appstart.s` | 应用启动汇编 |

### 4.2 配置头文件路径（Include Paths）

在 MDK → Options → C/C++ → Include Paths 中添加：

```
kernelsrc\include
board\stm32f427
```

- `kernelsrc\include` ：内核头文件
- `board\stm32f427` ：板级头文件（如 drvuart.h 等）

### 4.3 配置预定义宏（Preprocessor Defines）

在 MDK → Options → C/C++ → Preprocessor Symbols → Define 中添加：

**Cortex-M4 工程：**
```
SVCRT_USE_FPU,SVCRT_BOARD_CONFIG=\"svcrt_board_config.h\"
```

**Cortex-M3 工程：**
```
SVCRT_BOARD_CONFIG=\"svcrt_board_config.h\"
```

> `SVCRT_BOARD_CONFIG` 宏让 `svcrt_config.h` 自动包含板级配置文件，覆盖默认的主频和内存地址。

### 4.4 汇编文件配置

- `context_rvds.S` 需要使用 ARM 汇编器（ARMASM），MDK 默认支持
- 汇编文件中的条件编译使用 ARMASM 语法，在 Options → Asm → Preprocessor Symbols 中配置
- M4 工程需在 Define 中添加 `SVCRT_USE_FPU`

### 4.5 链接配置注意事项

- 内核入口 `main()` 需要位于 Flash 起始地址
- 各分区的 ROM 地址需与 `appconfig.c` 中的 `rom_start` 对应
- 共享内存地址 `SVCRT_SHARE_MEM_ADDR` 需在链接脚本中正确配置

### 4.6 移植检查清单

移植到新芯片时，请逐项确认：

- [ ] 创建 `board/<芯片>/svcrt_board_config.h`，设置正确的主频和内存地址
- [ ] 创建 `board/<芯片>/svcrt_board.c`，实现所有移植接口函数
- [ ] 在 `svcrt_board.c` 中实现 `SysTick_Handler()` 和 `HardFault_Handler()`
- [ ] 在 `svcrt_board.c` 的 `svcrt_dev_board_init()` 中注册板载设备
- [ ] 实现上下文切换汇编，根据架构选择是否包含 FPU 保存/恢复
- [ ] MDK 工程添加所有 `kernelsrc/src/*.c` 文件
- [ ] MDK 工程添加所有 `board/<芯片>/*.c` 和 `*.S` 文件
- [ ] MDK 工程添加 `kernelsrc/app/` 下的应用文件
- [ ] Include Paths 包含 `kernelsrc/include` 和 `board/<芯片>`
- [ ] 预定义宏包含 `SVCRT_BOARD_CONFIG=\"svcrt_board_config.h\"`
- [ ] M4 工程添加 `SVCRT_USE_FPU` 预定义宏（C 和汇编均需）
- [ ] `appconfig.c` 中分区地址与实际芯片内存映射匹配

---

## 5. 配置参数详解

### 5.1 svcrt_config.h 参数表

| 参数名 | 默认值 | 说明 |
|--------|--------|------|
| `SVCRT_CPU_ARCH` | `SVCRT_ARCH_CORTEX_M4` | CPU架构选择 |
| `SVCRT_USE_FPU` | 自动 | 浮点单元使能 |
| `SVCRT_USE_MPU` | 自动 | 内存保护单元使能 |
| `SVCRT_USE_PRIV` | 自动 | 特权模式分离（依赖MPU） |
| `SVCRT_TASK_MAX_NUM` | 7 | 最大任务数量 |
| `SVCRT_TICK_PERIOD_US` | 500 | 滴答周期(微秒) |
| `SVCRT_EVENT_NUM` | 10 | 事件对象数量 |
| `SVCRT_MAX_EVENT_WAITERS` | 4 | 事件最大等待任务数 |
| `SVCRT_DEV_MAX_NUM` | 8 | 最大设备驱动数量 |
| `SVCRT_USE_CPU_LOAD` | 1 | CPU负载统计开关 |
| `SVCRT_USE_STACK_CHECK` | 1 | 栈溢出检测开关 |
| `SVCRT_SYSTEM_CLOCK_HZ` | 168000000 | 系统主频（Hz），由板级配置覆盖 |
| `SVCRT_SHARE_MEM_ADDR` | 0x20028000 | 共享内存地址，由板级配置覆盖 |
| `SVCRT_SHARE_MEM_SIZE` | 0x8000 | 共享内存大小，由板级配置覆盖 |

### 5.2 功能裁剪生效方式

当 `SVCRT_USE_MPU=0` 时：
- `svcrt_mpu_module_init()` / `svcrt_mpu_set_app()` 等函数被宏替换为空
- `svcrt_task_t` 中不包含 `mpu_bar[8]` / `mpu_asr[8]` 字段（节省RAM）
- `svcrt_init.c` 中不调用MPU初始化，不设置MPU保护

当 `SVCRT_USE_FPU=0` 时：
- 异常帧中不包含浮点寄存器保存区
- 上下文切换时不保存/恢复浮点寄存器
- 任务栈需求大幅减少

---

## 6. Cortex-M3 移植特别说明

### 6.1 无MPU的后果

- 任务之间**没有**内存隔离保护
- 任何任务都可以访问全部RAM空间
- HardFault 只能杀死出错任务，不能防止越界访问
- `SVCRT_USE_PRIV` 强制关闭，无法实现特权级分离

### 6.2 无FPU的优化

- 上下文切换不保存16个浮点寄存器 + FPSCR
- 每个任务栈节省约68字节
- 中断响应速度更快

### 6.3 资源对比

| 项目 | Cortex-M3 | Cortex-M4 |
|------|-----------|-----------|
| 最小任务栈帧 | ~40字节 | ~108字节 |
| 上下文切换时间 | ~30周期 | ~60周期 |
| 额外RAM开销 | 较少 | 较多（MPU字段） |

---

## 7. 设备驱动框架

SVCrtOS 的设备驱动框架统一管理**内置驱动**和**可安装驱动**，共用同一套设备表和 `svcrt_dev_register()` 接口。

### 7.1 驱动形态对比

| 驱动形态 | 注册时机 | 生命周期 | 位置 |
|----------|----------|----------|------|
| 内置驱动 | `svcrt_dev_board_init()` 中注册 | 随内核常驻 | `board/<芯片>/` |
| 可安装驱动 | 动态注册 | 按需加载卸载 | 独立编译 |

两种驱动共用相同的 `svcrt_dev_drv_t` 结构（5个函数指针：open/close/read/write/ctrl），二进制兼容。

### 7.2 内置驱动开发

在 `board/<芯片>/svcrt_board.c` 的 `svcrt_dev_board_init()` 中注册内置驱动：

```c
extern svcrt_dev_drv_t usart_drv;
extern svcrt_dev_drv_t led_drv;

void svcrt_dev_board_init(void)
{
    svcrt_dev_register("COM1", &usart_drv, 0);
    svcrt_dev_register("LED",  &led_drv,  0);
}
```

此函数在 `svcrt_init.c` 的 `svcrt_kernel_init()` 中被自动调用。

### 7.3 可安装驱动开发（Driver SDK）

可安装驱动只需包含 `svcrt_driver_sdk.h`，实现驱动接口后调用注册函数：

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

### 7.4 卸载驱动

```c
svcrt_drv_unregister("MYDEV");
```

---

## 8. 移植新芯片快速指南

以移植到 STM32F103 为例：

### 8.1 创建板级目录

```
board/stm32f103/
├── svcrt_board.c           # 参考 stm32f427 版本，修改芯片头文件
├── svcrt_board_config.h    # 设置 F103 的主频和内存地址
├── context_rvds.S          # 参考 M3 版本，去掉 FPU 部分
├── drvuart.c/h             # 使用 F103 的 HAL 或 SPL 库
└── drvled.c/h              # 使用 F103 的 HAL 或 SPL 库
```

### 8.2 创建板级配置

```c
/* board/stm32f103/svcrt_board_config.h */
#define SVCRT_CPU_ARCH            SVCRT_ARCH_CORTEX_M3
#define SVCRT_SYSTEM_CLOCK_HZ     (72000000)
#define SVCRT_SHARE_MEM_ADDR      (0x20005000)
#define SVCRT_SHARE_MEM_SIZE      (0x2000)
```

### 8.3 修改 svcrt_board.c

```c
#include "stm32f10x.h"  // 替换为F103的头文件
// FPU/MPU 相关函数留空
```

### 8.4 修改上下文切换汇编

删除浮点寄存器保存/恢复（`VSTMDB`/`VLDMIA` 指令）。

### 8.5 在 MDK 中更新配置

- 替换 Board 组文件为 `board/stm32f103/` 下的文件
- 删除 `SVCRT_USE_FPU` 预定义宏
- 更新 Include Paths
- 更新预定义宏 `SVCRT_BOARD_CONFIG=\"svcrt_board_config.h\"`

**关键：以上所有修改都在 board/ 目录下完成，kernelsrc/ 目录无需任何改动！**

---

## 9. App SDK 使用说明

### 9.1 应用程序开发

应用程序只需实现 `AppMain()` 函数即可：

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

### 9.2 App SDK 文件清单

| 文件 | 说明 |
|------|------|
| `svcrt.h` | 应用API头文件 |
| `svcrt_types.h` | 基础类型定义 |
| `svcrt_app_config.h` | 应用配置结构 |
| `svcrt_oslib.c` | SVC调用封装 |
| `svcrt_app_main.c` | 应用入口模板 |
| `svcrt_app_start.s` | 应用启动汇编 |

### 9.3 完整API列表

| API | 说明 |
|-----|------|
| `svcrt_task_wait(ms)` | 等待指定毫秒 |
| `svcrt_task_wait_period()` | 等待当前周期 |
| `svcrt_task_delay(us)` | 微秒级忙等延时 |
| `svcrt_task_kill()` | 终止当前任务 |
| `svcrt_get_time_ms()` | 获取系统运行时间(ms) |
| `svcrt_get_cpu_usage()` | 获取CPU使用率 |
| `svcrt_event_create(name)` | 创建事件 |
| `svcrt_event_wait(handle, timeout)` | 等待事件 |
| `svcrt_event_set(handle)` | 触发事件 |
| `svcrt_dev_open(name, param)` | 打开设备 |
| `svcrt_dev_close(handle)` | 关闭设备 |
| `svcrt_dev_read(handle, data, len)` | 读取设备 |
| `svcrt_dev_write(handle, data, len)` | 写入设备 |
| `svcrt_dev_ctrl(handle, code, value)` | 设备控制 |
