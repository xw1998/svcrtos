# SVCrtOS 移植手册

## 1. 架构概述

SVCrtOS 采用分层解耦架构，内核代码（kernelsrc/）与具体芯片完全分离。移植到新芯片时，只需实现 port 层和 board 层，内核代码无需任何修改。

### 1.1 分层架构图

```
┌──────────────────────────────────────────────────────────────┐
│            应用层 (App SDK)                                   │
├──────────────────────────────────────────────────────────────┤
│         SVCrtOS 内核 (kernelsrc/src/)      │ ← 纯 C，零硬件依赖│
│    任务调度、事件管理、设备框架、SVC分发                       │
├──────────────────────────────────────────────────────────────┤
│     svcrt_hal.h (硬件抽象接口)             │ ← 纯函数声明     │
├──────────────────────────────────────────────────────────────┤
│      port/<架构>/ (架构适配层)             │ ← 架构相关实现   │
│   CPU指令、上下文切换、栈帧初始化、MPU操作                    │
│   例: port/arm/cortex-m4/svcrt_port.c                        │
│       port/arm/cortex-m4/svcrt_context.S                     │
├──────────────────────────────────────────────────────────────┤
│      board/<芯片>/ (板级适配层)             │ ← 芯片相关实现   │
│   板卡初始化、中断入口、设备驱动                              │
│   例: board/stm32f427/svcrt_board.c                          │
│       board/stm32f427/drvuart.c                              │
├──────────────────────────────────────────────────────────────┤
│            具体MCU硬件                                        │
└──────────────────────────────────────────────────────────────┘
```

**解耦原则：**
- `kernelsrc/src/` 中的代码不包含任何芯片头文件
- `kernelsrc/src/` 中的代码不直接操作任何硬件寄存器
- 所有硬件访问均通过 `svcrt_hal.h` 中声明的函数抽象
- 架构相关代码（上下文切换、栈帧初始化）在 `port/` 层实现
- 板级相关代码（中断入口、设备驱动）在 `board/` 层实现

### 1.2 支持的CPU架构

| 架构 | FPU | MPU | port目录 |
|------|-----|-----|----------|
| Cortex-M3 | - | 可选 | port/arm/cortex-m3 |
| Cortex-M4 | 有 | 有 | port/arm/cortex-m4 |
| Cortex-M7 | 有 | 有 | port/arm/cortex-m7 (待实现) |

---

## 2. 目录结构

```
SVCRTOS/
├── kernelsrc/                      # ★ 内核源码（零硬件依赖）
│   ├── include/                    # 内核头文件
│   │   ├── svcrt.h                 # 应用API头文件
│   │   ├── svcrt_hal.h             # ★ 硬件抽象接口（核心）
│   │   ├── svcrt_port.h            # 兼容层（包含svcrt_hal.h）
│   │   ├── svcrt_config.h          # 内核配置文件
│   │   ├── svcrt_types.h           # 基础类型定义
│   │   ├── svcrt_def.h             # 内核公共定义
│   │   ├── svcrt_task.h            # 任务管理（内核内部）
│   │   ├── svcrt_event.h           # 事件模块（内核内部）
│   │   ├── svcrt_fifo.h            # FIFO模块（内核内部）
│   │   ├── svcrt_mpu.h             # MPU管理接口（内核内部）
│   │   ├── svcrt_dev.h             # 设备驱动框架（内核内部）
│   │   └── svcrt_cfg.h             # 任务配置加载（内核内部）
│   ├── src/                        # 内核源文件（纯C，零硬件操作）
│   │   ├── svcrt_init.c            # 内核启动与初始化
│   │   ├── svcrt_task.c            # 任务调度 + SVC 分发
│   │   ├── svcrt_event.c           # 事件管理
│   │   ├── svcrt_fifo.c            # FIFO 环形缓冲区
│   │   ├── svcrt_dev.c             # 设备驱动框架
│   │   └── svcrt_cfg.c             # 任务配置加载
│   ├── port/                       # ★ 架构适配层
│   │   └── arm/
│   │       ├── cortex-m3/
│   │       │   ├── svcrt_port.c    # Cortex-M3 架构实现
│   │       │   └── svcrt_context.S # 上下文切换汇编
│   │       └── cortex-m4/
│   │           ├── svcrt_port.c    # Cortex-M4 架构实现
│   │           └── svcrt_context.S # 上下文切换汇编
│   ├── sdk/                        # SDK 开发包
│   └── components/                 # 可选组件
│
├── board/                          # ★ 板级适配层
│   └── stm32f427/                  # STM32F427 移植示例
│       ├── svcrt_board.c           # 板级初始化 + 中断入口
│       ├── svcrt_board_config.h    # 板级配置覆盖
│       ├── drvuart.c/h             # UART驱动
│       └── drvled.c/h              # LED驱动
│
└── example/                        # 工程示例
    └── stm32f427/                  # MDK 工程
```

