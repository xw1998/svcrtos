# SVCrtOS 内核文件详解

## 1. 概述

SVCrtOS 是一个面向 Cortex-M 系列微控制器的实时操作系统内核，采用 **SVC 系统调用 + PendSV 上下文切换** 的经典架构，实现用户态/内核态分离。内核代码（`kernelsrc/`）零芯片依赖，所有硬件操作通过 `svcrt_hal.h` 定义的端口接口由 port 层和 board 层实现。

本文档逐文件详解内核源码的设计思路、数据结构和函数逻辑。

---

## 2. 文件总览

```
kernelsrc/
├── include/                    # 头文件（接口定义）
│   ├── svcrt.h                 # 应用层统一入口（唯一对外头文件）
│   ├── svcrt_types.h           # 基础类型定义
│   ├── svcrt_config.h          # 内核编译配置（类似 rtconfig.h）
│   ├── svcrt_def.h             # 内核内部常量（SVC号、句柄标志）
│   ├── svcrt_hal.h             # 硬件抽象层接口（port 层契约）
│   ├── svcrt_port.h            # 兼容层（重定向到 svcrt_hal.h）
│   ├── svcrt_task.h            # 任务管理（TCB、调度器接口）
│   ├── svcrt_event.h           # 事件模块
│   ├── svcrt_dev.h             # 设备驱动框架
│   ├── svcrt_fifo.h            # 环形缓冲区
│   ├── svcrt_cfg.h             # 任务配置加载
│   └── svcrt_mpu.h             # MPU 内存保护
│
├── src/                        # 内核源文件（架构无关）
│   ├── svcrt_task.c            # 任务调度与 SVC 服务（核心）
│   ├── svcrt_dev.c             # 设备驱动框架
│   ├── svcrt_event.c           # 事件管理
│   ├── svcrt_fifo.c            # FIFO 实现
│   ├── svcrt_cfg.c             # 任务表与栈初始化
│   └── svcrt_init.c            # 默认启动入口（可选）
│
└── port/                       # 架构适配层
    └── arm/cortex-m4/
        └── svcrt_port.c        # Cortex-M4 硬件抽象实现
```

---

## 3. 头文件详解

### 3.1 svcrt_types.h — 基础类型

最底层的类型定义文件，不依赖任何 MCU 头文件：

```c
typedef unsigned long  uint32;
typedef signed   long  int32;
typedef unsigned short uint16;
typedef signed   short int16;
typedef unsigned char  uint8;
typedef signed   char  int8;
```

**设计意图**：内核不使用 `<stdint.h>`，避免引入编译器特定头文件，保持最大可移植性。

### 3.2 svcrt.h — 应用层统一入口

应用程序只需 `#include "svcrt.h"` 即可使用所有 OS 接口。所有函数通过 SVC 指令进入内核态执行：

| 接口分类 | 函数 | SVC 号 |
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
| 设备IO | `svcrt_dev_open(name, param)` | 0x10 |
| | `svcrt_dev_read/write/close/ctrl` | 0x10 |

**设计意图**：应用层头文件不暴露任何内核内部结构（TCB、调度器等），实现接口与实现的完全隔离。

### 3.3 svcrt_config.h — 内核编译配置

类似 RT-Thread 的 `rtconfig.h`，所有可裁剪参数集中定义：

| 配置项 | 默认值 | 说明 |
|--------|--------|------|
| `SVCRT_CPU_ARCH` | 1 (Cortex-M4) | CPU 架构选择 |
| `SVCRT_USE_FPU` | 1 | FPU 浮点单元开关 |
| `SVCRT_USE_MPU` | 1 | MPU 内存保护开关 |
| `SVCRT_USE_PRIV` | 跟随 MPU | 特权模式分离 |
| `SVCRT_TASK_MAX_NUM` | 7 | 最大任务数 |
| `SVCRT_TICK_PERIOD_US` | 500 | 系统节拍周期（微秒） |
| `SVCRT_EVENT_NUM` | 10 | 最大事件数 |
| `SVCRT_MAX_EVENT_WAITERS` | 4 | 每事件最大等待任务数 |
| `SVCRT_DEV_MAX_NUM` | 8 | 最大设备数 |
| `SVCRT_USE_CPU_LOAD` | 1 | CPU 负载统计开关 |
| `SVCRT_USE_STACK_CHECK` | 1 | 栈溢出检测开关 |
| `SVCRT_STACK_END_FLAG` | 0xed01 | 栈底标志值 |

