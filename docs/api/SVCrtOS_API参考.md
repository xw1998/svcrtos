# SVCrtOS API 参考

> 由 `tools/gen_api_doc.py` 自动生成，请勿手工修改。
> 安装 [Doxygen](https://www.doxygen.nl/) 后重新运行脚本，可得到带交叉引用与调用关系的 HTML 版本。

## 目录

- [kernelsrc/include/svcrt.h](#kernelsrcincludesvcrth)（46 项）
- [kernelsrc/include/svcrt_app_image.h](#kernelsrcincludesvcrt_app_imageh)（6 项）
- [kernelsrc/include/svcrt_cfg.h](#kernelsrcincludesvcrt_cfgh)（1 项）
- [kernelsrc/include/svcrt_hal.h](#kernelsrcincludesvcrt_halh)（24 项）
- [kernelsrc/include/svcrt_init.h](#kernelsrcincludesvcrt_inith)（1 项）
- [kernelsrc/include/svcrt_installer.h](#kernelsrcincludesvcrt_installerh)（1 项）
- [kernelsrc/include/svcrt_loader.h](#kernelsrcincludesvcrt_loaderh)（12 项）
- [kernelsrc/include/svcrt_ptable.h](#kernelsrcincludesvcrt_ptableh)（4 项）
- [kernelsrc/include/svcrt_share.h](#kernelsrcincludesvcrt_shareh)（4 项）
- [kernelsrc/include/svcrt_spin.h](#kernelsrcincludesvcrt_spinh)（10 项）
- [kernelsrc/include/svcrt_task.h](#kernelsrcincludesvcrt_taskh)（3 项）
- [kernelsrc/port/arm/cortex-m3/svcrt_port.c](#kernelsrcportarmcortex-m3svcrt_portc)（7 项）
- [kernelsrc/port/arm/cortex-m4/svcrt_port.c](#kernelsrcportarmcortex-m4svcrt_portc)（7 项）
- [kernelsrc/src/svcrt_cfg.c](#kernelsrcsrcsvcrt_cfgc)（1 项）
- [kernelsrc/src/svcrt_installer.c](#kernelsrcsrcsvcrt_installerc)（1 项）
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
**返回**：0=成功；SVCRT_SYNC_ERR_TIMEOUT(-2)=等待超时（未获得资源）；其它负值=参数错误

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
**返回**：0=成功；SVCRT_SYNC_ERR_TIMEOUT(-2)=等待超时（未获得资源）；其它负值=参数错误

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
- `timeout`：队列满时的等待时间（ms），0 表示不等待，负值表示永久等待
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

### `int32  svcrt_app_load(int32 dev, uint32 image_len);`

```c
int32  svcrt_app_load(int32 dev, uint32 image_len);
```

**从设备加载 App 镜像到空闲槽位**
- `dev`：已打开的设备句柄（数据从镜像头开始）
- `image_len`：期望镜像总长（含头），传 0 表示由镜像头决定
**返回**：成功返回槽位号(>=0)，失败返回负错误码（见 svcrt_loader.h）
> 说明：镜像会先擦后写并做 CRC 回读校验。

### `int32  svcrt_app_start(uint32 slot);`

```c
int32  svcrt_app_start(uint32 slot);
```

**启动槽位中的 App**
- `slot`：槽位号
**返回**：成功返回任务号(>0)，失败返回负错误码

### `int32  svcrt_app_stop(uint32 slot);`

```c
int32  svcrt_app_stop(uint32 slot);
```

**停止槽位中的 App（镜像保留在 Flash）**
- `slot`：槽位号
**返回**：0=成功，负值为错误码

### `uint32 svcrt_app_status(uint32 slot);`

```c
uint32 svcrt_app_status(uint32 slot);
```

**查询槽位状态**
- `slot`：槽位号
**返回**：0=空（EMPTY）,1=已加载（LOADED）,2=运行中（RUNNING）；
槽位非法返回 0xffffffff

### `int32  svcrt_driver_load(int32 dev, uint32 image_len);`

```c
int32  svcrt_driver_load(int32 dev, uint32 image_len);
```

**从设备安装驱动镜像到驱动区**
- `dev`：已打开的设备句柄（数据从镜像头开始）
- `image_len`：期望镜像总长（含头），传 0 表示由镜像头决定
**返回**：0=成功，负值为错误码（见 svcrt_loader.h）
> 说明：镜像头的 type 必须为驱动（SVCRT_APP_TYPE_DRIVER）；
驱动区为单入口，写入前会整体擦除目标区间。

## kernelsrc/include/svcrt_app_image.h

**SVCrtOS App 镜像文件格式（打包工具与 Loader 共用）**
App 镜像 = 固定 256 字节头 + 代码/数据段原始内容。
镜像由 App 工程编译产物转换而来（见 tools/pack_app.py），
Loader 按本结构解析、校验并写入 App 分区。

### `#define SVCRT_APP_MAGIC           (0x53564341u)`

```c
#define SVCRT_APP_MAGIC           (0x53564341u)
```

**App 镜像头魔数 "SVCA" */**

### `#define SVCRT_APP_HEADER_SIZE     (256u)`

```c
#define SVCRT_APP_HEADER_SIZE     (256u)
```

**App 镜像头固定长度（字节） */**

### `#define SVCRT_APP_TYPE_APP        (1u) #define SVCRT_APP_TYPE_DRIVER  `

```c
#define SVCRT_APP_TYPE_APP        (1u) #define SVCRT_APP_TYPE_DRIVER     (2u)
```

**镜像类型 */**

### `typedef struct {`

```c
typedef struct {
```

**App 镜像头（固定 256 字节）**
> 说明：crc32 覆盖范围：本头结构体（crc32 字段自身置 0）+ 头之后的镜像数据。

### `typedef char svcrt_app_header_size_check[ (sizeof(svcrt_app_header_t) `

```c
typedef char svcrt_app_header_size_check[ (sizeof(svcrt_app_header_t) == SVCRT_APP_HEADER_SIZE) ? 1 : -1];
```

**镜像头尺寸静态校验**
头结构与 SVCRT_APP_HEADER_SIZE 必须严格一致：打包工具与 Loader 分别按
“结构体布局”和“固定长度宏”解释镜像，一旦不一致就会静默错位。
此断言让不一致在编译期直接暴露。

### `uint32 svcrt_crc32(const void *data, uint32 len, uint32 crc);`

```c
uint32 svcrt_crc32(const void *data, uint32 len, uint32 crc);
```

**计算 CRC32（与打包工具使用同一算法：IEEE 802.3 反射多项式）**
- `data`：数据指针
- `len`：数据长度（字节）
- `crc`：前一段的 CRC 值（首段传 0）
**返回**：累积的 CRC32 值

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

### `void svcrt_port_resume_task(uint32 stack_ptr);`

```c
void svcrt_port_resume_task(uint32 stack_ptr);
```

**在异常处理程序内部直接恢复目标任务上下文并异常返回**
- `stack_ptr`：目标任务栈指针（指向其保存的 R4-R11[/EXC_RETURN]）
故障恢复等“已经处于异常处理程序中、不能依赖 PendSV”的场景专用：
Cortex-M 上 HardFault 优先级高于 PendSV，故障处理程序活动期间
PendSV 不会被服务，必须由本函数直接完成恢复 + 异常返回。
本函数不返回；stack_ptr 为 0 时调用方应自行停机，不要调用。

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

### `void svcrt_port_set_idle_mpu(uint32 task_func, uint32 stack_addr, uint`

```c
void svcrt_port_set_idle_mpu(uint32 task_func, uint32 stack_addr, uint32 stack_size);
```

**设置空闲任务MPU区域**
当 SVCRT_USE_MPU=1 时由port层实现
- `task_func`：任务函数地址
- `stack_addr`：栈地址
- `stack_size`：栈大小

### `int32  svcrt_port_flash_erase(uint32 addr, uint32 size);`

```c
int32  svcrt_port_flash_erase(uint32 addr, uint32 size);
```

**擦除 Flash 区间（按扇区擦除，自动对齐到扇区边界）**
- `addr`：起始地址
- `size`：字节长度
**返回**：0=成功，-1=失败

### `int32  svcrt_port_flash_write(uint32 addr, const uint8 *data, uint32 l`

```c
int32  svcrt_port_flash_write(uint32 addr, const uint8 *data, uint32 len);
```

**写入 Flash（按字节编程，写入前该区间必须已擦除）**
- `addr`：起始地址
- `data`：数据指针
- `len`：字节长度
**返回**：0=成功，-1=失败

### `uint32 svcrt_port_flash_sector_size(uint32 addr);`

```c
uint32 svcrt_port_flash_sector_size(uint32 addr);
```

**获取指定地址所在扇区的大小（字节）**
- `addr`：Flash 地址
**返回**：扇区大小；地址非法返回 0

## kernelsrc/include/svcrt_init.h

**SVCrtOS 内核启动初始化接口**
把"内核需要初始化哪些模块、以什么顺序"收敛到唯一一处实现
（svcrt_init.c 的 svcrt_kernel_module_init），避免出现两份启动流程
（内核默认入口与工程 main）各自演化、

### `void svcrt_kernel_module_init(void);`

```c
void svcrt_kernel_module_init(void);
```

**初始化全部内核模块，并注册内置服务任务**
顺序：事件 → 同步 → 消息队列 → 软定时器 → 故障记录 → 设备 → 板级设备，
然后注册软定时器服务任务，最后（需要时）初始化 MPU。
必须在 svcrt_cfg_load() 之后、调度启动之前调用。
> 说明：任何启动路径都应调用本函数，不要在工程侧重复逐个调用各模块 init。

## kernelsrc/include/svcrt_installer.h

**SVCrtOS 内核内安装任务（方案A）**
常驻任务从设备（默认 COM1）接收 SVCrtOS 镜像流并安装到空闲槽位，
使 App 的落位方式从「烧录器刷固件」变为「设备自己安装」。
接收协议（与 tools/pack_app.py 的输出直接对接）：

### `int32 svcrt_installer_init(void);`

```c
int32 svcrt_installer_init(void);
```

**初始化并注册安装任务**
**返回**：成功返回任务号（>0）；INSTALLER_ENABLE 为 0 时返回 0
需在调度启动之前调用（建议跟在 svcrt_loader_scan() 之后）。

## kernelsrc/include/svcrt_loader.h

**SVCrtOS App 镜像加载器（分区内加载 / 校验 / 启动）**
负责把 App 镜像写入空闲槽位，并按镜像头信息校验后拉起为任务。
镜像格式见 svcrt_app_image.h；分区布局运行期从共享内存分区表获取
（见 svcrt_ptable.h），本模块不硬编码任何地址。

### `int32 svcrt_loader_load_buffer(const uint8 *image, uint32 image_len);`

```c
int32 svcrt_loader_load_buffer(const uint8 *image, uint32 image_len);
```

**从内存缓冲区加载完整 App 镜像**
- `image`：指向镜像起始（含 256 字节头）的缓冲区
- `image_len`：缓冲区长度（字节），必须 >= 头长 + image_size
**返回**：成功返回槽位号（>=0），失败返回 SVCRT_LOADER_ERR_x

### `int32 svcrt_loader_load_dev(int32 dev, uint32 image_len);`

```c
int32 svcrt_loader_load_dev(int32 dev, uint32 image_len);
```

**从设备流式加载 App 镜像**
- `dev`：已打开的设备句柄（数据从镜像头开始）
- `image_len`：期望的镜像总长（含头）；传 0 表示由镜像头中的 image_size 决定
**返回**：成功返回槽位号（>=0），失败返回 SVCRT_LOADER_ERR_x
> 说明：边读边写 Flash，仅使用固定大小分块缓冲，不要求整镜像驻留 RAM。

### `uint32 svcrt_loader_scan(void);`

```c
uint32 svcrt_loader_scan(void);
```

**扫描全部槽位，把 Flash 中已存在且校验通过的镜像标记为 LOADED**
**返回**：有效（可启动）的槽位数量
槽位状态原本只存在于共享 RAM，重启后会丢失；本函数在启动时按
镜像头魔数 + 硬件兼容签名 + CRC32 重新认定槽位，使“先烧录镜像、
再上电运行”的最小闭环成立。

### `uint32 svcrt_loader_scan_driver(void);`

```c
uint32 svcrt_loader_scan_driver(void);
```

**扫描驱动区（DRIVER_POOL），认定其中的驱动镜像**
**返回**：1=驱动镜像有效且可启动，0=无有效驱动
与 App 槽位共用同一套认定规则：带头的 .svcapp 或开发期裸镜像。

### `int32 svcrt_loader_start_driver(void);`

```c
int32 svcrt_loader_start_driver(void);
```

**启动驱动区的驱动（单驱动：DRIVER_POOL 内一个入口）**
**返回**：成功返回任务号（>0），失败返回 SVCRT_LOADER_ERR_x
栈从 DRIVER_RAM 区顶部切出（与 App 同一套路），
参数取 DRIVER_TASK_PRIORITY / STACK_SIZE / PERIOD_MS。

### `int32 svcrt_loader_load_dev_hdr(int32 dev, const svcrt_app_header_t *p`

```c
int32 svcrt_loader_load_dev_hdr(int32 dev, const svcrt_app_header_t *p_hdr, uint32 image_len);
```

**从设备流式加载（镜像头已由调用方读出）**
- `dev`：已打开的设备句柄，位置正好在镜像头之后
- `p_hdr`：已读出的镜像头（调用方已完成魔数同步）
- `image_len`：期望镜像总长（含头），传 0 表示由镜像头决定
**返回**：成功返回槽位号（>=0），失败返回 SVCRT_LOADER_ERR_x
供安装任务使用：先逐字节同步到镜像头魔数，再把头交给本函数继续
流式写入负载，避免整镜像驻留 RAM。

### `int32 svcrt_loader_load_driver(int32 dev, uint32 image_len);`

```c
int32 svcrt_loader_load_driver(int32 dev, uint32 image_len);
```

**从设备安装驱动镜像（自行读头）**
- `dev`：已打开的设备句柄，位置在镜像头之前
- `image_len`：期望镜像总长（含头），传 0 表示由镜像头决定
**返回**：成功返回 0，失败返回 SVCRT_LOADER_ERR_x
> 说明：镜像头的 type 必须为 SVCRT_APP_TYPE_DRIVER；驱动区为单入口，
写入前会整体擦除目标区间。

### `int32 svcrt_loader_load_driver_dev(int32 dev, const svcrt_app_header_t`

```c
int32 svcrt_loader_load_driver_dev(int32 dev, const svcrt_app_header_t *p_hdr, uint32 image_len);
```

**从设备安装驱动镜像（镜像头已由调用方读出）**
**返回**：成功返回 0，失败返回 SVCRT_LOADER_ERR_x

### `int32 svcrt_loader_on_fault(int32 task_id);`

```c
int32 svcrt_loader_on_fault(int32 task_id);
```

**按“崩溃重启上限”策略处理 App 任务故障**
- `task_id`：发生故障的任务号
**返回**：1=已达上限并已禁用该 App，0=未达上限（调用方应恢复/重启该 App），
-1=不属于任何 App 槽位（内核任务，调用方沿用默认处理）
计数按槽位累计：故障一次加一，达到 APP_CRASH_RESTART_MAX 后
将该槽位置为 INVALID 并让任务脱离调度（不再重启）。
重新安装镜像时计数清零。
> 说明：计数保存在共享 RAM，掉电即清零，因此当前可挡住“App 反复崩溃重启”，
但擋不住“崩溃导致整机复位”的启动环——那需要把计数持久化（如备份寄存器）。

### `int32 svcrt_loader_start(uint32 slot);`

```c
int32 svcrt_loader_start(uint32 slot);
```

**把已加载的槽位拉起为任务**
- `slot`：槽位号
**返回**：成功返回任务号（>0），失败返回 SVCRT_LOADER_ERR_x

### `int32 svcrt_loader_stop(uint32 slot);`

```c
int32 svcrt_loader_stop(uint32 slot);
```

**停止槽位对应的任务（镜像仍保留在 Flash）**
- `slot`：槽位号
**返回**：0=成功，负值为 SVCRT_LOADER_ERR_x

### `uint32 svcrt_loader_state(uint32 slot);`

```c
uint32 svcrt_loader_state(uint32 slot);
```

**查询槽位状态**
- `slot`：槽位号
**返回**：SVCRT_APP_SLOT_x；槽位非法返回 0xffffffff

## kernelsrc/include/svcrt_ptable.h

**SVCrtOS 分区表运行期接口（内核侧）**
把 config/svcrt_partition.h 中的编译期布局，在启动时镜像到共享内存的
svcrt_partition_table_t 中，供内核、Loader、App 在运行期统一读取。
设计原则：

### `void svcrt_ptable_init(void);`

```c
void svcrt_ptable_init(void);
```

**初始化共享内存中的分区表**
用编译期布局填充布局字段，并把所有槽位状态清为空。
必须在任务调度启动之前调用（建议在 svcrt_kernel_init 阶段）。

### `svcrt_partition_table_t *svcrt_ptable_get(void);`

```c
svcrt_partition_table_t *svcrt_ptable_get(void);
```

**获取分区表指针**
**返回**：指向共享内存中分区表的指针；未初始化时同样返回有效地址

### `int32 svcrt_ptable_read_header(uint32 slot, svcrt_app_header_t *out);`

```c
int32 svcrt_ptable_read_header(uint32 slot, svcrt_app_header_t *out);
```

**读取指定槽位 Flash 中的 App 镜像头**
- `slot`：槽位号（0 ~ app_max_count-1）
- `out`：输出镜像头（可为 0，仅做存在性校验）
**返回**：0=成功且镜像有效，-1=槽位非法，-2=镜像头魔数错误
> 说明：Flash 已被映射到地址空间，可直接按地址读取。

### `int32 svcrt_ptable_set_slot(uint32 slot, uint32 state, uint32 entry, u`

```c
int32 svcrt_ptable_set_slot(uint32 slot, uint32 state, uint32 entry, uint32 task_id);
```

**更新槽位运行期状态**
- `slot`：槽位号
- `state`：SVCRT_APP_SLOT_x
- `entry`：入口地址（直接赋值，清空槽位时传 0）
- `task_id`：任务号（直接赋值，未启动时传 0）
**返回**：0=成功，-1=槽位非法

## kernelsrc/include/svcrt_share.h

**SVCrtOS 共享内存分区表（内核 / Loader / App 运行期接口）**
分区布局在编译期由 config/svcrt_partition.h 唯一定义，但该头文件
**只允许内核工程包含**。Loader 与 App 工程在运行期通过共享内存中的
本结构体获取布局信息（SVC 0x18 子命令 1 返回其地址），

### `#define SVCRT_PARTITION_MAGIC     (0x50415254u)`

```c
#define SVCRT_PARTITION_MAGIC     (0x50415254u)
```

**分区表魔数 "PART"，用于校验共享内存中的分区表是否有效 */**

### `#define SVCRT_PARTITION_VERSION   (1u)`

```c
#define SVCRT_PARTITION_VERSION   (1u)
```

**分区表结构版本 */**

### `#define SVCRT_APP_SLOT_EMPTY      (0u)    /* 空槽位 */ #define SVCRT_APP_`

```c
#define SVCRT_APP_SLOT_EMPTY      (0u)    /* 空槽位 */ #define SVCRT_APP_SLOT_LOADED     (1u)    /* 已写入镜像并通过校验 */ #define SVCRT_APP_SLOT_RUNNING    (2u)    /* 已注册为任务并运行 */ #define SVCRT_APP_SLOT_INVALID    (3u)    /* 槽位有内容但校验失败（魔数/兼容签名/CRC 不符） */ #define SVCRT_APP_SLOT_INSTALLING (4u)    /* 正在安装：写入未完成，不可启动（掉电后由 CRC 判定为 INVALID） */
```

**App 槽位状态 */**

### `typedef struct {`

```c
typedef struct {
```

**分区表（存放于共享内存起始处）**
由内核在启动时填充静态布局，运行期由 Loader 更新槽位状态。
Loader / App 只读布局字段，写状态字段前须确认自己拥有该槽位。

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

### `static inline void svcrt_spin_init(svcrt_spinlock_t *p_lock)`

```c
static inline void svcrt_spin_init(svcrt_spinlock_t *p_lock)
```

**初始化（或重新初始化）自旋锁**
- `p_lock`：锁对象指针
> 说明：仅可在确认无人持有该锁时调用。

### `static inline int32 svcrt_spin_trylock(svcrt_spinlock_t *p_lock)`

```c
static inline int32 svcrt_spin_trylock(svcrt_spinlock_t *p_lock)
```

**尝试获取自旋锁（非阻塞）**
- `p_lock`：锁对象指针
**返回**：1=获取成功，0=锁已被其他 CPU 持有
> 说明：同一 CPU 重入时直接累加嵌套计数并返回成功，避免自死锁。

### `static inline void svcrt_spin_lock(svcrt_spinlock_t *p_lock)`

```c
static inline void svcrt_spin_lock(svcrt_spinlock_t *p_lock)
```

**获取自旋锁（阻塞自旋直到成功）**
- `p_lock`：锁对象指针
> 说明：只能在任务或中断上下文短暂自旋，禁止在持锁期间调用任何
可能引起任务切换或长时间阻塞的接口。

### `static inline void svcrt_spin_unlock(svcrt_spinlock_t *p_lock)`

```c
static inline void svcrt_spin_unlock(svcrt_spinlock_t *p_lock)
```

**释放自旋锁**
- `p_lock`：锁对象指针
> 说明：非持有者调用时直接返回（防御性处理，不破坏锁状态）。

### `static inline void svcrt_spin_lock_irqsave(svcrt_spinlock_t *p_lock, u`

```c
static inline void svcrt_spin_lock_irqsave(svcrt_spinlock_t *p_lock, uint32 *p_state)
```

**关中断并获取自旋锁（中断与任务共用数据的标准做法）**
- `p_lock`：锁对象指针
- `p_state`：输出参数，保存进入前的中断状态，供解锁时恢复
> 说明：顺序为「先关中断，再抢锁」，与 unlock_irqrestore 严格对称。

### `static inline void svcrt_spin_unlock_irqrestore(svcrt_spinlock_t *p_lo`

```c
static inline void svcrt_spin_unlock_irqrestore(svcrt_spinlock_t *p_lock, uint32 state)
```

**释放自旋锁并恢复中断状态**
- `p_lock`：锁对象指针
- `state`：svcrt_spin_lock_irqsave 保存的中断状态

### `static inline int32 svcrt_spin_is_locked(svcrt_spinlock_t *p_lock)`

```c
static inline int32 svcrt_spin_is_locked(svcrt_spinlock_t *p_lock)
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

### `int32 svcrt_task_block_in_critical(uint32 timeout_ms);`

```c
int32 svcrt_task_block_in_critical(uint32 timeout_ms);
```

**在调用方已持有的临界区内阻塞当前任务（供信号量/互斥锁/消息队列使用）**
- `timeout_ms`：>0 定时等待（ms）；<=0 无限等待（只能被显式唤醒）
> 说明：这是内核内部原语，不直接对外。调用方（sem/mtx/mq/event）必须先把
对外 API 的“0=不等待、负值=永久等待”约定归一化后再调用，
不能把对外的 0 直接传进来（那会被本原语解释成无限等待）。
**返回**：0=被显式唤醒（已获得资源），1=等待超时，-1=未能进入阻塞（调用方需自行摘除队列）
调用约定：进入时中断已关，返回时中断仍关（调用方在同一临界区内继续处理等待队列）。
与 svcrt_task_wait_internal 的区别：后者会自行开关中断，
对“先把自己挂进等待队列、再睡下”的原语来说，那中间存在唤醒丢失窗口
（ISR 的唤醒会投给一个还没睡下的任务）。本函数把置状态与切走放在同一临界区内。

### `void svcrt_task_release_resources(int32 task_id);`

```c
void svcrt_task_release_resources(int32 task_id);
```

**任务下线收尸：清理该任务在同步对象/消息队列中的等待登记与锁持有关系**
- `task_id`：目标任务号（从 1 开始）
用于任务自杀（kill）、故障恢复、覆盖安装前硬停任务等场景。
不做收尸的后果：post/unlock 会把一个已经不在等待的任务置为 READY
（从旧栈“复活”）；它持有的互斥锁永久锁死，等待者全部饿死。
本函数自带保存-恢复语义的临界区，可在已有临界区内安全调用。

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

### `void svcrt_task_stack_fill(uint32 *stack_bottom, uint32 stack_size)`

```c
void svcrt_task_stack_fill(uint32 *stack_bottom, uint32 stack_size)
```

**用填充图案覆盖整段任务栈（跳过下标 0 的栈底保护字）**
- `stack_bottom`：栈底地址
- `stack_size`：栈大小（字节）
任务运行过程中会不断覆盖该图案，查询栈用量时扫描仍保持图案的
连续区域即可得到“从未使用过”的栈空间，换算后即为峰值用量。

## kernelsrc/src/svcrt_installer.c

**SVCrtOS 内核内安装任务实现（方案A）**
安装流程：
1) 打开镜像接收设备（默认 COM1），**只打开一次并常驻持有句柄**；
2) 在字节流中搜索镜像头魔数 "SVCA" 完成逐字节同步；

### `static int32 svcrt_installer_pump(int32 dev) {`

```c
static int32 svcrt_installer_pump(int32 dev) {
```

**推进接收状态机**
- `dev`：设备句柄
**返回**：0=已收满一个完整镜像头（内容在 svcrt_installer_hdr），-1=本轮还没收完
先用 4 字节窗口匹配魔数，不匹配则逐字节左移继续找，
因此串口上混杂的杂散字节会被自动跳过。
达到空转上限时**保留已收字节**直接返回，由调用方让出 CPU 后继续。

## kernelsrc/src/svcrt_task.c

**SVCrtOS 任务调度与系统调用分发实现**
实现任务调度器、上下文切换的 C 侧逻辑、SVC 系统调用分发，以及任务控制接口。
中断入口（SysTick_Handler/HardFault_Handler）在 board/ 中实现，
它们最终调用本文件的 svcrt_kernel_tick_handler / svcrt_hardfault_handler。

### `static int32 svcrt_sched_lock_blocks(void)`

```c
static int32 svcrt_sched_lock_blocks(void)
```

**检查调度器锁是否被持有（阻塞类接口的编程错误防护）**
**返回**：1=已锁定（调用方应忽略本次阻塞请求），0=未锁定
在调度器锁定期间调用阻塞接口会破坏任务状态一致性，
此处忽略请求并记录一条故障记录，便于定位问题代码。

### `void svcrt_sched_lock_internal(void)`

```c
void svcrt_sched_lock_internal(void)
```

**获取调度器锁（用户任务经 SVC 调用）**
累加嵌套计数后禁止任务切换，但不关闭中断。
临界区内不得调用阻塞接口（会被忽略并记录故障）。

### `uint32 svcrt_sched_unlock_internal(void)`

```c
uint32 svcrt_sched_unlock_internal(void)
```

**释放调度器锁**
**返回**：剩余嵌套层数（0 表示已完全解锁）
计数归零时主动触发一次任务切换，补偿锁定期间被推迟的调度。

### `int32 svcrt_sched_lock_count_internal(void)`

```c
int32 svcrt_sched_lock_count_internal(void)
```

**查询调度器锁嵌套层数**
**返回**：当前嵌套层数（0 表示未锁定）

### `int32 svcrt_task_stack_info_internal(int32 task_id, uint32 *out3)`

```c
int32 svcrt_task_stack_info_internal(int32 task_id, uint32 *out3)
```

**查询任务栈使用情况（峰值法）**
- `task_id`：任务号（从 1 开始）
- `out3`：输出数组：out3[0]=总字节，out3[1]=峰值已用字节，out3[2]=剩余字节
**返回**：0=成功，-1=参数非法
同时使用两种手段并取较大值：
1) 扫描填充图案得到“从未触及”区域；
2) 上下文切换记录的历史最低栈指针。
