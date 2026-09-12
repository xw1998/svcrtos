# SVCrtOS 内核文件详解
> **【时效性提示】** 本文写作于「分区表 + 加载器」架构改造之前。凡涉及
> **分区地址**、**内核入口宏（`BLED_DRV_ENTRY` 之类）**、**`app_config.c` /
> `svcrt_app_config.h`**、**手工维护的 `.sct`** 的段落，均已被下列内容取代：
>
> | 想知道 | 看哪里 |
> |---|---|
> | 今天怎么装 App / 驱动、怎么调试、崩溃了怎么办 | [docs/SVCrtOS应用安装与调试指南.md](docs/SVCrtOS应用安装与调试指南.md) |
> | 分区 / 加载器 / 镜像格式为什么这样设计 | [docs/Loader工程化落地说明.md](docs/Loader工程化落地说明.md) |
> | 全工程唯一地址源头 | [config/svcrt_partition.h](config/svcrt_partition.h) |
> | 文档总索引 | [docs/README.md](docs/README.md) |
>
> 口诀：**地址只在 `config/svcrt_partition.h` 写一次；`.sct` 由脚本生成；
> 入口由内核从分区表推导，任何地方都不要再抄第二遍地址。**

## 1. 概述

SVCrtOS 是一个面向 Cortex-M 系列的小型实时操作系统，核心特征是采用 **SVC 系统调用 + PendSV 上下文切换** 实现用户态/内核态分离。所有架构无关的内核代码集中在 `kernelsrc/` 目录，硬件相关实现通过 `svcrt_hal.h` 抽象接口隔离到 port 层与 board 层。

本文逐个文件、逐个函数地说明内核实现，便于阅读源码与二次移植。

---

## 2. 目录结构

```
kernelsrc/
├── include/                    # 内核头文件（接口与数据结构定义）
│   ├── svcrt.h                 # 应用 API 头文件（用户态唯一需要包含的）
│   ├── svcrt_types.h           # 基础整型类型定义
│   ├── svcrt_config.h          # 内核集中配置（类似 rtconfig.h）
│   ├── svcrt_def.h             # 公共常量（句柄标志、SVC 调用号等）
│   ├── svcrt_hal.h             # 硬件抽象层接口（由 port 层实现）
│   ├── svcrt_port.h            # port 层补充接口（含 MPU、板级）
│   ├── svcrt_task.h            # 任务、TCB、调度器内部接口
│   ├── svcrt_event.h           # 事件对象
│   ├── svcrt_sync.h            # 信号量与互斥锁对象及接口
│   ├── svcrt_dev.h             # 设备驱动框架接口
│   ├── svcrt_fifo.h            # 环形缓冲区接口
│   ├── svcrt_cfg.h             # 任务表与任务配置接口
│   └── svcrt_mpu.h             # MPU 内存保护相关定义
│
├── src/                        # 内核架构无关源文件
│   ├── svcrt_init.c            # 内核启动与初始化（默认 main 弱实现）
│   ├── svcrt_task.c            # 任务管理、调度器、SVC 分发、tick 处理
│   ├── svcrt_dev.c             # 设备驱动框架
│   ├── svcrt_event.c           # 事件实现
│   ├── svcrt_sync.c            # 信号量与互斥锁实现
│   ├── svcrt_fifo.c            # FIFO 环形缓冲区
│   └── svcrt_cfg.c             # 任务配置与栈初始化
│
└── port/                       # 架构相关移植层
    ├── arm/cortex-m3/
    │   ├── svcrt_context.S     # M3 上下文切换汇编（无 FPU）
    │   └── svcrt_port.c        # M3 移植层 C 实现
    └── arm/cortex-m4/
        ├── svcrt_context.S     # M4 上下文切换汇编（含 FPU 条件保存）
        └── svcrt_port.c        # M4 移植层 C 实现
```

---

## 3. 头文件详解

### 3.1 svcrt_types.h —— 基础类型定义

定义内核使用的固定宽度整型，不依赖任何 MCU 头文件：

```c
typedef unsigned long  uint32;
typedef signed   long  int32;
typedef unsigned short uint16;
typedef signed   short int16;
typedef unsigned char  uint8;
typedef signed   char  int8;
```

**设计目的**：不引入 `<stdint.h>`，保持内核对工具链与平台的最低依赖，便于在不同编译器/架构间移植。

### 3.2 svcrt.h —— 应用 API 头文件

应用程序只需 `#include "svcrt.h"` 即可使用全部 OS 接口。所有接口最终通过 SVC 指令陷入内核执行，对用户透明，用法与普通函数一致：

| 接口分类 | 函数 | 对应 SVC 号 |
|---------|------|--------|
| 任务管理 | `svcrt_task_wait(ms)` | 0x11 |
| | `svcrt_task_wait_period()` | 0x11 |
| | `svcrt_task_delay(us)` | 0x11 |
| | `svcrt_task_kill()` | 0x11 |
| 系统信息 | `svcrt_get_time_ms()` | 0x12 |
| | `svcrt_get_cpu_usage()` | 0x12 |
| 事件 | `svcrt_event_create(name)` | 0x13 |
| | `svcrt_event_wait(handle, timeout)` | 0x13 |
| | `svcrt_event_set(handle)` | 0x13 |
| 信号量 | `svcrt_sem_create/wait/post/delete` | 0x15 |
| 互斥锁 | `svcrt_mutex_create/lock/unlock/delete` | 0x15 |
| 设备 IO | `svcrt_dev_open(name, param)` | 0x10 |
| | `svcrt_dev_read/write/close/ctrl` | 0x10 |