**覆盖机制**：通过编译选项 `-DSVCRT_BOARD_CONFIG=\"svcrt_board_config.h\"` 在文件末尾 `#include` 板级配置，覆盖默认值。

**时间转换宏**：
```c
#define SVCRT_MS_TO_TICK(ms)  ((ms) * 1000 / SVCRT_TICK_PERIOD_US)
// 例：SVCRT_TICK_PERIOD_US=500 时，1ms = 2 ticks
```

### 3.4 svcrt_def.h — 内核内部常量

定义 SVC 调用号和句柄标志：

```c
#define SVCRT_SVC_DEV_IO        (0x10)    // 设备IO系统调用
#define SVCRT_SVC_TASK_CTRL     (0x11)    // 任务控制系统调用
#define SVCRT_SVC_SYS_INFO      (0x12)    // 系统信息系统调用
#define SVCRT_SVC_EVENT_CTRL    (0x13)    // 事件控制系统调用

#define SVCRT_DEV_HANDLE_FLAG   (0x01200000)   // 设备句柄标志
#define SVCRT_EVENT_HANDLE_FLAG (0x01100000)   // 事件句柄标志
#define SVCRT_HANDLE_MASK       (0xfff00000)   // 句柄类型掩码
#define SVCRT_HANDLE_RELMASK    (0x000fffff)   // 句柄索引掩码
```

**句柄设计**：句柄 = 类型标志 | 索引。通过 `handle & SVCRT_HANDLE_MASK` 判断类型，`handle & SVCRT_HANDLE_RELMASK` 获取索引。

### 3.5 svcrt_hal.h — 硬件抽象层接口

内核与硬件的完整解耦契约，分为五个层次：

**1) CPU 指令层**：
```c
void svcrt_port_wfi(void);     // 等待中断
void svcrt_port_wfe(void);     // 等待事件
void svcrt_port_isb(void);     // 指令同步屏障
void svcrt_port_dsb(void);     // 数据同步屏障
void svcrt_port_dmb(void);     // 数据内存屏障
```

**2) 中断控制层**：
```c
void svcrt_port_disable_irq(void);   // 关中断
void svcrt_port_enable_irq(void);    // 开中断
void svcrt_port_switch_task(void);   // 触发 PendSV 任务切换
```

**3) 上下文层**：
```c
void  svcrt_port_set_psp(uint32 val);                    // 设置 PSP
uint32 svcrt_port_get_control(void);                     // 读 CONTROL
void  svcrt_port_set_control(uint32 val);                // 写 CONTROL
uint32 svcrt_port_stack_init(uint32 top, void (*entry)(void));  // 初始化栈帧
void  svcrt_port_enter_idle(uint32 psp, uint32 priv);    // 切换到 idle
```

**4) 定时器层**：
```c
uint32 svcrt_port_get_system_clock(void);   // 获取主频
uint32 svcrt_port_get_systick_val(void);    // 读 SysTick->VAL
uint32 svcrt_port_get_systick_load(void);   // 读 SysTick->LOAD
void  svcrt_port_start_timer(uint32 us);    // 启动定时器
void  svcrt_port_delay_us(uint32 us);       // 微秒忙等
```

**5) 板级层**：
```c
void svcrt_port_board_init(void);           // 板卡初始化
void svcrt_port_irq_init(void);             // 中断控制器初始化
void svcrt_port_enable_fpu(void);           // FPU 使能
void svcrt_port_set_idle_mpu(...);          // idle MPU 区域
```

**MPU 接口**（`SVCRT_USE_MPU=1` 时有效，否则宏替换为空）：
```c
void svcrt_port_mpu_init(void);
void svcrt_port_mpu_set_region(uint32 rom_addr, uint32 rom_size, uint32 ram_addr, uint32 ram_size);
void svcrt_port_mpu_set_app(uint32 *mpu_bar, uint32 *mpu_asr);
void svcrt_port_mpu_reset(void);
```

