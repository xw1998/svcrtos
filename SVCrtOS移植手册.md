# SVCrtOS 内核移植手册 —— 以 STM32F427 MDK 工程为例

## 1. 概述

本文档记录将 SVCrtOS 内核移植到 STM32F427 Keil MDK 工程的完整过程。SVCrtOS 采用零芯片依赖架构，内核代码（`kernelsrc/`）不包含任何 MCU 特定头文件，所有硬件相关操作通过 `svcrt_port.h` 定义的端口接口由板级层（`board/`）实现。

### 1.1 目录结构

```
SVCRTOS/
├── kernelsrc/                    # 内核源码（零芯片依赖）
│   ├── include/                  # 内核头文件
│   │   ├── svcrt.h               # 应用层统一入口
│   │   ├── svcrt_config.h        # 内核编译配置
│   │   ├── svcrt_types.h         # 基础类型定义
│   │   ├── svcrt_def.h           # 内核内部定义
│   │   ├── svcrt_port.h          # 端口接口声明
│   │   ├── svcrt_task.h          # 任务管理
│   │   ├── svcrt_event.h         # 事件模块
│   │   ├── svcrt_dev.h           # 设备驱动框架
│   │   ├── svcrt_fifo.h          # 环形缓冲区
│   │   ├── svcrt_cfg.h           # 任务配置
│   │   └── svcrt_mpu.h           # MPU 内存保护
│   └── src/                      # 内核源文件
│       ├── svcrt_task.c          # 任务调度与 SVC 服务
│       ├── svcrt_event.c         # 事件管理
│       ├── svcrt_dev.c           # 设备驱动框架
│       ├── svcrt_fifo.c          # FIFO 实现
│       ├── svcrt_cfg.c           # 配置加载
│       └── svcrt_init.c          # 默认启动入口（可选）
│
├── board/                        # 板级适配层
│   └── stm32f427/                # STM32F427 适配
│       ├── svcrt_board.c         # 端口接口实现
│       ├── svcrt_board_config.h  # 板级配置
│       ├── svcrt_mpu.c           # MPU 配置实现
│       ├── context_rvds.S        # 上下文切换汇编（RVDS/Keil）
│       ├── drvuart.c / .h        # 串口驱动
│       └── drvled.c / .h         # LED 驱动
│
└── example/                      # 示例工程
    └── stm32f427/
        └── kernel/
            └── SVCRTOS_TEST/     # CubeMX 生成的 MDK 工程
```

### 1.2 移植原则

| 原则 | 说明 |
|------|------|
| 内核不动 | `kernelsrc/` 下代码不做任何修改以适配具体芯片 |
| 端口隔离 | 所有硬件操作通过 `svcrt_port.h` 接口，由 `board/` 实现 |
| 配置外置 | 通过 `SVCRT_BOARD_CONFIG` 宏引入板级配置头文件 |
| HAL 共存 | 保留 HAL 库用于硬件初始化，内核调度独立运行 |

---

## 2. 移植前准备

### 2.1 确认硬件参数

| 参数 | STM32F427 值 | 来源 |
|------|-------------|------|
| 内核架构 | Cortex-M4F | 数据手册 |
| FPU | 有 | 芯片特性 |
| MPU | 有 | 芯片特性 |
| 主频 | 168 MHz | SystemCoreClock |
| 主 SRAM | 0x20000000, 192KB | 参考手册 |
| CCM RAM | 0x10000000, 64KB | 参考手册 |
| Flash | 0x08000000, 1MB | 参考手册 |

### 2.2 确认内核配置

在 `svcrt_config.h` 和 `svcrt_board_config.h` 中确认以下配置：

```c
/* svcrt_config.h 中的默认值 */
#define SVCRT_CPU_ARCH            SVCRT_ARCH_CORTEX_M4
#define SVCRT_USE_FPU             1
#define SVCRT_USE_MPU             1
#define SVCRT_USE_PRIV            1
#define SVCRT_TASK_MAX_NUM        (7)
#define SVCRT_TICK_PERIOD_US      (500)
```