**设计目的**：应用只看到普通函数，不感知任何内核内部结构（TCB、句柄表等），实现用户态与内核态的清晰隔离。

### 3.3 svcrt_config.h —— 内核集中配置

借鉴 RT-Thread 的 `rtconfig.h` 思路，把内核所有可裁剪、可调参数集中在此：

| 配置宏 | 默认值 | 说明 |
|--------|--------|------|
| `SVCRT_CPU_ARCH` | 1 (Cortex-M4) | CPU 架构选择 |
| `SVCRT_USE_FPU` | 1 | FPU 浮点单元使能 |
| `SVCRT_USE_MPU` | 1 | MPU 内存保护使能 |
| `SVCRT_USE_PRIV` | 依赖 MPU | 特权级分离使能 |
| `SVCRT_TASK_MAX_NUM` | 7 | 最大任务数 |
| `SVCRT_TICK_PERIOD_US` | 500 | 系统节拍周期（微秒） |
| `SVCRT_EVENT_NUM` | 10 | 事件对象数量 |
| `SVCRT_MAX_EVENT_WAITERS` | 4 | 单个事件最大等待者数 |
| `SVCRT_SEM_NUM` | 8 | 信号量对象数量 |
| `SVCRT_MTX_NUM` | 8 | 互斥锁对象数量 |
| `SVCRT_MAX_SYNC_WAITERS` | 4 | 单个信号量/互斥锁最大等待者数 |
| `SVCRT_DEV_MAX_NUM` | 8 | 最大设备数 |
| `SVCRT_USE_CPU_LOAD` | 1 | CPU 负载统计开关 |
| `SVCRT_USE_STACK_CHECK` | 1 | 栈溢出检测开关 |
| `SVCRT_STACK_END_FLAG` | 0xed01 | 栈底标志值 |
| `SVCRT_SHARE_MEM_ADDR` / `SVCRT_SHARE_MEM_SIZE` | — | **已删除**：位置改由 `config/svcrt_partition.h` 的 `SHARE_RAM_BASE` / `SHARE_RAM_SIZE` 决定 |
| `SVCRT_SYSTEM_CLOCK_HZ` | 168000000 | 系统主频 |

**板级覆盖机制**：通过编译选项 `-DSVCRT_BOARD_CONFIG=\"svcrt_board_config.h\"`，在文件末尾 `#include` 板级头文件，从而用板级配置覆盖默认值。

**节拍换算宏**：
```c
#define SVCRT_MS_TO_TICK(ms)  ((ms) * 1000 / SVCRT_TICK_PERIOD_US)
// 当 SVCRT_TICK_PERIOD_US=500 时，1ms = 2 ticks
```

### 3.4 svcrt_def.h —— 公共常量定义

集中定义 SVC 调用号与句柄相关常量：

```c
#define SVCRT_SVC_DEV_IO        (0x10)    // 设备 IO 调用
#define SVCRT_SVC_TASK_CTRL     (0x11)    // 任务控制调用
#define SVCRT_SVC_SYS_INFO      (0x12)    // 系统信息调用
#define SVCRT_SVC_EVENT_CTRL    (0x13)    // 事件控制调用
#define SVCRT_SVC_DRV_MGR       (0x14)    // 驱动管理调用
#define SVCRT_SVC_SYNC_CTRL     (0x15)    // 同步原语调用（信号量/互斥锁）

#define SVCRT_EVENT_HANDLE_FLAG (0x01100000)   // 事件句柄标志
#define SVCRT_DEV_HANDLE_FLAG   (0x01200000)   // 设备句柄标志
#define SVCRT_SEM_HANDLE_FLAG   (0x01300000)   // 信号量句柄标志
#define SVCRT_MTX_HANDLE_FLAG   (0x01400000)   // 互斥锁句柄标志
#define SVCRT_HANDLE_MASK       (0xfff00000)   // 句柄类型掩码（高位）
#define SVCRT_HANDLE_RELMASK    (0x000fffff)   // 句柄索引掩码（低位）
```

**句柄设计**：句柄 = 类型标志位 | 对象索引。用 `handle & SVCRT_HANDLE_MASK` 判断对象类型，用 `handle & SVCRT_HANDLE_RELMASK` 取出对象在表中的索引。

### 3.5 svcrt_hal.h —— 硬件抽象层接口

内核与硬件解耦的核心接口，所有实现位于 port 层，内核代码不含任何 MCU 头文件、不直接操作寄存器。分为五个层次：

