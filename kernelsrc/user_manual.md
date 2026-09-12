# SVCrtOS 使用手册
> **【时效性提示】** 本文写作于「分区表 + 加载器」架构改造之前。凡涉及
> **分区地址**、**内核入口宏（`BLED_DRV_ENTRY` 之类）**、**`app_config.c` /
> `svcrt_app_config.h`**、**手工维护的 `.sct`** 的段落，均已被下列内容取代：
>
> | 想知道 | 看哪里 |
> |---|---|
> | 今天怎么装 App / 驱动、怎么调试、崩溃了怎么办 | [../docs/SVCrtOS应用安装与调试指南.md](../docs/SVCrtOS应用安装与调试指南.md) |
> | 分区 / 加载器 / 镜像格式为什么这样设计 | [../docs/Loader工程化落地说明.md](../docs/Loader工程化落地说明.md) |
> | 全工程唯一地址源头 | [../config/svcrt_partition.h](../config/svcrt_partition.h) |
> | 文档总索引 | [../docs/README.md](../docs/README.md) |
>
> 口诀：**地址只在 `config/svcrt_partition.h` 写一次；`.sct` 由脚本生成；
> 入口由内核从分区表推导，任何地方都不要再抄第二遍地址。**

## 1. 系统概述

SVCrtOS 是一个面向ARM Cortex-M系列MCU的实时操作系统，核心特性包括：

- **优先级+时间片混合调度**：高优先级抢占 + 同优先级轮转
- **MPU内存保护**：任务间内存隔离（M4/M7），M3可无MPU运行
- **事件驱动**：任务间同步通信
- **设备驱动框架**：统一设备接口 + 动态驱动注册
- **SVC系统调用**：用户态通过SVC陷入内核态访问资源
- **可配置**：通过 `svcrt_config.h` 开关所有功能

### 1.1 系统架构

```
┌──────────────────────────────────────────────┐
│              应用分区 (App SDK)               │
│  ┌─────────┐  ┌─────────┐  ┌─────────┐      │
│  │  App 1  │  │  App 2  │  │  App N  │      │
│  │ (用户态) │  │ (用户态) │  │ (用户态) │      │
│  └────┬────┘  └────┬────┘  └────┬────┘      │
│       │SVC         │SVC         │SVC         │
├───────┼────────────┼────────────┼────────────┤
│       └────────────┼────────────┘            │
│            SVCrtOS 内核 (内核态)              │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐     │
│  │ 调度器   │ │ 事件模块 │ │ 设备框架 │     │
│  └──────────┘ └──────────┘ └─────┬────┘     │
│       │                           │          │
│  ┌────┴────┐              ┌──────┴──────┐   │
│  │ MPU模块 │              │ Driver SDK  │   │
│  └─────────┘              │ (动态注册)  │   │
│                           └─────────────┘   │
├──────────────────────────────────────────────┤
│            svcrt_port.h (移植层)              │
├──────────────────────────────────────────────┤
│              目标MCU硬件                      │
└──────────────────────────────────────────────┘
```

---

## 2. 快速开始

### 2.1 配置系统

编辑 `svcrt_config.h`：

```c
#define SVCRT_CPU_ARCH         SVCRT_ARCH_CORTEX_M4
#define SVCRT_TASK_MAX_NUM     (32)   /* 工业建议 ≥32；并受 SVCRT_TASK_TABLE_RAM_MAX 预算约束 */
#define SVCRT_TICK_PERIOD_US   (500)
#define SVCRT_EVENT_NUM        (10)
```

### 2.2 实现移植接口

实现 `svcrt_port.h` 中定义的回调函数（详见移植手册）。

### 2.3 配置 App / 驱动（已改）

> **旧写法已废弃**：早期版本要在 `appconfig.c` 里填一张写死 `ram_start` / `rom_start` 的
> `svcrt_app_cfg_table[]`，并让内核在 `svcrt_register_tasks()` 里按入口宏静态注册。
> 这两处现在都不存在了（文件与结构体已删除），**不要再按旧文档写**。

当前做法：

1. **分区与任务参数只在 `config/svcrt_partition.h` 配一次** ——
   分区基址/容量、`APP_TASK_PRIORITY`、`APP_TASK_STACK_SIZE`、
   `DRIVER_TASK_PRIORITY`、`DRIVER_TASK_STACK_SIZE` 等全在那里；