### 3.6 svcrt_task.h — 任务管理

**任务状态枚举**：
```c
typedef enum {
    SVCRT_TASK_INVALID,   // 无效（栈溢出/HardFault/被kill）
    SVCRT_TASK_READY,     // 就绪（可被调度）
    SVCRT_TASK_WAIT,      // 等待（等待超时/事件/周期）
    SVCRT_TASK_RUNNING    // 运行中
} svcrt_task_status_t;
```

**SVC 调用上下文**（由 SVC_Handler 传递）：
```c
typedef struct {
    uint32 r0, r1, r2, r3;   // 参数/返回值
    uint32 r12, lr, pc, xpsr; // 硬件自动保存
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
    uint8  priority;               // 优先级（越小越高）
    uint8  shm_attri;              // 共享内存属性
    uint32 stack_top;              // 栈顶地址
    uint32 *stack_bottom;          // 栈底指针（溢出检测）
    uint32 mpu_bar[8];             // MPU RBAR 值（MPU 开启时）
    uint32 mpu_asr[8];             // MPU RASR 值（MPU 开启时）
    svcrt_task_status_t status;    // 当前状态
    int32  period_time;            // 周期剩余 tick
    int32  wait_time;              // 等待剩余 tick
    uint32 tim_tick;               // 上次 tick 处理时间
    uint32 touch_tick;             // 上次被调度时间
    uint32 stack_ptr;              // 当前栈指针
} svcrt_task_t;
```

### 3.7 svcrt_dev.h — 设备驱动框架

**驱动接口结构**：
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
    char dev_name[8];          // 设备名（最长7字符）
    svcrt_dev_drv_t *drv;      // 驱动接口指针
    uint32 dev_num;            // 设备编号
} svcrt_dev_desc_t;
```

### 3.8 svcrt_event.h — 事件模块

```c
typedef struct {
    char name[16];                              // 事件名
    svcrt_task_t *waiting_tasks[SVCRT_MAX_EVENT_WAITERS]; // 等待任务列表
} svcrt_event_obj_t;
```

### 3.9 svcrt_fifo.h — 环形缓冲区

```c
typedef struct {
    uint16 magic;       // 魔数 0xf1f0
    uint16 wt_idx;      // 写索引
    uint16 rd_idx;      // 读索引
    uint16 size;        // 数据区大小
    uint8  data[1];     // 数据区（柔性数组）
} svcrt_fifo_t;
```

### 3.10 svcrt_cfg.h — 任务配置

```c
extern svcrt_task_t svcrt_task_table[SVCRT_TASK_MAX_NUM];  // 全局任务表
extern int32 svcrt_task_count;                               // 已注册任务数
```

---

## 4. 源文件详解

### 4.1 svcrt_task.c — 任务调度与 SVC 服务（核心）

这是内核最核心的文件，包含调度器、SVC 服务分发、tick 处理等所有关键逻辑。

#### 4.1.1 全局变量

```c
uint32 svcrt_kernel_tick = 0;        // 系统节拍计数器
int32  svcrt_current_task_id = 0;    // 当前运行任务ID（0=idle, 1~N=任务）
uint16 svcrt_cpu_load_counter = 0;   // CPU 负载计数器
uint16 svcrt_cpu_idle_millis = 0;    // CPU 空闲毫秒数
static uint32 svcrt_idle_stack_ptr = 0;  // idle 任务栈指针
```

**任务 ID 约定**：
- `0` = idle 任务（不占 task_table 槽位）
- `1~N` = 用户任务（`task_table[id-1]`）

#### 4.1.2 svcrt_kernel_tick_handler() — 系统节拍处理

由 `SysTick_Handler` 调用，每个 tick 周期执行一次：

```
svcrt_kernel_tick++           → 递增节拍计数
SVCRT_SWITCH_TASK()           → 触发 PendSV 检查是否需要切换
CPU 负载统计（可选）           → 统计非 idle 时间的占比
```

**关键**：`SVCRT_SWITCH_TASK()` 只是设置 PendSV 挂起位，实际切换在 PendSV 中断中异步执行。

#### 4.1.3 SVC_Server() — SVC 系统调用服务分发

SVC_Handler 汇编入口提取 SVC 号和参数后，调用此函数：

```
SVC 号提取：((char *)p_svc_ctx->pc)[-2]  → 从 SVC 指令机器码中提取立即数
```

**分发表**：

| SVC 号 | 子功能 | 说明 |
|--------|--------|------|
| 0x10 (DEV_IO) | p[0]=1: open, 2: read, 3: write, 4: ctrl, 5: close | 设备操作 |
| 0x11 (TASK_CTRL) | r0=1: wait, 2: wait_period, 3: delay, 4: kill | 任务控制 |
| 0x12 (SYS_INFO) | r0=1: get_time, 2: get_cpu_idle | 系统信息 |
| 0x13 (EVENT_CTRL) | p[0]=1: create, 2: wait, 3: set | 事件操作 |

**参数传递**：
- TASK_CTRL：r0=子功能号, r1=参数
- DEV_IO/SYS_INFO/EVENT_CTRL：r0 指向参数数组

**返回值**：通过修改 `p_svc_ctx->r0` 返回给调用者。

#### 4.1.4 svcrt_hardfault_handler() — 硬件错误处理

```
if(当前是用户任务)
    → 标记任务为 INVALID，触发任务切换