**1) CPU 指令层**：
```c
void svcrt_port_wfi(void);     // 等待中断
void svcrt_port_wfe(void);     // 等待事件
void svcrt_port_nop(void);     // 空操作
void svcrt_port_isb(void);     // 指令同步屏障
void svcrt_port_dsb(void);     // 数据同步屏障
void svcrt_port_dmb(void);     // 数据存储屏障
```

**2) 中断控制层**：
```c
void svcrt_port_disable_irq(void);   // 关中断
void svcrt_port_enable_irq(void);    // 开中断
void svcrt_port_switch_task(void);   // 触发 PendSV 上下文切换
```

**3) 上下文层**：
```c
void  svcrt_port_set_psp(uint32 val);                    // 设置 PSP
uint32 svcrt_port_get_control(void);                     // 读 CONTROL
void  svcrt_port_set_control(uint32 val);                // 写 CONTROL
uint32 svcrt_port_stack_init(uint32 top, void (*entry)(void));  // 初始化栈帧
void  svcrt_port_enter_idle(uint32 psp, uint32 priv);    // 切换到 idle 上下文
```

**4) 定时器层**：
```c
uint32 svcrt_port_get_system_clock(void);   // 获取系统主频
uint32 svcrt_port_get_systick_val(void);    // 读 SysTick->VAL
uint32 svcrt_port_get_systick_load(void);   // 读 SysTick->LOAD
void  svcrt_port_start_timer(uint32 us);    // 启动系统节拍定时器
void  svcrt_port_delay_us(uint32 us);       // 微秒级忙等延时
```

**5) 板级层**：
```c
void svcrt_port_board_init(void);           // 板卡初始化
void svcrt_port_irq_init(void);             // 中断优先级初始化
void svcrt_port_enable_fpu(void);           // FPU 使能
void svcrt_port_set_idle_mpu(...);          // idle 任务 MPU 配置
```

**MPU 接口**（仅在 `SVCRT_USE_MPU=1` 时生效，定义见 svcrt_port.h）：
```c
void svcrt_port_mpu_init(void);
void svcrt_port_mpu_set_region(uint32 rom_addr, uint32 rom_size, uint32 ram_addr, uint32 ram_size);
void svcrt_port_mpu_set_app(uint32 *mpu_bar, uint32 *mpu_asr);
void svcrt_port_mpu_reset(void);
```

### 3.6 svcrt_task.h —— 任务与调度

**任务状态枚举**：
```c
typedef enum {
    SVCRT_TASK_INVALID,   // 无效（未注册/HardFault 后被 kill）
    SVCRT_TASK_READY,     // 就绪，等待被调度
    SVCRT_TASK_WAIT,      // 阻塞（延时/等事件/等信号量等）
    SVCRT_TASK_RUNNING    // 正在运行
} svcrt_task_status_t;
```

**SVC 上下文结构**（供 SVC_Handler 取参数）：
```c
typedef struct {
    uint32 r0, r1, r2, r3;   // 调用参数/返回值
    uint32 r12, lr, pc, xpsr; // 硬件自动压栈
} svcrt_svc_context_t;
```

**任务控制块 (TCB)**：
```c
typedef struct {
    uint32 ram_start;              // RAM 起始地址
    uint32 ram_size;               // RAM 大小
    uint32 stack_size;             // 栈大小
    uint32 rom_start;              // ROM 起始地址
    uint32 rom_size;               // ROM 大小
    int32  period;                 // 周期（tick 数）
    uint8  priority;               // 优先级（数值越小越高）
    uint8  shm_attri;              // 共享内存属性
    uint32 stack_top;              // 栈顶地址
    uint32 *stack_bottom;          // 栈底地址（用于溢出检测）
    uint32 mpu_bar[8];             // MPU RBAR 快照（仅 MPU 使能时存在）
    uint32 mpu_asr[8];             // MPU RASR 快照（仅 MPU 使能时存在）
    svcrt_task_status_t status;    // 任务状态
    int32  period_time;            // 当前周期剩余 tick
    int32  wait_time;              // 阻塞剩余 tick（<0 表示无限阻塞）
    uint32 tim_tick;               // 上次 tick 计算的时间戳
    uint32 touch_tick;             // 上次被调度运行的时间戳
    uint32 stack_ptr;              // 保存的栈指针
} svcrt_task_t;
```

### 3.7 svcrt_dev.h —— 设备驱动框架

**设备句柄基类**（每个驱动实例对象都以它作为首字段）：
```c
typedef struct {
    uint32 block_size;
} svcrt_dev_hdr_t;
```

**驱动接口表**：
```c
typedef struct {
    svcrt_drv_open_func    drv_open;    // 打开设备
    svcrt_drv_close_func   drv_close;   // 关闭设备
    svcrt_drv_rw_func      drv_read;    // 读设备
    svcrt_drv_rw_func      drv_write;   // 写设备
    svcrt_drv_ioctl_func   drv_ctrl;    // 控制设备
} svcrt_dev_drv_t;
```

**设备描述符**：
```c
typedef struct {
    char dev_name[8];          // 设备名（最多 7 字符 + 结束符）
    svcrt_dev_drv_t *drv;      // 驱动接口表指针
    uint32 dev_num;            // 设备编号（同类多实例区分）
} svcrt_dev_desc_t;
```