2. **App / 驱动工程里不出现任何分区地址** —— 分散加载文件由
   `tools/gen_scatter.py` 读配置头自动生成到 `build/*.sct`，
   App / 驱动工程也不包含 `config/svcrt_partition.h`；
3. **入口由内核从分区表推导** —— 上电时 `svcrt_loader_scan()` /
   `svcrt_loader_scan_driver()` 识别镜像并建任务，运行期用
   `svcrt_app_start()` / `svcrt_app_load()` / `svcrt_driver_load()` 管理。

App / 驱动侧只需要实现 `AppMain()` / `DrvMain()`，其余交给 SDK 与内核。
详细步骤见 [`../docs/SVCrtOS应用安装与调试指南.md`](../docs/SVCrtOS应用安装与调试指南.md)。

### 2.4 注册驱动

在 `svcrt_dev_board_init()` 中注册板载驱动（此函数在 `board/<芯片>/svcrt_board.c` 中实现）：

```c
void svcrt_dev_board_init(void)
{
    svcrt_dev_register("COM1", &usart_drv, 0);
    svcrt_dev_register("LED",  &led_drv,  0);
}
```

---

## 3. 任务管理

### 3.1 任务状态

```
                 ┌──────────┐
          创建 → │  READY   │ ← 周期到期/事件触发
                 └────┬─────┘
                      │ 调度器选中
                      ▼
                 ┌──────────┐
                 │ RUNNING  │
                 └────┬─────┘
                      │ svcrt_task_wait / svcrt_task_wait_period
                      ▼
                 ┌──────────┐
                 │   WAIT   │
                 └────┬─────┘
                      │ HardFault / svcrt_task_kill
                      ▼
                 ┌──────────┐
                 │ INVALID  │
                 └──────────┘
```

### 3.2 任务API

| API | 说明 | 示例 |
|-----|------|------|
| `svcrt_task_wait(ms)` | 等待指定毫秒 | `svcrt_task_wait(500);` |
| `svcrt_task_wait_period()` | 等待下一周期 | `svcrt_task_wait_period();` |
| `svcrt_task_delay(us)` | 微秒级忙等 | `svcrt_task_delay(100);` |
| `svcrt_task_kill()` | 终止当前任务 | `svcrt_task_kill();` |

### 3.3 调度策略

- **优先级抢占**：高优先级任务始终优先于低优先级
- **同优先级轮转**：同优先级任务按时间片轮转（基于touch_tick）
- **周期调度**：每个任务有独立周期，周期到期自动唤醒

### 3.4 任务配置参数

| 参数 | 说明 |
|------|------|
| `ram_start` | 任务RAM起始地址 |
| `ram_size` | 任务RAM大小 |
| `stack_size` | 任务栈大小 |
| `rom_start` | 任务代码起始地址 |
| `rom_size` | 任务代码大小 |
| `period` | 任务周期（tick数） |
| `priority` | 任务优先级（0=最高） |
| `shm_attri` | 共享内存访问属性 |

---

## 4. 设备驱动

### 4.1 设备操作API

| API | 说明 | 返回值 |
|-----|------|--------|
| `svcrt_dev_open(name, param)` | 打开设备 | 设备句柄(≥0)或-1 |
| `svcrt_dev_close(handle)` | 关闭设备，释放句柄 | 0=成功, -1=失败 |
| `svcrt_dev_read(handle, buf, len)` | 读取数据 | 实际读取字节数 |
| `svcrt_dev_write(handle, buf, len)` | 写入数据 | 实际写入字节数 |
| `svcrt_dev_ctrl(handle, code, value)` | 设备控制 | 操作结果 |

### 4.2 使用示例

```c
int32 com1 = svcrt_dev_open("COM1", 115200);
if(com1 >= 0)
{
    uint8 buf[64];
    int32 len = svcrt_dev_read(com1, buf, 64);
    svcrt_dev_write(com1, buf, len);
    svcrt_dev_close(com1);
}
```

### 4.3 开发自定义驱动（Driver SDK）

```c
#include "svcrt_driver_sdk.h"

static svcrt_dev_hdr_t* my_open(uint32 devid, uint32 param) { ... }
static int32 my_close(svcrt_dev_hdr_t *obj) { ... }
static int32 my_read(svcrt_dev_hdr_t *obj, uint8 *data, int32 len) { ... }
static int32 my_write(svcrt_dev_hdr_t *obj, uint8 *data, int32 len) { ... }
static int32 my_ctrl(svcrt_dev_hdr_t *obj, uint32 code, uint32 value) { ... }

static svcrt_dev_drv_t my_drv = {
    my_open, my_close, my_read, my_write, my_ctrl
};

void my_driver_init(void)
{
    svcrt_drv_register("MYDEV", &my_drv, 0);
}
```