```c
/* board/stm32f427/svcrt_board_config.h */
#define SVCRT_CPU_ARCH            SVCRT_ARCH_CORTEX_M4
#define SVCRT_SYSTEM_CLOCK_HZ     (168000000)
#define SVCRT_SHARE_MEM_ADDR      (0x20028000)
#define SVCRT_SHARE_MEM_SIZE      (0x8000)
```

---

## 3. 移植步骤

### 步骤一：修改 MDK 工程文件（.uvprojx）

#### 3.1.1 添加源文件分组

在 `<Groups>` 节点下新增两个分组：

**SVCRTOS/Kernel 分组**（内核源文件）：

| 文件 | 路径（相对于 MDK-ARM 目录） |
|------|---------------------------|
| svcrt_task.c | `..\..\..\..\..\kernelsrc\src\svcrt_task.c` |
| svcrt_dev.c | `..\..\..\..\..\kernelsrc\src\svcrt_dev.c` |
| svcrt_event.c | `..\..\..\..\..\kernelsrc\src\svcrt_event.c` |
| svcrt_fifo.c | `..\..\..\..\..\kernelsrc\src\svcrt_fifo.c` |
| svcrt_cfg.c | `..\..\..\..\..\kernelsrc\src\svcrt_cfg.c` |

> **注意**：不要添加 `svcrt_init.c`，因为 `main.c` 已手动实现了相同的启动逻辑，否则会产生 `main()` 重复定义。

**SVCRTOS/Board 分组**（板级适配文件）：

| 文件 | 路径（相对于 MDK-ARM 目录） |
|------|---------------------------|
| svcrt_board.c | `..\..\..\..\..\board\stm32f427\svcrt_board.c` |
| svcrt_mpu.c | `..\..\..\..\..\board\stm32f427\svcrt_mpu.c` |
| context_rvds.S | `..\..\..\..\..\board\stm32f427\context_rvds.S` |
| drvuart.c | `..\..\..\..\..\board\stm32f427\drvuart.c` |
| drvled.c | `..\..\..\..\..\board\stm32f427\drvled.c` |

> **路径计算**：MDK 工程文件位于 `example/stm32f427/kernel/SVCRTOS_TEST/MDK-ARM/`，需要上溯 5 级才能到达 `SVCRTOS/` 根目录：
> `MDK-ARM/ → SVCRTOS_TEST/ → kernel/ → stm32f427/ → example/ → SVCRTOS/`

#### 3.1.2 配置 Include Paths

在 C/C++ 编译选项的 Include Paths 中追加：

```
..\..\..\..\..\kernelsrc\include;..\..\..\..\..\board\stm32f427
```

完整的 Include Paths 应为：

```
../Core/Inc;
../Drivers/STM32F4xx_HAL_Driver/Inc;
../Drivers/STM32F4xx_HAL_Driver/Inc/Legacy;
../Drivers/CMSIS/Device/ST/STM32F4xx/Include;
../Drivers/CMSIS/Include;
..\..\..\..\..\kernelsrc\include;
..\..\..\..\..\board\stm32f427
```

#### 3.1.3 配置预定义宏

在 C/C++ 编译选项的 Define 中追加：

```
SVCRT_USE_FPU=1,SVCRT_BOARD_CONFIG=\"svcrt_board_config.h\"
```

完整 Define 应为：

```
USE_HAL_DRIVER,STM32F427xx,SVCRT_USE_FPU=1,SVCRT_BOARD_CONFIG=\"svcrt_board_config.h\"
```

在汇编器选项的 Define 中追加：

```
SVCRT_USE_FPU=1
```

> `SVCRT_USE_FPU=1` 使 `context_rvds.S` 中的 FPU 上下文保存/恢复代码生效。
> `SVCRT_BOARD_CONFIG` 宏使 `svcrt_config.h` 在末尾 `#include "svcrt_board_config.h"`，允许板级配置覆盖默认值。