### 3.8 svcrt_event.h —— 事件对象

```c
typedef struct {
    char name[16];                                        // 事件名
    svcrt_task_t *waiting_tasks[SVCRT_MAX_EVENT_WAITERS]; // 等待该事件的任务列表
} svcrt_event_obj_t;
```

### 3.9 svcrt_sync.h —— 信号量与互斥锁对象

```c
typedef struct {
    char   name[8];
    int32  count;                              // 计数值：>0 表示可用资源数，0 表示需排队
    uint8  used;                               // 该对象是否已被占用
    svcrt_task_t *waiters[SVCRT_MAX_SYNC_WAITERS]; // 等待者列表
} svcrt_sem_obj_t;

typedef struct {
    char   name[8];
    svcrt_task_t *owner;                       // 当前持有者，0 表示未上锁
    uint8  used;                               // 该对象是否已被占用
    uint8  orig_priority;                       // 持有者原始优先级（优先级继承后用于恢复）
    svcrt_task_t *waiters[SVCRT_MAX_SYNC_WAITERS]; // 等待者列表
} svcrt_mtx_obj_t;
```

### 3.10 svcrt_fifo.h —— 环形缓冲区

```c
#define SVCRT_FIFO_MAGIC  (0xf1f0)

typedef struct {
    uint16 magic;       // 魔数 0xf1f0，用于校验缓冲区有效性
    uint16 wt_idx;      // 写索引
    uint16 rd_idx;      // 读索引
    uint16 size;        // 数据区大小
    uint8  data[1];     // 数据区（柔性数组，实际长度为 size）
} svcrt_fifo_t;
```

### 3.11 svcrt_cfg.h —— 任务表声明

```c
extern svcrt_task_t svcrt_task_table[SVCRT_TASK_MAX_NUM];  // 全局任务表
extern int32 svcrt_task_count;                              // 已注册任务数

void svcrt_cfg_load(void);
void svcrt_task_stack_init(svcrt_task_t *p_task, void (*entry)(void),
                           uint32 *stack_bottom, uint32 stack_size);
```

---

## 4. 源文件详解

### 4.1 svcrt_init.c —— 内核启动与初始化

提供默认的内核启动 `main()` 入口（弱符号），若应用使用 CubeMX 生成的 `main.c`，可自行实现强符号 `main()` 覆盖。

**默认 main() 启动顺序**：
```
1. svcrt_port_board_init()               # 板卡初始化
2. svcrt_cfg_load()                       # 加载任务配置
3. svcrt_port_irq_init()                  # 中断优先级初始化
4. svcrt_kernel_init_default()            # 内核各模块初始化
5. svcrt_port_start_timer(TICK_PERIOD_US) # 启动系统节拍
6. svcrt_start_idle_default()             # 进入 idle，启动调度
```

**svcrt_kernel_init_default()** 内部依次初始化：
```
svcrt_event_module_init()   # 事件模块
svcrt_sync_module_init()    # 信号量/互斥锁模块
svcrt_dev_module_init()     # 设备框架
svcrt_dev_board_init()      # 板级设备注册
svcrt_port_mpu_init()       # MPU 初始化（仅 MPU 使能时）
```

**svcrt_start_idle_default()**：把 idle 栈对齐后切换到 PSP 栈模式，进入 `while(1) WFI` 循环。idle 任务是 CPU 空闲时运行的最低优先级背景任务。

> 应用自定义 main() 时必须按相同顺序调用上述步骤，否则内核无法正常启动。

### 4.2 svcrt_task.c —— 任务管理、调度与 SVC 分发

本文件是内核核心，包含调度器、SVC 分发、tick 处理与任务管理。

#### 4.2.1 全局变量

```c
uint32 svcrt_kernel_tick = 0;        // 全局节拍计数
int32  svcrt_current_task_id = 0;    // 当前运行任务 ID（0=idle, 1~N=任务）
uint16 svcrt_cpu_load_counter = 0;   // CPU 负载计数
uint16 svcrt_cpu_idle_millis = 0;    // CPU 空闲毫秒数
static uint32 svcrt_idle_stack_ptr = 0;  // idle 任务栈指针
```

**任务 ID 约定**：
- `0` = idle 任务（不在 task_table 中）
- `1~N` = 应用任务（对应 `task_table[id-1]`）

#### 4.2.2 svcrt_kernel_tick_handler() —— 系统节拍处理

由 `SysTick_Handler` 调用，每个 tick 周期执行：

```
svcrt_kernel_tick++           # 节拍计数自增
SVCRT_SWITCH_TASK()           # 触发 PendSV 进行调度与切换
CPU 负载统计相关处理           # 累计 idle 时间
```

**注意**：`SVCRT_SWITCH_TASK()` 仅置位 PendSV 挂起标志，真正的切换在 PendSV 中断里执行。

#### 4.2.3 SVC_Server() —— SVC 调用分发器

SVC_Handler 取出调用者栈帧后调用本函数，按 SVC 号分发到具体内部实现：