### 4.4 标准控制码

| 控制码 | 值 | 说明 |
|--------|-----|------|
| `SVCRT_DEV_CTRL_OPEN` | 0x0001 | 打开设备 |
| `SVCRT_DEV_CTRL_CLOSE` | 0x0002 | 关闭设备 |
| `SVCRT_DEV_CTRL_SET_BAUD` | 0x0010 | 设置波特率 |
| `SVCRT_DEV_CTRL_SET_MODE` | 0x0011 | 设置模式 |
| `SVCRT_DEV_CTRL_GET_STATUS` | 0x0020 | 获取状态 |
| `SVCRT_DEV_CTRL_RESET` | 0x0030 | 复位设备 |

---

## 5. 事件机制

### 5.1 事件API

| API | 说明 |
|-----|------|
| `svcrt_event_create(name)` | 创建事件 |
| `svcrt_event_wait(handle, timeout)` | 等待事件 |
| `svcrt_event_set(handle)` | 触发事件 |

### 5.2 使用示例

**生产者任务：**
```c
int32 evt = svcrt_event_create("data_ready");

while(1)
{
    produce_data();
    svcrt_event_set(evt);
    svcrt_task_wait(100);
}
```

**消费者任务：**
```c
int32 evt = svcrt_event_create("data_ready");

while(1)
{
    svcrt_event_wait(evt, 0);
    consume_data();
}
```

---

## 6. 系统信息

### 6.1 系统信息API

| API | 说明 |
|-----|------|
| `svcrt_get_time_ms()` | 获取系统运行时间(毫秒) |
| `svcrt_get_cpu_usage()` | 获取CPU负载率 |

### 6.2 CPU负载统计

CPU负载统计通过 `SVCRT_USE_CPU_LOAD` 配置开关控制。开启后，内核每1024个tick采样一次CPU使用情况。

---

## 7. 内存保护（MPU）

### 7.1 MPU功能说明

当 `SVCRT_USE_MPU=1` 时：
- 每个任务只能访问自己的RAM区域和ROM区域
- 任务无法修改内核或其他任务的内存
- 非法内存访问触发MemManage_Handler，任务被标记为INVALID

### 7.2 无MPU运行

当 `SVCRT_USE_MPU=0` 时（如Cortex-M3）：
- 所有任务共享全部内存空间
- 没有内存隔离保护
- HardFault仍然会杀死出错任务
- `SVCRT_USE_PRIV` 自动关闭

---

## 8. App SDK 开发指南

### 8.1 应用开发流程

1. 包含 `svcrt.h` 头文件
2. 实现 `AppMain()` 函数
3. 在 `AppMain()` 中使用OS API
4. 编译为独立固件，烧录到指定分区

### 8.2 App SDK 文件清单

| 文件 | 用途 |
|------|------|
| `svcrt.h` | 应用API入口头文件 |
| `svcrt_types.h` | 基础类型定义 |
| `svcrt_oslib.c` | SVC系统调用封装 |
| `svcrt_app_main.c` | 入口模板（含AppMain弱定义） |
| `svcrt_app_start.s` | 应用启动汇编 |

### 8.3 完整应用示例

```c
#include "svcrt.h"

void AppMain(void)
{
    int32 com1 = svcrt_dev_open("COM1", 115200);
    int32 led  = svcrt_dev_open("LED", 0);
    int32 evt  = svcrt_event_create("sync");

    while(1)
    {
        uint8 data[64];
        int32 len = svcrt_dev_read(com1, data, 64);

        if(len > 0)
        {
            svcrt_dev_write(com1, data, len);
        }

        svcrt_event_set(evt);
        svcrt_task_wait(100);
    }
}
```

---

## 9. Driver SDK 开发指南

### 9.1 驱动开发流程

1. 包含 `svcrt_driver_sdk.h` 头文件
2. 实现 `svcrt_dev_drv_t` 中的5个函数
3. 调用 `svcrt_drv_register()` 注册驱动
4. 应用通过 `svcrt_dev_open()` 使用驱动

### 9.2 驱动接口说明