---

## 3. 移植步骤

### 3.1 第一步：创建板级配置文件

在 `board/<芯片>/` 下创建 `svcrt_board_config.h`：

```c
#ifndef __SVCRT_BOARD_CONFIG_H__
#define __SVCRT_BOARD_CONFIG_H__

#define SVCRT_CPU_ARCH            SVCRT_ARCH_CORTEX_M4
#define SVCRT_SYSTEM_CLOCK_HZ     (168000000)
#define SVCRT_SHARE_MEM_ADDR      (0x20028000)
#define SVCRT_SHARE_MEM_SIZE      (0x8000)

#undef  SVCRT_USE_FPU
#define SVCRT_USE_FPU             1

#undef  SVCRT_USE_MPU
#define SVCRT_USE_MPU             0

#endif
```

在编译选项中添加：`-DSVCRT_BOARD_CONFIG=\"svcrt_board_config.h\"`

### 3.2 第二步：选择架构适配层

根据目标CPU架构，选择对应的port目录：

| CPU架构 | port目录 | 说明 |
|---------|----------|------|
| Cortex-M3 | port/arm/cortex-m3 | 无FPU，MPU可选 |
| Cortex-M4 | port/arm/cortex-m4 | 有FPU和MPU |
| Cortex-M7 | port/arm/cortex-m7 | 待实现 |

将对应目录下的 `svcrt_port.c` 和 `svcrt_context.S` 加入工程。

### 3.3 第三步：实现板级适配文件

在 `board/<芯片>/` 下创建 `svcrt_board.c`，覆盖port层的弱定义函数：

```c
#include "stm32f4xx.h"
#include "svcrt_hal.h"
#include "svcrt_dev.h"
#include "svcrt_task.h"

/* 覆盖port层弱定义 */
void svcrt_port_board_init(void)
{
    SCB->CPACR |= (3UL << 20) | (3UL << 22);  /* FPU使能 */
}

void svcrt_port_irq_init(void)
{
    NVIC_SetPriorityGrouping(0);
    NVIC_SetPriority(PendSV_IRQn,  0xFF);
    NVIC_SetPriority(SysTick_IRQn, 0x00);
    NVIC_SetPriority(SVCall_IRQn,  0x01);
}

/* 板载设备注册 */
extern svcrt_dev_drv_t usart_drv;
extern svcrt_dev_drv_t led_drv;

void svcrt_dev_board_init(void)
{
    svcrt_dev_register("COM1", &usart_drv, 0);
    svcrt_dev_register("LED",  &led_drv,  0);
}

/* 中断服务程序入口 */
void SysTick_Handler(void)
{
    svcrt_kernel_tick_handler();
}

void HardFault_Handler(void)
{
    while(1);
}
```

### 3.4 第四步：实现设备驱动

参考 `board/stm32f427/drvuart.c` 和 `drvled.c` 实现板载设备驱动。

---

## 4. svcrt_hal.h 接口说明

### 4.1 CPU指令层

| 函数 | 说明 |
|------|------|
| `svcrt_port_wfi()` | 等待中断 |
| `svcrt_port_wfe()` | 等待事件 |
| `svcrt_port_nop()` | 空操作 |
| `svcrt_port_isb()` | 指令同步屏障 |
| `svcrt_port_dsb()` | 数据同步屏障 |
| `svcrt_port_dmb()` | 数据内存屏障 |