```
SVC 号 = ((char *)p_svc_ctx->pc)[-2]  # 从 SVC 指令字节中取出立即数
```

**分发表**：

| SVC 号 | 参数约定 | 说明 |
|--------|--------|------|
| 0x10 (DEV_IO) | p[0]=1: open, 2: read, 3: write, 4: ctrl, 5: close | 设备操作 |
| 0x11 (TASK_CTRL) | r0=1: wait, 2: wait_period, 3: delay, 4: kill | 任务控制 |
| 0x12 (SYS_INFO) | r0=1: get_time, 2: get_cpu_idle | 系统信息 |
| 0x13 (EVENT_CTRL) | p[0]=1: create, 2: wait, 3: set | 事件控制 |
| 0x14 (DRV_MGR) | 驱动注册/注销 | 驱动管理 |
| 0x15 (SYNC_CTRL) | 信号量/互斥锁的 create/wait/post/lock/unlock/delete | 同步原语 |

**返回值约定**：通过写回 `p_svc_ctx->r0` 把结果返回给调用者。

#### 4.2.4 svcrt_hardfault_handler() —— 故障处理

```
if(当前是应用任务)
    → 把该任务标记为 INVALID，并强制调度切换
else(idle 出错)
    → 系统挂起
```

将出错任务隔离为 INVALID 后不再参与调度，保证其他任务继续运行。

#### 4.2.5 svcrt_sched_next() —— 选择下一个任务

按优先级抢占 + 同优先级轮转的策略选出应运行的任务：

```
for 每个任务:
    1. svcrt_tick_tasks()  # 更新该任务的计时与状态
    2. 若任务处于 READY 或 RUNNING:
        a. 优先选优先级更高（数值更小）的任务
        b. 同优先级时选 touch_tick 更小的（最久未运行）
返回: 选中的任务下标 (0~N-1)，-1 表示无任务可运行（运行 idle）
```

**调度策略**：
1. **优先级抢占**：数值越小优先级越高
2. **同优先级轮转**：选择最久未运行（`touch_tick` 最小）的任务

#### 4.2.6 svcrt_tick_tasks() —— 更新任务计时

为每个任务在被 `svcrt_sched_next` 评估时更新其阻塞计时与周期计时：

```
1. 计算流逝 tick: escape_tick = kernel_tick - tim_tick
2. 更新 tim_tick
3. 若 escape_tick <= 0 直接返回（同一 tick 内不重复处理）
4. 若 status == RUNNING 不处理（运行中任务不消耗等待时间）
5. 若 wait_time > 0:
    └ wait_time -= escape_tick
    └ 若 wait_time <= 0: 唤醒为 READY
6. 处理 period 周期:
    └ period_time -= escape_tick
    └ 若 period_time <= 0: 重载 period，唤醒为 READY
```

**关键修复**：`wait_time < 0` 表示无限阻塞（由 `svcrt_task_block_internal` 设置），此时跳过 tick 处理，只能被显式唤醒（如信号量 post）。这修复了互斥锁竞争导致任务永远不被唤醒的问题。

#### 4.2.7 svcrt_sched_is_switching() —— 判断是否需要切换

```
new_idx = svcrt_sched_next() + 1    # 转为内核任务 ID
if(new_idx == current_task_id)
    → 无需切换，更新 touch_tick 后返回 -1
else
    → 需要切换，返回 new_idx
```

#### 4.2.8 svcrt_sched_activate() —— 执行任务切换

由 PendSV_Handler 调用，完成旧任务上下文保存与新任务上下文恢复：

```
1. 保存旧任务:
   if(旧任务是 idle)
       → 保存 PSP 到 svcrt_idle_stack_ptr
   else
       → 保存 PSP 到 task.stack_ptr
       └ 若 status == RUNNING:
           栈溢出检测失败 → 置 INVALID
           否则           → 置 READY
       └ 若 status == WAIT: 保持阻塞状态不变

2. 切换到新任务:
   svcrt_current_task_id = new_task
   if(新任务是应用任务)
       → 更新 touch_tick
       → 置 status = RUNNING
       → 重配 MPU 区域
       → 返回 task.stack_ptr
   else(idle)
       → 返回 svcrt_idle_stack_ptr
```

**状态机要点**：只有 `status == RUNNING` 的任务会在被切走时降级；WAIT 任务被切走时保持阻塞，等待被显式唤醒。

#### 4.2.9 svcrt_task_wait_internal() —— 限时阻塞

```
1. 关中断
2. 设置 wait_time = MS_TO_TICK(ms)
3. 置 status = WAIT
4. 触发 PendSV 切换
5. 开中断
```

切走后由 `svcrt_tick_tasks` 按 tick 递减 `wait_time`，到期唤醒为 READY。

#### 4.2.10 svcrt_task_wait_period_internal() —— 周期等待

```
1. 关中断
2. 置 status = WAIT（不设 wait_time，置 0）
3. 触发 PendSV 切换
4. 开中断
```

由 `svcrt_tick_tasks` 的 period 逻辑处理，`period_time` 到期后周期性唤醒。

#### 4.2.11 svcrt_task_block_internal() —— 无限阻塞