else(当前是 idle)
    → 死循环
```

#### 4.1.5 svcrt_sched_next() — 调度器核心

遍历所有任务，选择下一个应该运行的任务：

```
for 每个任务:
    1. svcrt_tick_tasks()  → 更新等待时间，检查是否该唤醒
    2. 如果任务是 READY 或 RUNNING:
        a. 优先级比当前最高优先级更高 → 选它
        b. 优先级相同但 touch_tick 更小 → 选它（轮转）
返回: 任务索引 (0~N-1)，-1 表示无就绪任务
```

**调度策略**：
1. **优先级抢占**：数值越小优先级越高
2. **同优先级轮转**：选最久未被调度的任务（`touch_tick` 最小）

#### 4.1.6 svcrt_tick_tasks() — 任务时间管理

每个 tick 周期由 `svcrt_sched_next` 调用，处理任务的时间递减和唤醒：

```
1. 计算逃逸 tick: escape_tick = kernel_tick - tim_tick
2. 更新 tim_tick
3. 如果 escape_tick <= 0 → 返回（时间没走）
4. 如果 status == RUNNING → 返回（运行中的任务不处理）
5. 如果 wait_time > 0:
    → wait_time -= escape_tick
    → 如果 wait_time <= 0: 唤醒为 READY
    → 返回（不再处理 period_time）
6. 否则（period 模式）:
    → period_time -= escape_tick
    → 如果 period_time <= 0: 重载 period，唤醒为 READY
```

**重要设计**：`wait_time` 和 `period_time` 两种等待机制严格分离，通过 `wait_time > 0` 判断走哪个分支，避免互相干扰。

#### 4.1.7 svcrt_sched_is_switching() — 判断是否需要切换

```
new_idx = svcrt_sched_next() + 1    → 获取下一个任务 ID
if(new_idx == current_task_id)
    → 不需要切换，更新 touch_tick，返回 -1
else
    → 需要切换，返回 new_idx
```

#### 4.1.8 svcrt_sched_activate() — 执行任务切换

由 PendSV_Handler 调用，保存旧任务上下文，加载新任务上下文：

```
1. 保存旧任务:
   if(旧任务是 idle)
       → 保存 PSP 到 svcrt_idle_stack_ptr
   else
       → 保存 PSP 到 task.stack_ptr
       → 如果 status == RUNNING:
           栈溢出检测（可选）→ INVALID
           否则 → READY
       → 如果 status == WAIT: 不修改（等待唤醒）

2. 加载新任务:
   svcrt_current_task_id = new_task
   if(新任务是用户任务)
       → 更新 touch_tick
       → 设 status = RUNNING
       → 设置 MPU 区域
       → 返回 task.stack_ptr
   else(idle)
       → 返回 svcrt_idle_stack_ptr