```c
typedef struct {
    svcrt_drv_open_func    drv_open;    // 打开设备，返回设备对象
    svcrt_drv_close_func   drv_close;   // 关闭设备
    svcrt_drv_read_func    drv_read;    // 读取数据
    svcrt_drv_write_func   drv_write;   // 写入数据
    svcrt_drv_ctrl_func    drv_ctrl;    // 设备控制
} svcrt_dev_drv_t;
```

### 9.3 动态注册/注销

```c
// 注册驱动
int32 ret = svcrt_drv_register("SPI1", &spi_drv, 0);
// ret: 0=成功, -1=设备表满, -2=参数无效

// 注销驱动
int32 ret = svcrt_drv_unregister("SPI1");
// ret: 0=成功, -1=设备未找到

// 查询已注册设备数
int32 count = svcrt_drv_get_count();
```

---

## 10. 配置参考

### 10.1 svcrt_config.h 完整配置项

```c
// CPU架构（必须选择一项）
#define SVCRT_CPU_ARCH            SVCRT_ARCH_CORTEX_M4

// 自动派生配置（通常无需手动修改）
#define SVCRT_USE_FPU             1    // 浮点单元
#define SVCRT_USE_MPU             1    // 内存保护
#define SVCRT_USE_PRIV            1    // 特权分离

// 调度器参数
#define SVCRT_TASK_MAX_NUM        (7)   // 最大任务数
#define SVCRT_TICK_PERIOD_US      (500) // 滴答周期(微秒)
#define SVCRT_EVENT_NUM           (10)  // 最大事件数
#define SVCRT_MAX_EVENT_WAITERS   (4)   // 每事件最大等待者

// 设备框架
#define SVCRT_DEV_MAX_NUM         (8)   // 最大设备数

// 可选功能
#define SVCRT_USE_CPU_LOAD        1     // CPU负载统计
#define SVCRT_USE_STACK_CHECK     1     // 栈溢出检测
#define SVCRT_STACK_END_FLAG      (0xED01) // 栈底标记

// 内存配置
// 注：SVCRT_SHARE_MEM_ADDR / SVCRT_SHARE_MEM_SIZE 已删除。
// 共享内存位置由 config/svcrt_partition.h 的 SHARE_RAM_BASE / SHARE_RAM_SIZE 决定。
#define SVCRT_SYSTEM_CLOCK_HZ     (168000000)  // 系统主频
```

### 10.2 Cortex-M3 最小配置

```c
#define SVCRT_CPU_ARCH         SVCRT_ARCH_CORTEX_M3
#define SVCRT_TASK_MAX_NUM     (3)
#define SVCRT_TICK_PERIOD_US   (1000)
#define SVCRT_EVENT_NUM        (4)
#define SVCRT_MAX_EVENT_WAITERS (2)
#define SVCRT_DEV_MAX_NUM      (4)
#define SVCRT_USE_CPU_LOAD     0
#define SVCRT_USE_STACK_CHECK  0
```

---

## 11. 自旋锁、临界区与栈用量分析

### 11.1 自旋锁（内核 / 驱动侧）

自旋锁用于保护**极短**的临界区（微秒级），可运行于任务上下文或中断上下文，
接口定义在 `kernelsrc/include/svcrt_spin.h`。它基于 port 层原子 CAS 实现，
在单核 Cortex-M 上退化为"原子标志位 + 可选中关中断"，
移植到 RISC-V / LoongArch 时只需实现 `svcrt_port_atomic_cas()` 等三个 port 接口，
内核与自旋锁代码无需改动。

```c
#include "svcrt_hal.h"      /* 已自动包含 svcrt_spin.h */

SVCRT_SPINLOCK_DEFINE(g_dev_lock);      /* 定义并初始化一把全局锁 */

/* 任务上下文中保护共享数据 */
void dev_write_reg(uint32 value)
{
    uint32 state;

    svcrt_spin_lock_irqsave(&g_dev_lock, &state);
    /* ... 访问共享寄存器/共享变量 ... */
    svcrt_spin_unlock_irqrestore(&g_dev_lock, state);
}

/* 中断服务程序中访问同一份数据：必须使用 irqsave 变体 */
void EXTI0_IRQHandler(void)
{
    uint32 state;

    svcrt_spin_lock_irqsave(&g_dev_lock, &state);
    /* ... 与任务共享的数据 ... */
    svcrt_spin_unlock_irqrestore(&g_dev_lock, state);
}
```