设置 `wait_time = -1` 表示无限阻塞，`svcrt_tick_tasks` 对 `wait_time < 0` 的任务跳过 tick 处理，只能被显式唤醒（如信号量/互斥锁释放）。

#### 4.2.12 svcrt_task_delay_internal() —— 忙等延时

```c
void svcrt_task_delay_internal(uint32 us)
{
    svcrt_port_delay_us(us);  // 直接调用 port 层忙等
}
```

**注意**：忙等延时**不会让出 CPU**，任务保持 RUNNING，适用于极短的硬件时序延时。

#### 4.2.13 svcrt_sched_activate_higher() —— 唤醒后立即抢占

```
if(存在就绪任务 && 其优先级 > ck_pri)
    → 触发切换（让更高优先级任务立即运行）
```

用于信号量/互斥锁释放唤醒等待者后，若被唤醒任务优先级更高则立即抢占。

---

### 4.3 svcrt_dev.c —— 设备驱动框架

#### 4.3.1 全局状态

```c
static svcrt_dev_desc_t svcrt_dev_list[SVCRT_DEV_MAX_NUM];    // 设备描述符表
static svcrt_dev_hdr_t *svcrt_dev_handles[SVCRT_DEV_MAX_NUM]; // 设备实例对象表
static int32 svcrt_dev_count = 0;                             // 已注册设备数
```

#### 4.3.2 主要函数

**设备注册** (`svcrt_dev_register`)：
```
1. 校验名称/接口表有效性
2. 检查设备数未超上限
3. 检查名称未重复
4. 填入设备描述符，dev_count++
```

**设备打开** (`svcrt_dev_open_internal`)：
```
1. 按名称查找设备描述符
2. 调用 drv_open(dev_num, param) 得到实例对象
3. 把实例对象存入 handles[]
4. 返回句柄 = SVCRT_DEV_HANDLE_FLAG | 索引
```

**设备读写** (`svcrt_dev_read/write_internal`)：
```
1. 从句柄取索引，校验范围
2. 校验 handles[idx] 实例有效
3. 调用 drv_read/drv_write(handles[idx], data, len)
```

**设备关闭** (`svcrt_dev_close_internal`)：
```
1. 调用 drv_close(handles[idx])
2. 清空 handles[idx]
```

---

### 4.4 svcrt_event.c —— 事件实现

#### 4.4.1 全局状态

```c
static svcrt_event_obj_t svcrt_events[SVCRT_EVENT_NUM];  // 事件对象表
```

#### 4.4.2 主要函数

**创建事件** (`svcrt_event_create_internal`)：
```
1. 找空闲事件槽位
2. 记录事件名
3. 返回句柄 = SVCRT_EVENT_HANDLE_FLAG | 索引
```

**等待事件** (`svcrt_event_wait_internal`)：
```
1. 把当前任务加入该事件的 waiting_tasks 列表
2. 若 timeout_ms > 0: 调用 svcrt_task_wait_internal(timeout_ms)（限时等待）
3. 若 timeout_ms == 0: 调用 svcrt_task_wait_period_internal()（永久等待，直到被 set 唤醒）
```

**触发事件** (`svcrt_event_set_internal`)：
```
1. 遍历 waiting_tasks 列表
2. 把所有等待该事件的任务唤醒为 READY
3. 清空等待列表
```

---

### 4.5 svcrt_sync.c —— 信号量与互斥锁实现

#### 4.5.1 全局状态

```c
static svcrt_sem_obj_t svcrt_sems[SVCRT_SEM_NUM];  // 信号量对象表
static svcrt_mtx_obj_t svcrt_mtxs[SVCRT_MTX_NUM];  // 互斥锁对象表
```

#### 4.5.2 信号量

**wait** (`svcrt_sem_wait_internal`)：
```
1. count > 0  → count-- 立即获取
2. count == 0 → 把当前任务加入 waiters，无限阻塞等待 post
```

**post** (`svcrt_sem_post_internal`)：
```
1. waiters 非空 → 唤醒优先级最高的等待者为 READY
2. waiters 为空 → count++
3. 触发 PendSV 重新调度（被唤醒任务可能立即抢占）
```

#### 4.5.3 互斥锁（带优先级继承）

**lock** (`svcrt_mtx_lock_internal`)：
```
1. owner == 0      → 上锁成功，记录 owner 与 orig_priority
2. owner == 当前任务 → 返回 -1（不支持递归）
3. owner 优先级低于当前任务 → 提升 owner 优先级到当前任务（优先级继承）
4. 把当前任务加入 waiters，阻塞
```

**unlock** (`svcrt_mtx_unlock_internal`)：
```
1. 校验调用者是 owner（非 owner 返回 -1）
2. 若 owner 优先级被提升过，恢复其原始优先级
3. waiters 非空 → 把锁移交给优先级最高的等待者并唤醒
4. waiters 为空 → owner = 0
5. 触发 PendSV 重新调度
```

**优先级继承**：用于防止优先级反转。设想高优先级任务 H 等待低优先级任务 L 持有的锁，若不继承，中优先级任务 M 可抢占 L，使 H 被间接长时间阻塞。继承让 L 临时获得 H 的优先级，尽快释放锁。