```

**关键修复点**：栈溢出检测只在 `status == RUNNING` 时执行。WAIT 状态下任务栈可能被正常使用（SVC/PendSV 压栈），此时检查栈底标志会误判。

#### 4.1.9 svcrt_task_wait_internal() — 定时等待

```
1. 关中断
2. 设置 wait_time = MS_TO_TICK(ms)
3. 设 status = WAIT
4. 触发 PendSV 切换
5. 开中断
```

任务被切换出去后，由 `svcrt_tick_tasks` 在每个 tick 递减 `wait_time`，到期后唤醒为 READY。

#### 4.1.10 svcrt_task_wait_period_internal() — 周期等待

```
1. 关中断
2. 设 status = WAIT（不修改 wait_time，保持为 0）
3. 触发 PendSV 切换
4. 开中断
```

任务由 `svcrt_tick_tasks` 的 period 分支管理，`period_time` 到期后唤醒并自动重载。

#### 4.1.11 svcrt_task_delay_internal() — 忙等延时

```c
void svcrt_task_delay_internal(uint32 us)
{
    svcrt_port_delay_us(us);  // 直接调用 port 层忙等
}
```

**注意**：此函数**不让出 CPU**，任务保持 RUNNING 状态，仅适用于极短延时。

#### 4.1.12 svcrt_sched_activate_higher() — 优先级抢占检查

```
if(当前是用户任务 && 当前优先级 > ck_pri)
    → 触发切换（被更高优先级抢占）
if(当前是 idle)
    → 总是触发切换
```

---

### 4.2 svcrt_dev.c — 设备驱动框架

#### 4.2.1 数据结构

```c
static svcrt_dev_desc_t svcrt_dev_list[SVCRT_DEV_MAX_NUM];   // 设备描述表
static svcrt_dev_hdr_t *svcrt_dev_handles[SVCRT_DEV_MAX_NUM]; // 设备实例句柄
static int32 svcrt_dev_count = 0;                              // 已注册设备数
```

#### 4.2.2 核心流程

**设备注册** (`svcrt_dev_register`)：
```
1. 检查名称/驱动指针有效性
2. 检查设备表是否已满
3. 检查名称是否已存在
4. 填入设备描述符，dev_count++
```

**设备打开** (`svcrt_dev_open_internal`)：
```
1. 按名称查找设备描述符
2. 调用 drv_open(dev_num, param) 获取设备实例
3. 保存实例指针到 handles[]
4. 返回句柄 = SVCRT_DEV_HANDLE_FLAG | 索引
```

**设备读写** (`svcrt_dev_read/write_internal`)：
```
1. 从句柄提取索引，验证类型标志
2. 检查 handles[idx] 是否有效
3. 调用 drv_read/drv_write(handles[idx], data, len)
```

**设备关闭** (`svcrt_dev_close_internal`)：
```
1. 调用 drv_close(handles[idx])
2. 清空 handles[idx]
```

---

### 4.3 svcrt_event.c — 事件管理

#### 4.3.1 数据结构

```c
static svcrt_event_obj_t svcrt_events[SVCRT_EVENT_NUM];  // 事件对象池
```

#### 4.3.2 核心流程

**事件创建** (`svcrt_event_create_internal`)：
```
1. 在事件池中找空闲槽位
2. 填入名称
3. 返回句柄 = SVCRT_EVENT_HANDLE_FLAG | 索引
```

**事件等待** (`svcrt_event_wait_internal`)：
```
1. 将当前任务加入事件的 waiting_tasks 列表
2. 如果 timeout_ms > 0: svcrt_task_wait_internal(timeout_ms)
3. 如果 timeout_ms == 0: svcrt_task_wait_period_internal()
```

**事件设置** (`svcrt_event_set_internal`)：
```
1. 遍历 waiting_tasks 列表
2. 将每个等待任务的状态设为 READY
3. 清空等待列表
```

---

### 4.4 svcrt_fifo.c — 环形缓冲区

**创建** (`svcrt_fifo_create`)：
```
1. 将缓冲区前 8 字节作为 FIFO 头部
2. 初始化 magic/wt_idx/rd_idx/size
3. 数据区 = buff + 8，大小 = size - 8
```

**写入** (`svcrt_fifo_write`)：
```
逐字节写入，wt_idx = (wt_idx + 1) % size
```

**读取** (`svcrt_fifo_read`)：
```
逐字节读取，rd_idx = (rd_idx + 1) % size
当 rd_idx == wt_idx 时停止（缓冲区空）
```

---

### 4.5 svcrt_cfg.c — 任务配置加载

**全局任务表**：
```c
svcrt_task_t svcrt_task_table[SVCRT_TASK_MAX_NUM];
int32 svcrt_task_count = 0;
```

**svcrt_cfg_load()**：清空任务计数，由 `main.c` 中的 `svcrt_register_tasks` 填充任务表。

**svcrt_task_stack_init()**：
```
1. 计算栈顶地址（8 字节对齐）
2. 保存 stack_bottom 和 stack_top
3. 调用 svcrt_port_stack_init() 初始化栈帧
4. 保存返回的 stack_ptr
```

---

### 4.6 svcrt_port.c — Cortex-M4 架构适配

实现 `svcrt_hal.h` 中所有架构相关函数。

#### 4.6.1 栈帧初始化

Cortex-M4 异常栈帧格式（从高地址到低地址）：

```
硬件自动保存（8 word）:
  xPSR    = 0x01000000  (Thumb 模式)
  PC      = entry       (任务入口)
  LR      = 0           (无返回)
  R12     = 0
  R3~R0   = 0