---

### 步骤二：修改 main.c

将 CubeMX 生成的 `main.c` 替换为内核启动入口：

```c
#include "main.h"
#include "gpio.h"
#include "svcrt_port.h"
#include "svcrt_cfg.h"
#include "svcrt_event.h"
#include "svcrt_dev.h"
#include "svcrt_mpu.h"

void SystemClock_Config(void);
static void svcrt_kernel_init(void);
static void svcrt_start_idle(void);

int main(void)
{
    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init();

    svcrt_port_board_init();
    svcrt_cfg_load();
    svcrt_port_irq_init();
    svcrt_kernel_init();
    svcrt_port_start_timer(SVCRT_TICK_PERIOD_US);

    svcrt_start_idle();
    return 0;
}

static uint32 svcrt_idle_stack[100];

static void svcrt_start_idle(void)
{
    SVCRT_SET_PSP((uint32)(svcrt_idle_stack + 99));

    #if (SVCRT_USE_MPU == 1)
    svcrt_port_set_idle_mpu((uint32)svcrt_start_idle,
                            (uint32)svcrt_idle_stack,
                            sizeof(svcrt_idle_stack));
    #endif

    #if (SVCRT_USE_PRIV == 1)
    SVCRT_SET_CONTROL(0x3 | SVCRT_GET_CONTROL());
    #else
    SVCRT_SET_CONTROL(0x2 | SVCRT_GET_CONTROL());
    #endif
    SVCRT_ISB();
    SVCRT_WFI();

    while(1) {}
}

static void svcrt_kernel_init(void)
{
    svcrt_event_module_init();
    svcrt_dev_module_init();
    svcrt_dev_board_init();

    #if (SVCRT_USE_MPU == 1)
    svcrt_mpu_module_init();
    #endif
}
```

**关键要点**：

1. **保留 HAL 初始化**：`HAL_Init()`、`SystemClock_Config()`、`MX_GPIO_Init()` 必须在内核启动前调用
2. **启动顺序**：板卡初始化 → 配置加载 → 中断初始化 → 内核初始化 → 启动定时器 → 进入 idle
3. **前向声明**：`svcrt_kernel_init` 和 `svcrt_start_idle` 是 static 函数，必须在 `main()` 前声明
4. **idle 任务**：设置 PSP 栈指针、配置 MPU（可选）、切换到非特权模式、进入 WFI

---

### 步骤三：修改 stm32f4xx_it.c

移除与内核/board 层冲突的中断处理器：

**移除以下函数**（已在 `svcrt_board.c` 和 `context_rvds.S` 中实现）：

| 函数 | 实现位置 |
|------|---------|
| `HardFault_Handler` | `svcrt_board.c` |
| `SysTick_Handler` | `svcrt_board.c` |
| `SVC_Handler` | `context_rvds.S` |
| `PendSV_Handler` | `context_rvds.S` |

**保留以下函数**：

```c
void NMI_Handler(void)      { while(1) {} }
void MemManage_Handler(void) { while(1) {} }
void BusFault_Handler(void)  { while(1) {} }
void UsageFault_Handler(void){ while(1) {} }
void DebugMon_Handler(void)  {}
```

同步修改 `stm32f4xx_it.h`，移除对应声明。

---

### 步骤四：修改 usart.c

清空 `MX_USART1_UART_Init()` 函数体，UART 初始化由 `drvuart.c` 在设备打开时负责：

```c
void MX_USART1_UART_Init(void)
{
}
```

保留 `HAL_UART_MspInit()` 中的 GPIO 配置（如果需要 HAL 阶段的引脚初始化），或也可清空。

---

### 步骤五：修改 Scatter File

调整 SRAM 区域大小，为内核共享内存预留空间：