| 接口 | 说明 |
|------|------|
| `SVCRT_SPINLOCK_DEFINE(name)` | 定义并初始化全局自旋锁 |
| `svcrt_spin_init(lock)` | 初始化（或复位）一把锁 |
| `svcrt_spin_lock(lock)` | 阻塞自旋直到获取 |
| `svcrt_spin_trylock(lock)` | 非阻塞尝试获取，返回 1/0 |
| `svcrt_spin_unlock(lock)` | 释放（同一 CPU 重入时按嵌套计数递减） |
| `svcrt_spin_lock_irqsave(lock, &state)` | 关中断 + 加锁（中断与任务共用数据的标准做法） |
| `svcrt_spin_unlock_irqrestore(lock, state)` | 解锁 + 恢复中断状态 |
| `svcrt_spin_is_locked(lock)` | 查询锁是否被持有 |

> 注意事项：
> 1. 持锁期间禁止调用任何可能引起阻塞或任务切换的接口；
> 2. 中断与任务共用同一把锁时，任务侧必须使用 `irqsave` 变体，否则单核上会自死锁；
> 3. 同一 CPU 重复获取同一把锁会累加嵌套计数，解锁次数须与之匹配；
> 4. 可用 `SVCRT_USE_SPINLOCK` 配置开关关闭（关闭后头文件内容整体不编译）。

### 11.2 调度器锁（用户态临界区）

用户任务无法直接关中断，若需要保护一段**较长**的共享数据访问（不想长时间关中断），
可使用调度器锁：它只禁止任务切换，不关闭中断，等价于 RT-Thread 的 `rt_enter_critical`。

```c
void app_task(void)
{
    while(1)
    {
        svcrt_sched_lock();         /* 进入临界区：期间不会被其他任务抢占 */
        shared_counter++;
        shared_table[shared_counter & 0x0f] = svcrt_get_time_ms();
        svcrt_sched_unlock();       /* 退出临界区：恢复任务切换 */

        svcrt_task_wait(10);
    }
}
```

| API | 说明 |
|-----|------|
| `svcrt_sched_lock()` | 进入临界区（可嵌套，计数加一） |
| `svcrt_sched_unlock()` | 退出临界区（计数归零时恢复调度） |
| `svcrt_sched_lock_count()` | 查询当前嵌套层数（0 表示不在临界区） |

> 注意事项：
> 1. 临界区内**不得调用阻塞接口**（`svcrt_task_wait`、`svcrt_sem_wait` 等）。
>    内核会忽略该次阻塞请求，并记录一条 `SVCRT_FAULT_SCHEDLOCK` 故障记录，
>    可经 `svcrt_fault_record_read()` 查询定位问题代码；
> 2. 中断仍可正常响应，因此与中断共享的数据仍需配合自旋锁或原子访问。

### 11.3 任务栈用量分析

开启 `SVCRT_USE_STACK_USAGE`（默认开启）后，内核会在任务创建/重建时用
`SVCRT_STACK_FILL_PATTERN` 填充整段任务栈，并记录上下文切换时的最低栈指针，
从而给出每个任务的**峰值栈用量**，用于裁剪栈空间、排查栈溢出隐患。

```c
void app_task(void)
{
    uint32 stack[3];

    while(1)
    {
        if(svcrt_task_stack_info(2, stack) == 0)   /* 查询 2 号任务 */
        {
            /* stack[0]=栈总字节数, stack[1]=峰值已用, stack[2]=剩余 */
            if(stack[2] < stack[0] / 5)            /* 剩余不足 20% 时告警 */
            {
                svcrt_dev_write(uart, "stack low!\r\n", 13);
            }
        }
        svcrt_task_wait(1000);
    }
}
```

调试建议：

- 在系统跑满所有业务场景后读取峰值，按"峰值 × 1.3"左右设置任务栈大小，避免拍脑袋；
- 峰值接近栈总大小时，说明该任务存在栈溢出风险（配合 `SVCRT_USE_STACK_CHECK` 使用）；
- 定时器服务任务、空闲任务也会计入任务表，可一并查询其用量。

### 11.4 API 文档生成

内核头文件已按 Doxygen 风格注释，可一键生成 API 参考：

```bash
python tools/gen_api_doc.py          # 有 Doxygen 时生成 HTML，否则生成 Markdown
python tools/gen_api_doc.py --md     # 强制生成 Markdown（零依赖）
```

- 有 Doxygen：`docs/api/html/index.html`
- 无 Doxygen：`docs/api/SVCrtOS_API参考.md`

详见 `docs/README.md`（含源码 GBK/UTF-8 混用的处理说明）。