#### 4.5.4 唤醒策略

`svcrt_waiters_pop_highest()` 遍历等待者列表，按 `priority` 选出优先级最高（数值最小）的等待者并将其从列表移除，实现优先级有序唤醒。

---

### 4.6 svcrt_fifo.c —— 环形缓冲区

**创建** (`svcrt_fifo_create`)：
```
1. 在缓冲区前 8 字节放置 FIFO 头（magic/wt_idx/rd_idx/size）
2. 数据区 = buff + 8，可用大小 = size - 8
```

**写入** (`svcrt_fifo_write`)：
```
逐字节写入，wt_idx = (wt_idx + 1) % size
写前判断是否已满，满则停止写入，不覆盖未读数据（关键修复）
```

**读取** (`svcrt_fifo_read`)：
```
逐字节读取，rd_idx = (rd_idx + 1) % size
当 rd_idx == wt_idx 表示数据已读完
```

---

### 4.7 svcrt_cfg.c —— 任务配置与栈初始化

**全局任务表定义**：
```c
svcrt_task_t svcrt_task_table[SVCRT_TASK_MAX_NUM];
int32 svcrt_task_count = 0;
```

**svcrt_cfg_load()**：加载/清零任务表，配合 `main.c` 中的 `svcrt_register_tasks` 完成任务注册。

**svcrt_task_stack_init()**：
```
1. 栈顶按 8 字节对齐
2. 记录 stack_bottom 与 stack_top
3. 调用 svcrt_port_stack_init() 构造初始栈帧
4. 保存返回的 stack_ptr 到 TCB
```

---

### 4.8 svcrt_port.c —— Cortex-M4 移植层

实现 `svcrt_hal.h` 与 `svcrt_port.h` 声明的全部硬件相关接口。

#### 4.8.1 栈帧初始化

Cortex-M4 任务首次运行时由硬件出栈恢复现场，初始栈帧布局：

```
硬件自动出栈区（8 word）:
  xPSR    = 0x01000000  (Thumb 位)
  PC      = entry       (任务入口)
  LR      = 0           (返回地址)
  R12     = 0
  R3~R0   = 0

软件保存区（8 word）:
  R11~R4  = 0
```

共 16 word = 64 字节，PSP 指向 R4 所在位置。

> **FPU 支持（SVCRT_USE_FPU=1）**：M4F 还需在软件保存区前预置 EXC_RETURN(LR)。
> 取值 `0xFFFFFFFD`（Thread 模式 + PSP + 不带 FPU 扩展帧）。`svcrt_context.S`
> 在 PendSV 切换时根据 EXC_RETURN 的 bit4 判断当前任务是否使用了 FPU，
> 若使用则额外保存/恢复浮点寄存器 S16-S31（S0-S15 由硬件自动处理）。
> 这修复了之前 FPU 上下文丢失的问题。

#### 4.8.2 idle 任务切换

```c
void svcrt_port_enter_idle(uint32 psp, uint32 use_priv)
{
    __set_PSP(psp);
    // CONTROL[1]=1: 使用 PSP, CONTROL[0]=1: 非特权模式
    __set_CONTROL(0x2 | (use_priv ? 0x1 : 0x0) | __get_CONTROL());
    __ISB();  // ISB 确保 CONTROL 写入生效
}
```

#### 4.8.3 微秒延时

基于 SysTick 计数实现忙等：
```
1. 读取 SysTick->VAL 当前值
2. 根据系统主频计算目标计数差
3. 轮询 SysTick->VAL 直到达到目标（处理回卷）
```

#### 4.8.4 弱实现板级函数

`svcrt_port_board_init`、`svcrt_port_irq_init` 等在 port 层提供 `__weak` 默认实现，板级代码可覆盖。

> **Cortex-M3 移植层**（`port/arm/cortex-m3/`）与 M4 基本一致，区别是无 FPU，
> `svcrt_context.S` 不含 S16-S31 的保存/恢复，配置应设 `SVCRT_USE_FPU=0`。

---

## 5. 关键流程时序

### 5.1 任务主动让出（wait）

```
应用调用 svcrt_task_wait(1000)
  │
  ├─ __svc(0x11)                    # SVC 指令携带 SVC 号
  │   └─ r0=1(子操作), r1=1000(参数)
  │
  ├─ SVC_Handler (svcrt_context.S)   # 异常入口
  │   └─ 取出 SVC 上下文后调用 SVC_Server()
  │
  ├─ SVC_Server()                   # C 层分发
  │   └─ case 0x11: r0=1 → svcrt_task_wait_internal(1000)
  │
  ├─ svcrt_task_wait_internal()
  │   ├─ 关中断
  │   ├─ wait_time = MS_TO_TICK(1000) = 2000
  │   ├─ status = WAIT
  │   ├─ SVCRT_SWITCH_TASK()        # 触发 PendSV
  │   └─ 开中断
  │
  ├─ PendSV_Handler (svcrt_context.S)
  │   ├─ 保存当前任务现场（R4-R11 入 PSP）
  │   ├─ 调用 svcrt_sched_is_switching()
  │   │   └─ svcrt_sched_next() → 选下一个任务
  │   ├─ 调用 svcrt_sched_activate(new_id, old_psp)
  │   │   ├─ 保存旧任务 PSP
  │   │   ├─ status==WAIT → 保持阻塞不降级
  │   │   └─ 返回新任务 PSP
  │   └─ 恢复新任务现场（PSP 出栈 R4-R11）
  │
  └─ 新任务继续运行
```