```
; *************************************************************
; *** Scatter-Loading Description File for SVCrtOS          ***
; *************************************************************

LR_IROM1 0x08000000 0x00100000  {
  ER_IROM1 0x08000000 0x00100000  {
   *.o (RESET, +First)
   *(InRoot$$Sections)
   .ANY (+RO)
   .ANY (+XO)
  }
  RW_IRAM1 0x20000000 0x00028000  {
   .ANY (+RW +ZI)
  }
  RW_IRAM2 0x10000000 0x00010000  {
   .ANY (+RW +ZI)
  }
}
```

**内存布局说明**：

| 区域 | 起始地址 | 大小 | 用途 |
|------|---------|------|------|
| Flash | 0x08000000 | 1MB | 代码与只读数据 |
| 主 SRAM | 0x20000000 | 160KB (0x28000) | 内核 + 任务 RAM |
| 共享内存 | 0x20028000 | 32KB (0x8000) | 任务间共享（由 SVCRT_SHARE_MEM_ADDR 配置） |
| CCM RAM | 0x10000000 | 64KB | 可用但仅 CPU 可访问 |

> 主 SRAM 从 192KB 缩减为 160KB，高地址 32KB 留作内核共享内存区。

---

## 4. 编译问题排查

### 4.1 SystemCoreClock 类型冲突

**错误信息**：
```
error: #147: declaration is incompatible with "uint32_t SystemCoreClock"
   extern uint32 SystemCoreClock;
```

**原因**：`svcrt_port.h` 中声明为 `uint32`（unsigned long），而 CMSIS `system_stm32f4xx.h` 声明为 `uint32_t`（unsigned int），在 ARMCC 中两者不兼容。

**解决方案**：在 `svcrt_port.h` 中使用 `uint32_t` 类型：

```c
#include <stdint.h>
extern uint32_t SystemCoreClock;
```

### 4.2 USART_TypeDef 未定义

**错误信息**：
```
error: #20: identifier "USART_TypeDef" is undefined
```

**原因**：`drvuart.h` 中使用了 `USART_TypeDef` 但未包含设备头文件。

**解决方案**：在 `drvuart.h` 中添加：

```c
#include "stm32f4xx.h"
```

### 4.3 HAL 函数未声明

**错误信息**：board 层 `.c` 文件中 HAL 函数隐式声明。

**解决方案**：在 `drvuart.c` 和 `drvled.c` 中添加：

```c
#include "stm32f4xx_hal.h"
```

### 4.4 static 函数前向声明缺失

**错误信息**：
```
error: #159: declaration is incompatible with previous "svcrt_start_idle"
warning: #223-D: function "svcrt_kernel_init" declared implicitly
```

**原因**：`main()` 中调用了定义在其后的 `static` 函数。

**解决方案**：在 `main()` 前添加前向声明：

```c
static void svcrt_kernel_init(void);
static void svcrt_start_idle(void);
```

### 4.5 MX_GPIO_Init 隐式声明

**解决方案**：在 `main.c` 中添加：

```c
#include "gpio.h"
```

---

## 5. 中断分配

| 中断 | 优先级 | 处理函数 | 所在文件 |
|------|--------|---------|---------|
| SysTick | 0x00（最高） | `SysTick_Handler` | `svcrt_board.c` |
| SVC | 0x01 | `SVC_Handler` | `context_rvds.S` |
| USART1 | 0x0A | `USART1_IRQHandler` | `drvuart.c` |
| PendSV | 0xFF（最低） | `PendSV_Handler` | `context_rvds.S` |

> 优先级分组为 0（即只使用抢占优先级，无子优先级）。

---

## 6. 内核启动流程