软件手动保存（8 word）:
  R11~R4  = 0
```

共 16 word = 64 字节。PSP 指向 R4 保存位置。

#### 4.6.2 idle 任务切换

```c
void svcrt_port_enter_idle(uint32 psp, uint32 use_priv)
{
    __set_PSP(psp);
    // CONTROL[1]=1: 使用 PSP, CONTROL[0]=1: 非特权模式
    __set_CONTROL(0x2 | (use_priv ? 0x1 : 0x0) | __get_CONTROL());
    __ISB();  // ISB 确保 CONTROL 写入生效
}
```

#### 4.6.3 微秒延时

基于 SysTick 实现精确忙等：
```
1. 记录 SysTick->VAL 起始值
2. 计算需要等待的时钟周期数
3. 循环读取 SysTick->VAL，计算差值
4. 处理 SysTick 向下溢出（差值为负时加 LOAD 值）
5. 累减等待周期直到为 0
```

#### 4.6.4 弱定义函数

`svcrt_port_board_init`、`svcrt_port_irq_init` 等函数在 port 层提供 `__weak` 默认实现，board 层可覆盖。

---

## 5. 完整调用链路

### 5.1 任务等待流程

```
应用调用 svcrt_task_wait(1000)
  │
  ├─ __svc(0x11)                    # SVC 指令，触发 SVC 异常
  │   └─ r0=1(子功能), r1=1000(参数)
  │
  ├─ SVC_Handler (context_rvds.S)   # 汇编入口
  │   └─ 提取 SVC 号，调用 SVC_Server()
  │
  ├─ SVC_Server()                   # C 分发函数
  │   └─ case 0x11: r0=1 → svcrt_task_wait_internal(1000)
  │
  ├─ svcrt_task_wait_internal()
  │   ├─ 关中断
  │   ├─ wait_time = MS_TO_TICK(1000) = 2000
  │   ├─ status = WAIT
  │   ├─ SVCRT_SWITCH_TASK()        # 触发 PendSV
  │   └─ 开中断
  │
  ├─ PendSV_Handler (context_rvds.S)
  │   ├─ 保存当前任务上下文（R4-R11 → PSP）
  │   ├─ 调用 svcrt_sched_is_switching()
  │   │   └─ svcrt_sched_next() → 选择下一个任务
  │   ├─ 调用 svcrt_sched_activate(new_id, old_psp)
  │   │   ├─ 保存旧任务 PSP
  │   │   ├─ status==WAIT → 不修改（等待唤醒）
  │   │   └─ 加载新任务 PSP
  │   └─ 恢复新任务上下文（PSP → R4-R11）
  │
  └─ 新任务开始执行