### 5.2 节拍唤醒（tick wakeup）

```
SysTick 中断（每 500us）
  │
  ├─ SysTick_Handler()
  │   └─ svcrt_kernel_tick_handler()
  │       ├─ svcrt_kernel_tick++
  │       └─ SVCRT_SWITCH_TASK()     # 触发 PendSV
  │
  ├─ PendSV_Handler()
  │   ├─ 保存当前任务现场
  │   ├─ svcrt_sched_is_switching()
  │   │   └─ svcrt_sched_next()
  │   │       └─ svcrt_tick_tasks()  # 更新各任务计时
  │   │           ├─ wait_time -= escape_tick
  │   │           └─ if wait_time <= 0: status = READY  ← 唤醒!
  │   │
  │   ├─ svcrt_sched_activate()
  │   │   ├─ 旧任务: status==RUNNING → READY
  │   │   └─ 新任务: status=RUNNING, 恢复 PSP
  │   └─ 恢复新任务现场
  │
  └─ 到期任务从 svcrt_task_wait() 返回继续执行
```

### 5.3 设备写流程

```
应用调用 svcrt_dev_write(led, &on, 1)
  │
  ├─ __svc(0x10)                    # SVC 指令
  │   └─ r0 指向参数数组 [3(子操作=write), handle, data, len]
  │
  ├─ SVC_Handler → SVC_Server()
  │   └─ case 0x10: p[0]=3 → svcrt_dev_write_internal(handle, data, len)
  │
  ├─ svcrt_dev_write_internal()
  │   ├─ 取索引: idx = handle & 0xfffff
  │   ├─ 校验类型: handle & 0xfff00000 == 0x01200000
  │   └─ 调用 drv->drv_write(instance, data, len)
  │
  └─ drv_write → led_drv_write()    # 板级 LED 驱动
      └─ GPIOC->BSRR = ...          # 操作硬件寄存器
```

---

## 6. 中断优先级与抢占关系

```
优先级    中断           用途
─────────────────────────────────────────────
0x00     SysTick        系统节拍，最高优先级
0x01     SVC            系统调用入口
0x0A     USART1         外设中断（示例）
0xFF     PendSV         上下文切换，最低优先级
```

**设计要点**：
- SysTick 最高，保证节拍准时
- SVC 负责处理应用的系统调用陷入
- PendSV 最低，保证上下文切换发生在所有其他中断处理完之后，避免中途被打断

---

## 7. 内存布局

**本节不列出具体地址。** 全工程 Flash / RAM 布局只在 `config/svcrt_partition.h`
定义一次（顶部 4 个芯片物理参数 + 各分区大小策略），其余自动推导。
查看当前布局：

```bash
python tools/gen_scatter.py --dump     # 打印各分区基址/大小
python tools/gen_scatter.py --check    # 校验无重叠、无越界
```

RAM 从低到高的分区顺序是：
`SHARE_RAM`（分区表 + 内核/用户数据交换）→ `KERNEL_RAM` → `DRIVER_RAM` → `APP_RAM`；
每个固件区内部再切出自己的栈（App 栈从 `APP_RAM` 顶部切、驱动栈从 `DRIVER_RAM` 顶部切），
且链接期的 RW 上限已扣除栈区，撞栈会在链接期而不是运行期暴露。

```
（原手写布局图已删除：地址会在配置变更后失真，一律以 gen_scatter.py --dump 为准。）
```

---

## 8. 内核裁剪指南

| 功能 | 配置宏 | 关闭后效果 |
|------|--------|---------|
| FPU 支持 | `SVCRT_USE_FPU=0` | 不保存/恢复浮点寄存器，省栈空间，但任务不能用浮点 |
| MPU 隔离 | `SVCRT_USE_MPU=0` | 任务间无内存隔离，切换更快 |
| 特权分离 | `SVCRT_USE_PRIV=0` | 任务运行在特权态（依赖 MPU） |
| CPU 负载 | `SVCRT_USE_CPU_LOAD=0` | 不统计 CPU 占用率 |
| 栈溢出检测 | `SVCRT_USE_STACK_CHECK=0` | 不检测栈溢出，省一点开销 |

**最小裁剪示例**（无 FPU、无 MPU 的精简配置）：
```c
#define SVCRT_USE_FPU          0
#define SVCRT_USE_MPU          0
#define SVCRT_USE_PRIV         0
#define SVCRT_USE_CPU_LOAD     0
#define SVCRT_USE_STACK_CHECK  0
#define SVCRT_TASK_MAX_NUM     (3)
#define SVCRT_TICK_PERIOD_US   (1000)
```