```
main()
  │
  ├── HAL_Init()                    # HAL 库初始化
  ├── SystemClock_Config()          # 系统时钟配置（168MHz）
  ├── MX_GPIO_Init()                # GPIO 初始化
  │
  ├── svcrt_port_board_init()       # FPU 使能（CPACR）
  ├── svcrt_cfg_load()              # 加载任务配置表
  ├── svcrt_port_irq_init()         # NVIC 优先级分组 + PendSV/SysTick/SVC 优先级
  │
  ├── svcrt_kernel_init()           # 内核子系统初始化
  │   ├── svcrt_event_module_init() # 事件模块
  │   ├── svcrt_dev_module_init()   # 设备框架
  │   ├── svcrt_dev_board_init()    # 注册板级驱动（COM1, LED）
  │   └── svcrt_mpu_module_init()   # MPU 初始化
  │
  ├── svcrt_port_start_timer()      # 启动 SysTick 定时器
  │
  └── svcrt_start_idle()            # 切换到非特权 idle 任务
      ├── 设置 PSP 栈指针
      ├── 配置 idle MPU 区域
      ├── 切换到非特权模式 + PSP
      └── WFI 等待中断
```

---

## 7. 移植到其他芯片的检查清单

将 SVCrtOS 移植到其他 Cortex-M 芯片时，需完成以下工作：

- [ ] **创建 `board/<chip>/` 目录**，实现以下文件：
  - `svcrt_board.c` — 实现 `svcrt_port.h` 中所有端口函数
  - `svcrt_board_config.h` — 配置 CPU 架构、时钟频率、共享内存地址
  - `context_rvds.S`（Keil）或 `context_gcc.S`（GCC）— 上下文切换汇编
  - `svcrt_mpu.c` — MPU 配置（如芯片支持）
  - 设备驱动文件（`drvuart.c`、`drvled.c` 等）

- [ ] **修改 `svcrt_board_config.h`**：
  - `SVCRT_CPU_ARCH` — 设为对应的架构常量
  - `SVCRT_SYSTEM_CLOCK_HZ` — 设为实际系统时钟频率
  - `SVCRT_SHARE_MEM_ADDR` / `SVCRT_SHARE_MEM_SIZE` — 根据实际 RAM 布局调整

- [ ] **修改 Scatter File**：根据芯片实际内存映射调整

- [ ] **修改 MDK 工程文件**：
  - 更新源文件路径指向新的 `board/<chip>/` 目录
  - 更新 Include Paths
  - 更新预定义宏（`SVCRT_USE_FPU`、`SVCRT_BOARD_CONFIG` 等）

- [ ] **修改 `main.c`**：保留 HAL 初始化，替换内核启动入口

- [ ] **修改 `stm32f4xx_it.c`**：移除冲突的中断处理器

- [ ] **编译验证**：解决类型兼容性问题（如 `SystemCoreClock` 声明）

---

## 8. 已知问题与修复记录

### 8.1 多任务调度异常（2026.05.30 修复）

**现象**：两个任务同时运行时，task2 只执行一次 `svcrt_task_wait` 后不再被调度，task1 正常运行。

**根因**：`svcrt_sched_activate()` 中的栈溢出检测逻辑不区分任务状态，在任务处于 WAIT 状态时也检查栈底标志，导致误判为栈溢出并将任务标记为 INVALID。

**修复**：只在任务处于 RUNNING 状态时执行栈溢出检测：

```c
// svcrt_task.c: svcrt_sched_activate()
if(svcrt_task_table[tid].status == SVCRT_TASK_RUNNING)
{
    #if (SVCRT_USE_STACK_CHECK == 1)
    if(*svcrt_task_table[tid].stack_bottom != SVCRT_STACK_END_FLAG_VAL)
    {
        svcrt_task_table[tid].status = SVCRT_TASK_INVALID;
    }
    else
    #endif
    {
        svcrt_task_table[tid].status = SVCRT_TASK_READY;
    }
}
// WAIT 状态不做检查，等待唤醒
```

**相关修复**：`svcrt_tick_tasks()` 中严格分离 `wait_time` 和 `period_time` 两种等待机制，避免互相干扰。

### 8.1b 栈溢出检测误判（2026.05.30 修复）

