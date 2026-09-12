# SVCrtOS API 参考

> 由 `tools/gen_api_doc.py` 自动生成，请勿手工修改。
> 安装 [Doxygen](https://www.doxygen.nl/) 后重新运行脚本，可得到带交叉引用与调用关系的 HTML 版本。

## 目录

- [kernelsrc/include/svcrt.h](#kernelsrcincludesvcrth)（41 项）
- [kernelsrc/include/svcrt_cfg.h](#kernelsrcincludesvcrt_cfgh)（1 项）
- [kernelsrc/include/svcrt_hal.h](#kernelsrcincludesvcrt_halh)（21 项）
- [kernelsrc/include/svcrt_spin.h](#kernelsrcincludesvcrt_spinh)（10 项）
- [kernelsrc/include/svcrt_task.h](#kernelsrcincludesvcrt_taskh)（1 项）
- [kernelsrc/port/arm/cortex-m3/svcrt_port.c](#kernelsrcportarmcortex-m3svcrt_portc)（7 项）
- [kernelsrc/port/arm/cortex-m4/svcrt_port.c](#kernelsrcportarmcortex-m4svcrt_portc)（7 项）
- [kernelsrc/src/svcrt_cfg.c](#kernelsrcsrcsvcrt_cfgc)（1 项）
- [kernelsrc/src/svcrt_task.c](#kernelsrcsrcsvcrt_taskc)（5 项）

---

## kernelsrc/include/svcrt.h

**SVCrtOS 应用 API 头文件（用户态唯一需要包含的头文件）**
应用程序（用户态）包含本文件即可使用全部操作系统接口。
所有调用最终通过 SVC 指令陷入内核态执行，对用户透明，
用法与普通函数一致。

### `void   svcrt_task_wait(uint32 ms);`

```c
void   svcrt_task_wait(uint32 ms);
```

**当前任务睡眠指定毫秒（阻塞调度）**
- `ms`：睡眠时间，单位毫秒
> 说明：睡眠期间任务状态为等待，CPU 交给其他就绪任务。

### `void   svcrt_task_wait_period(void);`

```c
void   svcrt_task_wait_period(void);
```

**等待当前任务的周期结束（周期任务同步用）**
> 说明：任务须在配置表中设置了周期；未到期即阻塞，到期自动唤醒。

### `void   svcrt_task_delay(uint32 us);`

```c
void   svcrt_task_delay(uint32 us);
```

**微秒级忙等延时（不引起任务切换）**
- `us`：延时时间，单位微秒
> 说明：占用 CPU，只适合极短延时（如外设时序），长延时应使用 svcrt_task_wait。

### `void   svcrt_task_kill(void);`

```c
void   svcrt_task_kill(void);
```

**终止当前任务**
> 说明：仅能终止自身；任务槽位转为 INVALID，可经 svcrt_task_recover_req 恢复。

### `uint32 svcrt_get_time_ms(void);`

```c
uint32 svcrt_get_time_ms(void);
```

**获取系统运行时间**
**返回**：自内核启动以来的毫秒数

### `uint32 svcrt_get_cpu_usage(void);`

```c
uint32 svcrt_get_cpu_usage(void);
```

**获取 CPU 空闲率**
**返回**：空闲率（百分比 0~100）
> 说明：需开启 SVCRT_USE_CPU_LOAD 配置。

### `int32  svcrt_event_create(char *name);`

```c
int32  svcrt_event_create(char *name);
```

**创建命名事件**
- `name`：事件名（字符串）
**返回**：事件句柄，失败返回负值

### `void   svcrt_event_wait(int32 handle, int32 timeout);`

```c
void   svcrt_event_wait(int32 handle, int32 timeout);
```

**等待事件触发**
- `handle`：事件句柄
- `timeout`：超时时间（ms），0 表示不等待，负值表示永久等待

### `void   svcrt_event_set(int32 handle);`

```c
void   svcrt_event_set(int32 handle);
```

**触发事件，唤醒所有等待该事件的任务**
- `handle`：事件句柄

### `int32  svcrt_sem_create(char *name, int32 init_count);`

```c
int32  svcrt_sem_create(char *name, int32 init_count);
```

**创建计数信号量**
- `name`：信号量名
- `init_count`：初始计数值
**返回**：信号量句柄，失败返回负值

### `int32  svcrt_sem_wait(int32 handle, int32 timeout);`

```c
int32  svcrt_sem_wait(int32 handle, int32 timeout);
```

**等待信号量（计数减一，计数为 0 时阻塞）**
- `handle`：信号量句柄
- `timeout`：超时时间（ms），0 表示不等待，负值表示永久等待
**返回**：0=成功，负值=超时或参数错误

### `int32  svcrt_sem_post(int32 handle);`

```c
int32  svcrt_sem_post(int32 handle);
```

**释放信号量（唤醒等待者或计数加一）**
- `handle`：信号量句柄
**返回**：0=成功，负值=失败

### `int32  svcrt_sem_delete(int32 handle);`

```c
int32  svcrt_sem_delete(int32 handle);
```

**删除信号量**
- `handle`：信号量句柄
**返回**：0=成功，负值=失败

### `int32  svcrt_mutex_create(char *name);`

```c
int32  svcrt_mutex_create(char *name);
```

**创建互斥锁（带优先级继承，防止优先级反转）**
- `name`：互斥锁名
**返回**：互斥锁句柄，失败返回负值

### `int32  svcrt_mutex_lock(int32 handle, int32 timeout);`

```c
int32  svcrt_mutex_lock(int32 handle, int32 timeout);
```

**加锁**
- `handle`：互斥锁句柄
- `timeout`：超时时间（ms），0 表示不等待，负值表示永久等待
**返回**：0=成功，负值=超时或参数错误

### `int32  svcrt_mutex_unlock(int32 handle);`

```c
int32  svcrt_mutex_unlock(int32 handle);
```

**解锁（仅持有者可解锁）**
- `handle`：互斥锁句柄
**返回**：0=成功，负值=失败

### `int32  svcrt_mutex_delete(int32 handle);`

```c
int32  svcrt_mutex_delete(int32 handle);
```

**删除互斥锁**
- `handle`：互斥锁句柄
**返回**：0=成功，负值=失败

### `int32  svcrt_dev_open(char *name, uint32 param);`

```c
int32  svcrt_dev_open(char *name, uint32 param);
```

**打开设备**
- `name`：设备名（内置驱动或独立驱动注册的名字）
- `param`：设备相关参数（由驱动解释，无参数填 0）
**返回**：设备句柄，失败返回负值

### `int32  svcrt_dev_close(int32 handle);`

```c
int32  svcrt_dev_close(int32 handle);
```

**关闭设备**
- `handle`：设备句柄
**返回**：0=成功，负值=失败

### `int32  svcrt_dev_read(int32 handle, void *pdata, int32 len);`

```c
int32  svcrt_dev_read(int32 handle, void *pdata, int32 len);
```

**从设备读取数据**
- `handle`：设备句柄
- `pdata`：接收缓冲区
- `len`：期望读取长度（字节）
**返回**：实际读取长度，负值表示错误

### `int32  svcrt_dev_write(int32 handle, void *pdata, int32 len);`

```c
int32  svcrt_dev_write(int32 handle, void *pdata, int32 len);
```

**向设备写入数据**
- `handle`：设备句柄
- `pdata`：发送缓冲区
- `len`：写入长度（字节）
**返回**：实际写入长度，负值表示错误

### `int32  svcrt_dev_ctrl(int32 handle, uint32 code, uint32 value);`

```c
int32  svcrt_dev_ctrl(int32 handle, uint32 code, uint32 value);
```

**设备控制命令**
- `handle`：设备句柄
- `code`：控制命令码（由驱动定义）
- `value`：命令参数
**返回**：0 或正值=成功，负值=失败

### `int32  svcrt_mq_create(char *name);`

```c
int32  svcrt_mq_create(char *name);
```

**创建消息队列**
- `name`：队列名
**返回**：消息队列句柄，失败返回负值
> 说明：队列深度与单条消息长度由 SVCRT_MQ_DEPTH / SVCRT_MQ_MSG_WORDS 配置。

### `int32  svcrt_mq_send(int32 handle, void *buf, int32 len_words, int32 t`

```c
int32  svcrt_mq_send(int32 handle, void *buf, int32 len_words, int32 timeout);
```

**发送消息（消息按字拷贝入队）**
- `handle`：消息队列句柄
- `buf`：消息缓冲区
- `len_words`：消息长度（以 32 位字为单位）
- `timeout`：队列满时的等待时间（ms），负值表示永久等待
**返回**：0=成功，负值=失败

### `int32  svcrt_mq_recv(int32 handle, void *buf, int32 len_words, int32 t`

```c
int32  svcrt_mq_recv(int32 handle, void *buf, int32 len_words, int32 timeout);
```

**接收消息（阻塞式）**
- `handle`：消息队列句柄
- `buf`：接收缓冲区
- `len_words`：缓冲区可容纳的字数
- `timeout`：超时时间（ms），0 表示不等待，负值表示永久等待
**返回**：实际收到的字数，负值=超时或错误

### `int32  svcrt_mq_delete(int32 handle);`

```c
int32  svcrt_mq_delete(int32 handle);
```

**删除消息队列**
- `handle`：消息队列句柄
**返回**：0=成功，负值=失败

### `int32  svcrt_timer_create(char *name);`

```c
int32  svcrt_timer_create(char *name);
```

**创建软定时器**
- `name`：定时器名
**返回**：定时器句柄，失败返回负值

### `int32  svcrt_timer_start(int32 handle, uint32 period_ms, uint32 mode,`

```c
int32  svcrt_timer_start(int32 handle, uint32 period_ms, uint32 mode,
```

**启动定时器**
- `handle`：定时器句柄
- `period_ms`：定时周期（ms）
- `mode`：0=单次触发，1=周期触发（SVCRT_TIMER_MODE_x）
- `cb`：到期回调函数
- `arg`：回调参数
**返回**：0=成功，负值=失败
> 说明：回调在专用定时器任务上下文中执行，不在中断内运行，
因此回调中可以安全调用其它系统调用。

### `int32  svcrt_timer_stop(int32 handle);`

```c
int32  svcrt_timer_stop(int32 handle);
```

**停止定时器**
- `handle`：定时器句柄
**返回**：0=成功，负值=失败

### `int32  svcrt_timer_delete(int32 handle);`

```c
int32  svcrt_timer_delete(int32 handle);
```

**删除定时器**
- `handle`：定时器句柄
**返回**：0=成功，负值=失败

### `int32  svcrt_task_status_get(int32 task_id);`

```c
int32  svcrt_task_status_get(int32 task_id);
```

**查询任务状态**
- `task_id`：任务号（从 1 开始）
**返回**：任务状态枚举值（svcrt_task_status_t），-1 表示任务号非法

### `int32  svcrt_task_recover_req(int32 task_id);`

```c
int32  svcrt_task_recover_req(int32 task_id);
```

**请求恢复（重启）指定任务**
- `task_id`：任务号（从 1 开始）
**返回**：0=成功，-1=参数非法
> 说明：重建任务栈帧并重新调度，用于任务级故障恢复。

### `int32  svcrt_fault_record_count(void);`

```c
int32  svcrt_fault_record_count(void);
```

**获取故障记录条数**
**返回**：当前环形缓冲区中已记录的故障条数

### `int32  svcrt_fault_record_read(int32 index, uint32 *out3);`

```c
int32  svcrt_fault_record_read(int32 index, uint32 *out3);
```

**读取一条故障记录**
- `index`：记录序号（从 0 开始）
- `out3`：用户缓冲区（3 个字）：[0]=故障类型，[1]=任务号，[2]=发生时刻 tick
**返回**：0=成功，负值=失败

### `int32  svcrt_sem_post_from_isr(int32 handle);`

```c
int32  svcrt_sem_post_from_isr(int32 handle);
```

**中断中释放信号量（不阻塞、不引起切换）**
- `handle`：信号量句柄
**返回**：0=成功，负值=失败
> 说明：该接口直接在内核态执行，不经 SVC，只允许中断服务程序调用。

### `int32  svcrt_event_set_from_isr(int32 handle);`

```c
int32  svcrt_event_set_from_isr(int32 handle);
```

**中断中触发事件**
- `handle`：事件句柄
**返回**：0=成功，负值=失败

### `int32  svcrt_mq_send_from_isr(int32 handle, void *buf, int32 len_words`

```c
int32  svcrt_mq_send_from_isr(int32 handle, void *buf, int32 len_words);
```

**中断中发送消息**
- `handle`：消息队列句柄
- `buf`：消息缓冲区
- `len_words`：消息长度（字）
**返回**：0=成功，负值=失败

### `void   svcrt_sched_lock(void);`

```c
void   svcrt_sched_lock(void);
```

**进入临界区（禁止任务切换，不关闭中断）**
相当于 RT-Thread 的 rt_enter_critical，支持嵌套调用。
适用于保护较长的共享数据访问，开销小于关中断。
> 说明：临界区内禁止调用阻塞接口（如 wait/sem_wait），
否则阻塞请求会被忽略并记录一条 SVCRT_FAULT_SCHEDLOCK 故障记录。

### `void   svcrt_sched_unlock(void);`

```c
void   svcrt_sched_unlock(void);
```

**退出临界区**
> 说明：须与 svcrt_sched_lock 成对调用；计数归零时恢复任务切换。

### `int32  svcrt_sched_lock_count(void);`

```c
int32  svcrt_sched_lock_count(void);
```

**查询调度器锁嵌套层数**
**返回**：当前嵌套层数（0 表示未处于临界区）

### `int32  svcrt_task_stack_info(int32 task_id, uint32 *out3);`

```c
int32  svcrt_task_stack_info(int32 task_id, uint32 *out3);
```

**查询任务栈使用峰值**
- `task_id`：任务号（从 1 开始）
- `out3`：用户缓冲区（3 个字）：
[0]=栈总字节数，[1]=峰值已用字节数，[2]=剩余字节数
**返回**：0=成功，-1=参数非法
采用两种手段并取较大值：栈填充图案扫描 + 上下文切换最低栈指针记录。
建议按峰值用量保留 30% 以上余量再减小任务栈配置。
> 说明：需开启 SVCRT_USE_STACK_USAGE 配置。

## kernelsrc/include/svcrt_cfg.h

**SVCrtOS 任务配置与任务表声明**
声明全局任务表、任务计数，以及任务配置加载与任务栈初始化接口。
svcrt_cfg_load 在内核启动时加载任务配置，svcrt_task_stack_init 初始化任务栈帧。

### `void svcrt_task_stack_fill(uint32 *stack_bottom, uint32 stack_size);`

```c
void svcrt_task_stack_fill(uint32 *stack_bottom, uint32 stack_size);
```

**用填充图案覆盖整段任务栈（栈底保护字除外）**
- `stack_bottom`：栈底地址
- `stack_size`：栈大小（字节）
> 说明：仅在任务创建/重建时调用，用于后续统计峰值栈用量。

## kernelsrc/include/svcrt_hal.h

**SVCrtOS 硬件抽象层接口**
定义内核与硬件的完整解耦接口，分为四个层次：
1) CPU指令层 - CPU核心指令封装（WFI/ISB/DSB等）
2) 上下文层 - 任务上下文结构与栈帧初始化

### `uint32 svcrt_port_svc_get_arg(void *p_exc_ctx, uint32 idx);`

```c
uint32 svcrt_port_svc_get_arg(void *p_exc_ctx, uint32 idx);
```

**读取系统调用上下文中的第 idx 个参数（0~3）**
- `p_exc_ctx`：系统调用入口传给内核的上下文指针
- `idx`：参数序号（0 对应 a0/r0）
**返回**：参数值
各架构的栈帧/trap 帧布局不同，由 port 层负责解析，
内核只通过本接口与上面的宏访问参数，不感知任何寄存器布局。

### `void   svcrt_port_svc_set_ret(void *p_exc_ctx, uint32 value);`

```c
void   svcrt_port_svc_set_ret(void *p_exc_ctx, uint32 value);
```

**写回系统调用返回值（第 0 个参数寄存器）**
- `p_exc_ctx`：系统调用上下文指针
- `value`：返回值

### `uint32 svcrt_port_enter_critical(void);`

```c
uint32 svcrt_port_enter_critical(void);
```

**进入临界区（保存中断使能状态并关中断）**
**返回**：进入前的中断状态，必须原样传给 svcrt_port_exit_critical
与 SVCRT_DISABLE_IRQ 的区别：本接口保存并恢复状态，
支持嵌套，且适配 RISC-V/LoongArch 等需要保存状态寄存器的架构。
新代码建议优先使用本接口。

### `void   svcrt_port_exit_critical(uint32 state);`

```c
void   svcrt_port_exit_critical(uint32 state);
```

**退出临界区（恢复此前保存的中断状态）**
- `state`：svcrt_port_enter_critical 的返回值

### `uint32 svcrt_port_atomic_cas(volatile uint32 *p_addr, uint32 expect, u`

```c
uint32 svcrt_port_atomic_cas(volatile uint32 *p_addr, uint32 expect, uint32 new_value);
```

**比较并交换（原子操作）**
- `p_addr`：目标地址（必须 4 字节对齐）
- `expect`：期望的当前值
- `new_value`：期望成立时写入的新值
**返回**：1=交换成功，0=当前值与 expect 不符（未修改）

### `uint32 svcrt_port_cpu_id(void);`

```c
uint32 svcrt_port_cpu_id(void);
```

**获取当前 CPU 编号（单核恒返回 0，多核返回硬件核号）**
**返回**：CPU 编号

### `void   svcrt_port_spin_hint(void);`

```c
void   svcrt_port_spin_hint(void);
```

**自旋等待提示（降低自旋总线压力，可插入 NOP/WFE/PAUSE）**

### `void svcrt_port_set_psp(uint32 val);`

```c
void svcrt_port_set_psp(uint32 val);
```

**设置进程栈指针PSP**
- `val`：PSPֵ

### `uint32 svcrt_port_get_control(void);`

```c
uint32 svcrt_port_get_control(void);
```

**获取CONTROL寄存器值**
**返回**：CONTROL寄存器当前值

### `void svcrt_port_set_control(uint32 val);`

```c
void svcrt_port_set_control(uint32 val);
```

**设置CONTROL寄存器值**
- `val`：要写入的值

### `uint32 svcrt_port_stack_init(uint32 stack_top, void (*entry)(void));`

```c
uint32 svcrt_port_stack_init(uint32 stack_top, void (*entry)(void));
```

**初始化任务栈帧**
- `stack_top`：栈顶地址（高地址，已8字节对齐）
- `entry`：任务入口函数
**返回**：初始化后的栈指针（将存入task->stack_ptr）
不同架构的异常栈帧格式不同：
- ARM Cortex-M: 压入xPSR/PC/LR/R12/R3-R0(硬件) + R4-R11(软件)
- RISC-V: 压入mepc/mstatus/x1-x31
此函数由port层实现，内核无需关心具体布局

### `void svcrt_port_enter_idle(uint32 stack_ptr, uint32 use_priv);`

```c
void svcrt_port_enter_idle(uint32 stack_ptr, uint32 use_priv);
```

**切换到空闲任务上下文**
设置PSP/CONTROL寄存器，切换到PSP栈模式
不同架构的特权级切换机制不同，由port层实现
- `psp`：空闲任务栈顶地址
- `use_priv`：是否使用特权分离模式

### `uint32 svcrt_port_get_system_clock(void);`

```c
uint32 svcrt_port_get_system_clock(void);
```

**获取系统主频(Hz)**
**返回**：系统主频，单位Hz
不同平台获取方式不同：
- STM32: SystemCoreClock变量
- 其他MCU: 可能是宏或函数
内核通过此接口统一获取，不直接依赖SystemCoreClock

### `uint32 svcrt_port_get_timer_counter(void);`

```c
uint32 svcrt_port_get_timer_counter(void);
```

**获取系统节拍定时器当前计数值（自由递减计数器）**
**返回**：当前计数值
ARM 为 SysTick->VAL，RISC-V 可为 mtime 低 32 位，
LoongArch 可为恒定频率定时器计数，由 port 层映射。

### `uint32 svcrt_port_get_timer_reload(void);`

```c
uint32 svcrt_port_get_timer_reload(void);
```

**获取系统节拍定时器重载值（计数上限）**
**返回**：重载值
ARM 为 SysTick->LOAD，其他架构为等效周期值。

### `void svcrt_port_start_timer(uint32 tick_period_us);`

```c
void svcrt_port_start_timer(uint32 tick_period_us);
```

**启动系统节拍定时器**
- `tick_period_us`：节拍周期（微秒）

### `void svcrt_port_delay_us(uint32 us);`

```c
void svcrt_port_delay_us(uint32 us);
```

**微秒级忙等延时**
- `us`：延时微秒数
使用硬件定时器实现精确延时，
由port层根据具体定时器实现

### `void svcrt_port_board_init(void);`

```c
void svcrt_port_board_init(void);
```

**板卡硬件初始化**
初始化CPU协处理器（如FPU）、时钟等

### `void svcrt_port_irq_init(void);`

```c
void svcrt_port_irq_init(void);
```

**中断控制器初始化**
配置NVIC优先级分组，设置PendSV/SysTick/SVC优先级

### `void svcrt_port_enable_fpu(void);`

```c
void svcrt_port_enable_fpu(void);
```

**FPU使能**
当 SVCRT_USE_FPU=1 时由port层实现

### `void svcrt_port_set_idle_mpu(uint32 task_func, uint32 stack_addr, uint`

```c
void svcrt_port_set_idle_mpu(uint32 task_func, uint32 stack_addr, uint32 stack_size);
```

**设置空闲任务MPU区域**
当 SVCRT_USE_MPU=1 时由port层实现
- `task_func`：任务函数地址
- `stack_addr`：栈地址
- `stack_size`：栈大小

## kernelsrc/include/svcrt_spin.h

**SVCrtOS 自旋锁与临界区（内核/驱动侧）**
提供架构无关的自旋锁实现，用于：
1) 内核内部短小临界区保护（对象表、设备表等）；
2) 中断服务程序与任务共享数据的保护；

### `typedef struct {`

```c
typedef struct {
```

**自旋锁对象**
lock 为原子访问的锁字（0 空闲 / 1 占用），owner 记录持有者 CPU，
nest 记录同一 CPU 的嵌套层数。

### `#define SVCRT_SPINLOCK_INIT          { 0u, 0u, 0u }`

```c
#define SVCRT_SPINLOCK_INIT          { 0u, 0u, 0u }
```

**自旋锁静态初始化值（用于定义全局锁对象） */**

### `#define SVCRT_SPINLOCK_DEFINE(name)  svcrt_spinlock_t name = SVCRT_SPI`

```c
#define SVCRT_SPINLOCK_DEFINE(name)  svcrt_spinlock_t name = SVCRT_SPINLOCK_INIT
```

**定义并初始化一把全局自旋锁**
- `name`：锁变量名
SVCRT_SPINLOCK_DEFINE(g_dev_lock);

### `static inline void svcrt_spin_init(svcrt_spinlock_t *p_lock) {`

```c
static inline void svcrt_spin_init(svcrt_spinlock_t *p_lock) {
```

**初始化（或重新初始化）自旋锁**
- `p_lock`：锁对象指针
> 说明：仅可在确认无人持有该锁时调用。

### `static inline int32 svcrt_spin_trylock(svcrt_spinlock_t *p_lock) {`

```c
static inline int32 svcrt_spin_trylock(svcrt_spinlock_t *p_lock) {
```

**尝试获取自旋锁（非阻塞）**
- `p_lock`：锁对象指针
**返回**：1=获取成功，0=锁已被其他 CPU 持有
> 说明：同一 CPU 重入时直接累加嵌套计数并返回成功，避免自死锁。

### `static inline void svcrt_spin_lock(svcrt_spinlock_t *p_lock) {`

```c
static inline void svcrt_spin_lock(svcrt_spinlock_t *p_lock) {
```

**获取自旋锁（阻塞自旋直到成功）**
- `p_lock`：锁对象指针
> 说明：只能在任务或中断上下文短暂自旋，禁止在持锁期间调用任何
可能引起任务切换或长时间阻塞的接口。

### `static inline void svcrt_spin_unlock(svcrt_spinlock_t *p_lock) {`

```c
static inline void svcrt_spin_unlock(svcrt_spinlock_t *p_lock) {
```

**释放自旋锁**
- `p_lock`：锁对象指针
> 说明：非持有者调用时直接返回（防御性处理，不破坏锁状态）。

### `static inline void svcrt_spin_lock_irqsave(svcrt_spinlock_t *p_lock, u`

```c
static inline void svcrt_spin_lock_irqsave(svcrt_spinlock_t *p_lock, uint32 *p_state) {
```

**关中断并获取自旋锁（中断与任务共用数据的标准做法）**
- `p_lock`：锁对象指针
- `p_state`：输出参数，保存进入前的中断状态，供解锁时恢复
> 说明：顺序为「先关中断，再抢锁」，与 unlock_irqrestore 严格对称。

### `static inline void svcrt_spin_unlock_irqrestore(svcrt_spinlock_t *p_lo`

```c
static inline void svcrt_spin_unlock_irqrestore(svcrt_spinlock_t *p_lock, uint32 state) {
```

**释放自旋锁并恢复中断状态**
- `p_lock`：锁对象指针
- `state`：svcrt_spin_lock_irqsave 保存的中断状态

### `static inline int32 svcrt_spin_is_locked(svcrt_spinlock_t *p_lock) {`

```c
static inline int32 svcrt_spin_is_locked(svcrt_spinlock_t *p_lock) {
```

**查询自旋锁当前是否被持有**
- `p_lock`：锁对象指针
**返回**：1=已持有，0=空闲

## kernelsrc/include/svcrt_task.h

**SVCrtOS 任务与调度相关定义（内核内部头文件）**
定义任务控制块、任务状态、SVC 上下文结构，以及任务管理和调度器函数。
这些是内核内部接口，应用/驱动一般不直接包含本文件，而是通过 SDK 或 svcrt.h 使用。
其中 stack_ptr 字段用于保存/恢复任务的栈指针，是上下文切换的关键。

### `typedef struct {`

```c
typedef struct {
```

**任务控制块（TCB）**
保存一个任务的全部运行信息：内存区域、优先级、栈、状态、调度计时等。
所有任务的 TCB 排成 svcrt_task_table 数组。其中 stack_ptr 在任务被切走时
保存其 PSP，切回时据此恢复现场；mpu 是该任务的 MPU 区域快照。

## kernelsrc/port/arm/cortex-m3/svcrt_port.c

**SVCrtOS Cortex-M3 架构适配层**
实现 svcrt_hal.h 中声明的所有架构相关函数。
Cortex-M3 无FPU、MPU可选。
此文件仅依赖CMSIS核心头文件，不依赖任何厂商HAL库。

### `uint32 svcrt_port_atomic_cas(volatile uint32 *p_addr, uint32 expect, u`

```c
uint32 svcrt_port_atomic_cas(volatile uint32 *p_addr, uint32 expect, uint32 new_value)
```

**比较并交换（原子操作）**
- `p_addr`：目标地址（4 字节对齐）
- `expect`：期望值
- `new_value`：期望成立时写入的新值
**返回**：1=成功，0=当前值不是 expect（未修改）

### `uint32 svcrt_port_cpu_id(void)`

```c
uint32 svcrt_port_cpu_id(void)
```

**获取当前 CPU 编号**
**返回**：单核 MCU 固定返回 0，多核移植时返回硬件核号

### `void svcrt_port_spin_hint(void)`

```c
void svcrt_port_spin_hint(void)
```

**自旋等待提示**
> 说明：单核无总线争用，用 NOP 即可；多核可改为 __WFE() 降低功耗与争用。

### `uint32 svcrt_port_enter_critical(void)`

```c
uint32 svcrt_port_enter_critical(void)
```

**进入临界区，保存 PRIMASK 并关中断**
**返回**：进入前的中断状态

### `void svcrt_port_exit_critical(uint32 state)`

```c
void svcrt_port_exit_critical(uint32 state)
```

**退出临界区，恢复进入前的中断状态**

### `uint32 svcrt_port_svc_get_arg(void *p_exc_ctx, uint32 idx)`

```c
uint32 svcrt_port_svc_get_arg(void *p_exc_ctx, uint32 idx)
```

**读取系统调用参数（0~3，对应 R0~R3）**
Cortex-M 硬件压栈顺序为 R0,R1,R2,R3,R12,LR,PC,xPSR，
使用栈帧指针直接索引即可。

### `void svcrt_port_svc_set_ret(void *p_exc_ctx, uint32 value)`

```c
void svcrt_port_svc_set_ret(void *p_exc_ctx, uint32 value)
```

**写回系统调用返回值（R0）**

## kernelsrc/port/arm/cortex-m4/svcrt_port.c

**SVCrtOS Cortex-M4 移植层实现**
实现 svcrt_hal.h 中声明的所有硬件抽象接口，包括：
CPU 控制、中断开关、寄存器访问、任务栈帧初始化、
系统时钟与微秒延时、MPU 配置等。

### `uint32 svcrt_port_atomic_cas(volatile uint32 *p_addr, uint32 expect, u`

```c
uint32 svcrt_port_atomic_cas(volatile uint32 *p_addr, uint32 expect, uint32 new_value)
```

**比较并交换（原子操作）**
- `p_addr`：目标地址（4 字节对齐）
- `expect`：期望值
- `new_value`：期望成立时写入的新值
**返回**：1=成功，0=当前值不是 expect（未修改）

### `uint32 svcrt_port_cpu_id(void)`

```c
uint32 svcrt_port_cpu_id(void)
```

**获取当前 CPU 编号**
**返回**：单核 MCU 固定返回 0，多核移植时返回硬件核号

### `void svcrt_port_spin_hint(void)`

```c
void svcrt_port_spin_hint(void)
```

**自旋等待提示**
> 说明：单核无总线争用，用 NOP 即可；多核可改为 __WFE() 降低功耗与争用。

### `uint32 svcrt_port_enter_critical(void)`

```c
uint32 svcrt_port_enter_critical(void)
```

**进入临界区，保存 PRIMASK 并关中断**
**返回**：进入前的中断状态

### `void svcrt_port_exit_critical(uint32 state)`

```c
void svcrt_port_exit_critical(uint32 state)
```

**退出临界区，恢复进入前的中断状态**

### `uint32 svcrt_port_svc_get_arg(void *p_exc_ctx, uint32 idx)`

```c
uint32 svcrt_port_svc_get_arg(void *p_exc_ctx, uint32 idx)
```

**读取系统调用参数（0~3，对应 R0~R3）**
Cortex-M 硬件压栈顺序为 R0,R1,R2,R3,R12,LR,PC,xPSR，
使用栈帧指针直接索引即可。

### `void svcrt_port_svc_set_ret(void *p_exc_ctx, uint32 value)`

```c
void svcrt_port_svc_set_ret(void *p_exc_ctx, uint32 value)
```

**写回系统调用返回值（R0）**

## kernelsrc/src/svcrt_cfg.c

**SVCrtOS 任务配置加载模块**
管理任务表和任务栈初始化。
栈帧格式由port层的 svcrt_port_stack_init() 实现，
内核不再硬编码任何架构特定的寄存器布局。

### `void svcrt_task_stack_fill(uint32 *stack_bottom, uint32 stack_size) {`

```c
void svcrt_task_stack_fill(uint32 *stack_bottom, uint32 stack_size) {
```

**用填充图案覆盖整段任务栈（跳过下标 0 的栈底保护字）**
- `stack_bottom`：栈底地址
- `stack_size`：栈大小（字节）
任务运行过程中会不断覆盖该图案，查询栈用量时扫描仍保持图案的
连续区域即可得到“从未使用过”的栈空间，换算后即为峰值用量。

## kernelsrc/src/svcrt_task.c

**SVCrtOS 任务调度与系统调用分发实现**
实现任务调度器、上下文切换的 C 侧逻辑、SVC 系统调用分发，以及任务控制接口。
中断入口（SysTick_Handler/HardFault_Handler）在 board/ 中实现，
它们最终调用本文件的 svcrt_kernel_tick_handler / svcrt_hardfault_handler。

### `static int32 svcrt_sched_lock_blocks(void) {`

```c
static int32 svcrt_sched_lock_blocks(void) {
```

**检查调度器锁是否被持有（阻塞类接口的编程错误防护）**
**返回**：1=已锁定（调用方应忽略本次阻塞请求），0=未锁定
在调度器锁定期间调用阻塞接口会破坏任务状态一致性，
此处忽略请求并记录一条故障记录，便于定位问题代码。

### `void svcrt_sched_lock_internal(void) {`

```c
void svcrt_sched_lock_internal(void) {
```

**获取调度器锁（用户任务经 SVC 调用）**
累加嵌套计数后禁止任务切换，但不关闭中断。
临界区内不得调用阻塞接口（会被忽略并记录故障）。

### `uint32 svcrt_sched_unlock_internal(void) {`

```c
uint32 svcrt_sched_unlock_internal(void) {
```

**释放调度器锁**
**返回**：剩余嵌套层数（0 表示已完全解锁）
计数归零时主动触发一次任务切换，补偿锁定期间被推迟的调度。

### `int32 svcrt_sched_lock_count_internal(void) {`

```c
int32 svcrt_sched_lock_count_internal(void) {
```

**查询调度器锁嵌套层数**
**返回**：当前嵌套层数（0 表示未锁定）

### `int32 svcrt_task_stack_info_internal(int32 task_id, uint32 *out3) {`

```c
int32 svcrt_task_stack_info_internal(int32 task_id, uint32 *out3) {
```

**查询任务栈使用情况（峰值法）**
- `task_id`：任务号（从 1 开始）
- `out3`：输出数组：out3[0]=总字节，out3[1]=峰值已用字节，out3[2]=剩余字节
**返回**：0=成功，-1=参数非法
同时使用两种手段并取较大值：
1) 扫描填充图案得到“从未触及”区域；
2) 上下文切换记录的历史最低栈指针。