### 4.2 中断控制层

| 函数 | 说明 |
|------|------|
| `svcrt_port_disable_irq()` | 关闭全局中断 |
| `svcrt_port_enable_irq()` | 开启全局中断 |
| `svcrt_port_switch_task()` | 触发任务切换 |

### 4.3 上下文层

| 函数 | 说明 |
|------|------|
| `svcrt_port_set_psp()` | 设置PSP栈指针 |
| `svcrt_port_get_control()` | 读取CONTROL寄存器 |
| `svcrt_port_set_control()` | 写入CONTROL寄存器 |
| `svcrt_port_stack_init()` | 初始化任务栈帧 |
| `svcrt_port_enter_idle()` | 进入空闲任务上下文 |

### 4.4 定时器层

| 函数 | 说明 |
|------|------|
| `svcrt_port_get_system_clock()` | 获取系统主频 |
| `svcrt_port_get_systick_val()` | 获取SysTick当前值 |
| `svcrt_port_get_systick_load()` | 获取SysTick重载值 |
| `svcrt_port_start_timer()` | 启动系统定时器 |
| `svcrt_port_delay_us()` | 微秒级忙等延时 |

### 4.5 板级层（弱定义，可覆盖）

| 函数 | 说明 |
|------|------|
| `svcrt_port_board_init()` | 板卡硬件初始化 |
| `svcrt_port_irq_init()` | 中断控制器初始化 |
| `svcrt_port_enable_fpu()` | FPU使能 |
| `svcrt_port_set_idle_mpu()` | 空闲任务MPU设置 |

### 4.6 MPU接口层

| 函数 | 说明 |
|------|------|
| `svcrt_port_mpu_init()` | MPU初始化 |
| `svcrt_port_mpu_set_region()` | 设置MPU区域 |
| `svcrt_port_mpu_set_app()` | 设置任务MPU |
| `svcrt_port_mpu_reset()` | 重置MPU |

---

## 5. MDK工程配置

### 5.1 添加源文件

**Group: Kernel**
- kernelsrc/src/svcrt_init.c
- kernelsrc/src/svcrt_task.c
- kernelsrc/src/svcrt_event.c
- kernelsrc/src/svcrt_fifo.c
- kernelsrc/src/svcrt_dev.c
- kernelsrc/src/svcrt_cfg.c

**Group: Port**
- kernelsrc/port/arm/cortex-m4/svcrt_port.c
- kernelsrc/port/arm/cortex-m4/svcrt_context.S

**Group: Board**
- board/stm32f427/svcrt_board.c
- board/stm32f427/drvuart.c
- board/stm32f427/drvled.c

### 5.2 头文件路径

```
kernelsrc\include
kernelsrc\port\arm\cortex-m4
board\stm32f427
```

### 5.3 预定义宏

```
SVCRT_BOARD_CONFIG=\"svcrt_board_config.h\"
```

---

## 6. 移植新架构指南

以移植到 RISC-V 为例：

1. 创建 `kernelsrc/port/risc-v/rv32imac/` 目录
2. 实现 `svcrt_port.c`：
   - CPU指令层（wfi、fence等）
   - 中断控制层（mie、mip等）
   - 上下文层（栈帧初始化、mstatus设置）
   - 定时器层（mtime配置）
3. 实现 `svcrt_context.S`：
   - 任务上下文保存/恢复（x1-x31寄存器）
   - 异常入口处理
4. 内核代码无需任何修改

---

## 7. 功能完整性验证

移植完成后，请验证以下功能：

- [ ] 任务创建与调度
- [ ] 任务延时（svcrt_task_wait）
- [ ] 微秒延时（svcrt_task_delay）
- [ ] 事件创建与等待
- [ ] 设备驱动注册与操作
- [ ] 栈溢出检测
- [ ] CPU负载统计
- [ ] MPU内存保护（如启用）