**现象**：引入信号量/互斥锁后，任务通过 SVC 调用同步原语（多层 SVC 嵌套 + FPU 栈帧）时被误判为栈溢出，标记为 INVALID 后不再调度。

**根因**：原栈溢出检测使用"栈底魔术字节"法（检查 `*stack_bottom != SVCRT_STACK_END_FLAG_VAL`），在深调用链下该字节可能被合法栈使用覆盖或判断不可靠，造成误判。

**修复**：改用 **PSP 越界检测**——只有保存的栈指针真正越过栈底地址才判定溢出：

```c
// svcrt_task.c: svcrt_sched_activate()
if(old_psp < (uint32)svcrt_task_table[tid].stack_bottom)
{
    svcrt_task_table[tid].status = SVCRT_TASK_INVALID;
}
```

此方法直接比较实际栈指针与栈底地址，准确且无误判。

### 8.2 任务等待机制说明

SVCrtOS 支持两种任务等待方式：

| 函数 | 等待机制 | 唤醒条件 |
|------|---------|---------|
| `svcrt_task_wait(ms)` | `wait_time` 递减 | `wait_time <= 0` 时唤醒 |
| `svcrt_task_wait_period()` | `period_time` 递减 | `period_time <= 0` 时唤醒并重载 |

**重要**：两种机制独立运行，同一任务不应混用。`svcrt_tick_tasks()` 会根据 `wait_time > 0` 判断走哪个分支。

---

## 9. 内核架构说明

### 9.1 分层架构

```
┌─────────────────────────────────────────────────────────┐
│                    应用程序 (App)                        │
│                  仅包含 svcrt.h                          │
├─────────────────────────────────────────────────────────┤
│                    内核 API 层                           │
│              svcrt_task_wait / svcrt_dev_open           │
│                  (通过 SVC 调用进入内核)                  │
├─────────────────────────────────────────────────────────┤
│                    内核核心层                            │
│     svcrt_task.c / svcrt_dev.c / svcrt_event.c          │
│              (架构无关，零芯片依赖)                       │
├─────────────────────────────────────────────────────────┤
│                    硬件抽象层                            │
│                   svcrt_hal.h                           │
│            (定义 CPU/中断/定时器/上下文接口)              │
├──────────────────────┬──────────────────────────────────┤
│     Port 层          │          Board 层                │
│  svcrt_port.c        │      svcrt_board.c               │
│ (架构相关实现)        │    (板级初始化+设备注册)          │
│  context_rvds.S      │      drvled.c / drvuart.c        │
└──────────────────────┴──────────────────────────────────┘
```

### 9.2 关键数据结构

**任务控制块 (svcrt_task_t)**：

| 字段 | 说明 |
|------|------|
| `stack_ptr` | 当前栈指针（上下文保存位置） |
| `stack_bottom` | 栈底地址（用于溢出检测） |
| `status` | 任务状态（INVALID/READY/WAIT/RUNNING） |
| `priority` | 优先级（数值越小优先级越高） |
| `wait_time` | 等待时间（tick 数，用于 `svcrt_task_wait`） |
| `period_time` | 周期时间（tick 数，用于 `svcrt_task_wait_period`） |
| `touch_tick` | 最后一次被调度的时间戳（用于同优先级轮转） |

### 9.3 调度算法

1. **优先级调度**：每次调度选择 READY/RUNNING 状态中优先级最高（priority 值最小）的任务
2. **同优先级轮转**：优先级相同时，选择 `touch_tick` 最小的任务（最久未被调度）
3. **状态转换**：
   - `READY` → `RUNNING`：被 `svcrt_sched_activate` 选中
   - `RUNNING` → `READY`：被抢占或主动让出（`svcrt_sched_activate` 中转换）
   - `RUNNING` → `WAIT`：调用 `svcrt_task_wait` 或 `svcrt_task_wait_period`
   - `WAIT` → `READY`：`svcrt_tick_tasks` 中等待时间到期
   - `*` → `INVALID`：栈溢出检测失败或 HardFault