```

### 5.2 任务唤醒流程

```
SysTick 中断（每 500us）
  │
  ├─ SysTick_Handler()
  │   └─ svcrt_kernel_tick_handler()
  │       ├─ svcrt_kernel_tick++
  │       └─ SVCRT_SWITCH_TASK()     # 触发 PendSV
  │
  ├─ PendSV_Handler()
  │   ├─ 保存当前任务上下文
  │   ├─ svcrt_sched_is_switching()
  │   │   └─ svcrt_sched_next()
  │   │       └─ svcrt_tick_tasks()  # 遍历所有任务
  │   │           ├─ wait_time -= escape_tick
  │   │           └─ if wait_time <= 0: status = READY  ← 唤醒!
  │   │
  │   │   → 选中刚唤醒的任务（优先级最高/最久未调度）
  │   │
  │   ├─ svcrt_sched_activate()
  │   │   ├─ 旧任务: status==RUNNING → READY
  │   │   └─ 新任务: status=RUNNING, 加载 PSP
  │   └─ 恢复新任务上下文
  │
  └─ 被唤醒的任务继续执行 svcrt_task_wait() 之后的代码
```

### 5.3 设备操作流程

```
应用调用 svcrt_dev_write(led, &on, 1)
  │
  ├─ __svc(0x10)                    # SVC 指令
  │   └─ r0 指向参数数组 [5(子功能), handle, data, len]
  │
  ├─ SVC_Handler → SVC_Server()
  │   └─ case 0x10: p[0]=5 → svcrt_dev_write_internal(handle, data, len)
  │
  ├─ svcrt_dev_write_internal()
  │   ├─ 提取索引: idx = handle & 0xfffff
  │   ├─ 验证类型: handle & 0xfff00000 == 0x01200000
  │   └─ 调用 drv->drv_write(instance, data, len)
  │
  └─ drv_write → led_drv_write()    # 板级 LED 驱动
      └─ GPIOC->BSRR = ...          # 操作硬件
```

---

## 6. 中断优先级与调度关系

```
优先级    中断           作用
─────────────────────────────────────────────
0x00     SysTick        系统节拍，驱动调度
0x01     SVC            系统调用入口
0x0A     USART1         外设中断（示例）
0xFF     PendSV         上下文切换（最低优先级）
```

**设计原理**：
- SysTick 优先级最高，保证节拍精度
- SVC 次高，系统调用不被其他中断打断
- PendSV 最低，确保上下文切换在所有中断处理完成后执行

---

## 7. 内存布局

```
0x20028000 ┌────────────────────┐
           │   共享内存区        │ 32KB (SVCRT_SHARE_MEM_SIZE)
           │   (任务间通信)      │
0x20028000 └────────────────────┘
           │                    │
           │   主 SRAM          │ 160KB (应用可用)
           │   (内核+任务栈)    │
0x20000000 └────────────────────┘

0x10010000 ┌────────────────────┐
           │   CCM RAM          │ 64KB (仅 CPU 可访问)
0x10000000 └────────────────────┘

0x08000000 ┌────────────────────┐
           │   Flash            │ 1MB
0x08000000 └────────────────────┘
```

---

## 8. 配置与裁剪指南

| 功能 | 宏开关 | 关闭影响 |
|------|--------|---------|
| FPU 支持 | `SVCRT_USE_FPU=0` | 不保存/恢复浮点寄存器，节省栈空间 |
| MPU 保护 | `SVCRT_USE_MPU=0` | 无内存隔离，任务可互相访问 |
| 特权分离 | `SVCRT_USE_PRIV=0` | 所有代码在特权模式运行 |
| CPU 负载 | `SVCRT_USE_CPU_LOAD=0` | 不统计 CPU 使用率 |
| 栈溢出检测 | `SVCRT_USE_STACK_CHECK=0` | 不检测栈溢出，节省运行时开销 |

**裁剪示例**：最小配置（无 FPU、无 MPU、无负载统计）：
```c
#define SVCRT_USE_FPU          0
#define SVCRT_USE_MPU          0
#define SVCRT_USE_PRIV         0
#define SVCRT_USE_CPU_LOAD     0
#define SVCRT_USE_STACK_CHECK  0
#define SVCRT_TASK_MAX_NUM     (3)
#define SVCRT_TICK_PERIOD_US   (1000)
```
