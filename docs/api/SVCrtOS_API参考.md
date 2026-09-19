# SVCrtOS API 参考

> 由 `tools/gen_api_doc.py` 自动生成，请勿手工修改。
> 安装 [Doxygen](https://www.doxygen.nl/) 后重新运行脚本，可得到带交叉引用与调用关系的 HTML 版本。

## 目录

- [kernelsrc/include/svcrt.h](#kernelsrcincludesvcrth)（49 项）
- [kernelsrc/include/svcrt_app_image.h](#kernelsrcincludesvcrt_app_imageh)（31 项）
- [kernelsrc/include/svcrt_cfg.h](#kernelsrcincludesvcrt_cfgh)（1 项）
- [kernelsrc/include/svcrt_hal.h](#kernelsrcincludesvcrt_halh)（25 项）
- [kernelsrc/include/svcrt_init.h](#kernelsrcincludesvcrt_inith)（1 项）
- [kernelsrc/include/svcrt_installer.h](#kernelsrcincludesvcrt_installerh)（2 项）
- [kernelsrc/include/svcrt_layout.h](#kernelsrcincludesvcrt_layouth)（11 项）
- [kernelsrc/include/svcrt_layout_def.h](#kernelsrcincludesvcrt_layout_defh)（2 项）
- [kernelsrc/include/svcrt_loader.h](#kernelsrcincludesvcrt_loaderh)（22 项）
- [kernelsrc/include/svcrt_log.h](#kernelsrcincludesvcrt_logh)（5 项）
- [kernelsrc/include/svcrt_ptable.h](#kernelsrcincludesvcrt_ptableh)（18 项）
- [kernelsrc/include/svcrt_share.h](#kernelsrcincludesvcrt_shareh)（6 项）
- [kernelsrc/include/svcrt_shell.h](#kernelsrcincludesvcrt_shellh)（6 项）
- [kernelsrc/include/svcrt_spin.h](#kernelsrcincludesvcrt_spinh)（10 项）
- [kernelsrc/include/svcrt_svc_call.h](#kernelsrcincludesvcrt_svc_callh)（10 项）
- [kernelsrc/include/svcrt_task.h](#kernelsrcincludesvcrt_taskh)（4 项）
- [kernelsrc/include/svcrt_ulog.h](#kernelsrcincludesvcrt_ulogh)（5 项）
- [kernelsrc/include/svcrt_ushell.h](#kernelsrcincludesvcrt_ushellh)（4 项）
- [kernelsrc/port/arm/cortex-m3/svcrt_port.c](#kernelsrcportarmcortex-m3svcrt_portc)（7 项）
- [kernelsrc/port/arm/cortex-m4/svcrt_port.c](#kernelsrcportarmcortex-m4svcrt_portc)（7 项）
- [kernelsrc/sdk/posix/errno.h](#kernelsrcsdkposixerrnoh)（1 项）
- [kernelsrc/sdk/posix/pthread.h](#kernelsrcsdkposixpthreadh)（1 项）
- [kernelsrc/sdk/posix/semaphore.h](#kernelsrcsdkposixsemaphoreh)（1 项）
- [kernelsrc/sdk/posix/svcrt_posix_types.h](#kernelsrcsdkposixsvcrt_posix_typesh)（1 项）
- [kernelsrc/sdk/posix/unistd.h](#kernelsrcsdkposixunistdh)（4 项）
- [kernelsrc/src/svcrt_cfg.c](#kernelsrcsrcsvcrt_cfgc)（1 项）
- [kernelsrc/src/svcrt_installer.c](#kernelsrcsrcsvcrt_installerc)（2 项）
- [kernelsrc/src/svcrt_loader.c](#kernelsrcsrcsvcrt_loaderc)（5 项）
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
**返回**：成功返回驱动槽位号（>=0），负值为错误码（见 svcrt_loader.h）
> 说明：镜像头的 type 必须为驱动（SVCRT_APP_TYPE_DRIVER）；
镜像头的 load_addr 必须等于某个驱动槽位基址，写入前只擦除该槽区间。

### `int32  svcrt_thread_create(void (*entry)(void), void *stack, uint32 st`

```c
int32  svcrt_thread_create(void (*entry)(void), void *stack, uint32 stack_size,
```

**在当前 App/驱动的 RAM 里创建一个线程**
- `entry`：线程入口，必须位于调用者自己的固件窗口内
- `stack`：栈底地址，整段栈必须位于调用者自己的 RAM 窗口内
- `stack_size`：栈字节数（最小 128）
- `priority`：内核优先级（1~254，数值越小越高；255 是调度器哨兵，会被拒绝）
- `period_ms`：0 = 事件驱动（默认）；非 0 = 周期任务
**返回**：任务号（>0），失败返回负值
> 说明：内核只接受"调用者自己的"入口与栈：不能借别人的 RAM 建栈，
也不能把入口指到内核或别的 App 里去。

### `int32  svcrt_thread_self(void);`

```c
int32  svcrt_thread_self(void);
```

**取当前线程的内核任务号**
**返回**：任务号（>0）；在内核自带任务上返回 0

### `void   svcrt_thread_exit(void);`

```c
void   svcrt_thread_exit(void);
```

**结束当前线程，不再返回**

## kernelsrc/include/svcrt_app_image.h

**SVCrtOS App 镜像文件格式（打包工具与 Loader 共用）**
App 镜像 = 固定 256 字节头 + 代码/数据段原始内容（负载）+ 重定位表。
镜像由 App 工程编译产物转换而来（见 tools/pack_app.py），
Loader 按本结构解析、校验、重定位并写入镜像池。

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

### `#define SVCRT_APP_FLAG_AUTOSTART  (1u << 0)   /* 开机扫描认定后被自动启动 */`

```c
#define SVCRT_APP_FLAG_AUTOSTART  (1u << 0)   /* 开机扫描认定后被自动启动 */
```

**镜像头 flags 位定义（见 svcrt_app_header_t.flags）**
> 说明：该字段落在头部 offset 96（原 reserved 区首 4 字节）。旧镜像该位置
恒为 0，等价于「不设任何标志」，因此新增本字段不破坏旧镜像。 */

### `#define SVCRT_APP_FLAG_NONE       (0u)`

```c
#define SVCRT_APP_FLAG_NONE       (0u)
```

**未设置任何标志 */**

### `#define SVCRT_APP_STATE_VALID       (0u)      /* 已提交，可用（旧镜像恒为该值） */ #d`

```c
#define SVCRT_APP_STATE_VALID       (0u)      /* 已提交，可用（旧镜像恒为该值） */ #define SVCRT_APP_STATE_UNCOMMITTED (1u)      /* 未提交，上电必须丢弃 */
```

**镜像提交状态（svcrt_app_header_t.state）**
搬移一个镜像时先在新落点写一份 UNCOMMITTED 副本，校验通过后
只把这一个字段改写为 VALID（单字编程，原子），最后才擦除旧副本。
上电扫描见到 UNCOMMITTED 一律丢弃；见到同 image_id 的两份 VALID
则保留地址较低的那份。两条规则合起来使断电恢复幂等且确定，
因此不需要额外的日志扇区。
注意：本字段只能表达「安装/搬移的提交点」。不能拿它表达「卸载」：
镜像一旦提交，state 已经是 0，而 Flash 编程只能把 1 清成 0，
写 UNCOMMITTED（=1）要求把 0 置 1，硬件会静默保持 0——卸载因此
变成空操作，镜像在下次上电又会被扫回来。卸载走的是清 type 字段
（见 SVCRT_APP_OFF_TYPE）。

### `#define SVCRT_APP_RELOC_KIND_MASK  (0x3u) #define SVCRT_APP_RELOC_KIND`

```c
#define SVCRT_APP_RELOC_KIND_MASK  (0x3u) #define SVCRT_APP_RELOC_KIND_ROM   (0u) #define SVCRT_APP_RELOC_KIND_RAM   (1u)
```

**重定位表条目编码**
每项是一个 32 位字：低 2 位是类型，高位是「相对镜像起始」的偏移。
表项按偏移升序排列。
为什么需要两种类型：镜像里既有指向**自身代码/只读数据**的绝对地址，
也有指向**自己的 RAM 窗口**的绝对地址。前者随 Flash 落点平移，
后者随 RAM 块基址平移，两者增量不同，必须分开标记。
kind = 0（ROM）：装载时 word += (落点 + 负载偏移 - nominal_base)
kind = 1（RAM）：装载时 word += (RAM 块基址 - nominal_ram_base)
之所以 ROM 增量还要加上「负载偏移」：负载被链接在 nominal_base 上，
运行期却落在 落点+payload_offset——重定位表本身把负载往后推了
reloc_count*4 字节，这部分位移必须计入，否则代码里的自引用会整体
偏出一个表长。
两个基址都在镜像头里声明，因此单个镜像被整体搬移时只需要
对 ROM 类表项再叠加一个增量（新落点 - 旧落点），RAM 类不动。
> 说明：偏移低 2 位用来放类型，因此偏移必须按「半字」为单位存放：
表项 = (offset / 2) << 2 | kind。Thumb 的 32 位指令只保证
半字对齐，MOVW/MOVT 指令对完全可能落在 2 mod 4 的偏移上；
若按字节直接存放偏移，低 2 位会被类型覆盖，这类表项会静默
少 2 字节（内存里的表是对的，写进镜像再读出来才错）。

### `#define SVCRT_APP_RELOC_KIND_ROM_MOVW  (2u)`

```c
#define SVCRT_APP_RELOC_KIND_ROM_MOVW  (2u)
```

**MOVW/MOVT 立即数对里的 ROM 类地址（表项占 8 字节：两条指令） */**

### `#define SVCRT_APP_RELOC_KIND_RAM_MOVW  (3u)`

```c
#define SVCRT_APP_RELOC_KIND_RAM_MOVW  (3u)
```

**MOVW/MOVT 立即数对里的 RAM 类地址（表项占 8 字节：两条指令） */**

### `#define SVCRT_APP_RELOC_KIND_32    (0u)`

```c
#define SVCRT_APP_RELOC_KIND_32    (0u)
```

**重定位表编码版本：偏移按字节存放（已废弃，低 2 位会被类型截断） */**

### `#define SVCRT_APP_RELOC_KIND_HALF  (1u)`

```c
#define SVCRT_APP_RELOC_KIND_HALF  (1u)
```

**重定位表编码版本：偏移按半字存放（当前版本） */**

### `#define SVCRT_APP_RELOC_KIND_CUR   (SVCRT_APP_RELOC_KIND_HALF)`

```c
#define SVCRT_APP_RELOC_KIND_CUR   (SVCRT_APP_RELOC_KIND_HALF)
```

**内核唯一接受的表编码版本 */**

### `#define SVCRT_APP_RELOC_OFF_UNIT   (2u)`

```c
#define SVCRT_APP_RELOC_OFF_UNIT   (2u)
```

**表项里偏移的存放单位（字节）：2 = 按半字 */**

### `#define SVCRT_APP_RELOC_OFF(e) \ (((e) & ~SVCRT_APP_RELOC_KIND_MASK) >`

```c
#define SVCRT_APP_RELOC_OFF(e) \ (((e) & ~SVCRT_APP_RELOC_KIND_MASK) >> 1)
```

**从条目取出偏移（相对镜像起始，按半字存放，恒为偶数） */**

### `#define SVCRT_APP_RELOC_ENTRY(off, kind) \ ((((uint32)(off) / SVCRT_AP`

```c
#define SVCRT_APP_RELOC_ENTRY(off, kind) \ ((((uint32)(off) / SVCRT_APP_RELOC_OFF_UNIT) << 2) | \ ((uint32)(kind) & SVCRT_APP_RELOC_KIND_MASK))
```

**由偏移与类型组装条目（与 SVCRT_APP_RELOC_OFF 互逆） */**

### `#define SVCRT_APP_RELOC_KIND(e)    ((e) & SVCRT_APP_RELOC_KIND_MASK)`

```c
#define SVCRT_APP_RELOC_KIND(e)    ((e) & SVCRT_APP_RELOC_KIND_MASK)
```

**从条目取出类型 */**

### `#define SVCRT_APP_RELOC_SIZE       (4u)`

```c
#define SVCRT_APP_RELOC_SIZE       (4u)
```

**一条重定位表目占用的字节数 */**

### `#define SVCRT_APP_RELOC_MOV_SIZE   (8u)`

```c
#define SVCRT_APP_RELOC_MOV_SIZE   (8u)
```

**MOVW/MOVT 表项覆盖的字节数（两条 4 字节指令） */**

### `#define SVCRT_APP_RELOC_NONE       (0u)`

```c
#define SVCRT_APP_RELOC_NONE       (0u)
```

**重定位表无需 CRC 归零的字段，故整表参与校验；本宏仅为可读性 */**

### `#define SVCRT_APP_RELOC_MAX        (1024u)`

```c
#define SVCRT_APP_RELOC_MAX        (1024u)
```

**重定位表条目数上限（内核用于边界校验；表本身不驻留 RAM）**
> 说明：表项按「需要修补的字」计数，不是按镜像字节数。4 KB 表 = 1024 个
条目，对 1 MB 池里最大的镜像（768 KB 负载）也留出足够余量。 */

### `#define SVCRT_APP_RELOC_TABLE_MAX  (SVCRT_APP_RELOC_MAX * SVCRT_APP_RE`

```c
#define SVCRT_APP_RELOC_TABLE_MAX  (SVCRT_APP_RELOC_MAX * SVCRT_APP_RELOC_SIZE)
```

**重定位表字节数上限（= 条目上限 x 每项字节数） */**

### `#define SVCRT_APP_RELOC_OFFSET     (SVCRT_APP_HEADER_SIZE)`

```c
#define SVCRT_APP_RELOC_OFFSET     (SVCRT_APP_HEADER_SIZE)
```

**重定位表在镜像内的固定偏移（紧跟镜像头） */**

### `#define SVCRT_APP_PAYLOAD_OFFSET(p_hdr) \ (SVCRT_APP_HEADER_SIZE + ((p`

```c
#define SVCRT_APP_PAYLOAD_OFFSET(p_hdr) \ (SVCRT_APP_HEADER_SIZE + ((p_hdr)->reloc_count * SVCRT_APP_RELOC_SIZE))
```

**负载相对镜像起始的偏移（= 头 + 重定位表）**
- `p_hdr`：指向 svcrt_app_header_t 的指针

### `#define SVCRT_APP_TOTAL_LEN(p_hdr) \ (SVCRT_APP_PAYLOAD_OFFSET(p_hdr) `

```c
#define SVCRT_APP_TOTAL_LEN(p_hdr) \ (SVCRT_APP_PAYLOAD_OFFSET(p_hdr) + (p_hdr)->image_size)
```

**镜像总长度（头 + 重定位表 + 负载）**
- `p_hdr`：指向 svcrt_app_header_t 的指针

### `typedef struct {`

```c
typedef struct {
```

**App 镜像头（固定 256 字节）**
> 说明：CRC 覆盖范围：本头结构体（crc32 与 state 两个字段按 0 参与计算）
+ 负载 + 重定位表。之所以把 state 排除在外，是因为它必须能在写入
之后被单独改写（提交动作），而 CRC 不能因此失效。
crc32 字段自身的偏移见 SVCRT_APP_OFF_CRC32，state 见 SVCRT_APP_OFF_STATE。

### `#define SVCRT_APP_OFF_CRC32       (28u)`

```c
#define SVCRT_APP_OFF_CRC32       (28u)
```

**crc32 字段在头内的偏移（Loader 计算 CRC 时要把它按 0 处理） */**

### `#define SVCRT_APP_OFF_STATE       (100u)`

```c
#define SVCRT_APP_OFF_STATE       (100u)
```

**state 字段在头内的偏移（同上） */**

### `#define SVCRT_APP_OFF_TYPE        (4u)`

```c
#define SVCRT_APP_OFF_TYPE        (4u)
```

**type 字段在头内的偏移。卸载把镜像作废时清这一个字：**
0 -> 0 之外没有任何位要动，是合法的 1 -> 0 编程；
而「类型不是已知镜像」会被 check_header 拒收、上电扫描丢弃。
magic 故意留着，好让回收沿用「本区间有带头内容」的判断。 */

### `typedef char svcrt_app_header_size_check[ (sizeof(svcrt_app_header_t) `

```c
typedef char svcrt_app_header_size_check[ (sizeof(svcrt_app_header_t) == SVCRT_APP_HEADER_SIZE) ? 1 : -1];
```

**镜像头尺寸静态校验**
头结构与 SVCRT_APP_HEADER_SIZE 必须严格一致：打包工具与 Loader 分别按
“结构体布局”和“固定长度宏”解释镜像，一旦不一致就会静默错位。
此断言让不一致在编译期直接暴露。

### `typedef char svcrt_app_off_crc32_check[ (((uint32)&(((svcrt_app_header`

```c
typedef char svcrt_app_off_crc32_check[ (((uint32)&(((svcrt_app_header_t *)0)->crc32)) == SVCRT_APP_OFF_CRC32) ? 1 : -1];
```

**关键字段偏移静态校验**
Loader 计算 CRC 时按「偏移切片」而不是整体结构体来累加 CRC
（crc32 与 state 两个字段要按 0 参与），因此这些偏移一旦漂移
就会静默算出错误的校验值。这里逐个钉死。

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

### `void   svcrt_port_switch_enable(void);`

```c
void   svcrt_port_switch_enable(void);
```

**Enable task switching and request the first switch**
Must be called after svcrt_port_enter_idle() (CPU on PSP) and
svcrt_port_start_timer(). Switching requests issued before this
call are ignored on purpose.

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
开启内核 Shell（SHELL_ENABLE == 1）时不要调用本函数：常驻安装任务
会与控制台抢同一个串口 FIFO，此时改用 svcrt_installer_run_once()
由 `install` 命令触发一次性窗口，串行占用串口。

### `int32 svcrt_installer_run_once(int32 dev, uint32 timeout_ms);`

```c
int32 svcrt_installer_run_once(int32 dev, uint32 timeout_ms);
```

**在指定设备上接收并安装一个镜像（阻塞式，带超时）**
- `dev`：已打开的镜像接收设备句柄（控制台复用同一个句柄）
- `timeout_ms`：等待镜像头的超时（ms）；超时后清空接收状态并返回
**返回**：成功返回槽位号（>=0），失败返回 SVCRT_LOADER_ERR_x
与常驻任务共用同一套同步/校验/落盘逻辑，区别只在调用方式：
本函数在一个任务上下文内同步跑完「等头 → 落盘 → 按自启标志启动」，
期间调用方不应再去读同一个设备。
是否自启由镜像头 flags（SVCRT_APP_FLAG_AUTOSTART）决定。

## kernelsrc/include/svcrt_layout.h

**Kernel-side layout resolution: reads the device-side configuration**
record (if any), validates it and publishes the effective layout.
Called once during boot, right after svcrt_ptable_init() and before
the pool is scanned: the loader, the installer and the shell all ask

### `void   svcrt_layout_init(void);`

```c
void   svcrt_layout_init(void);
```

**Resolve the effective layout and publish it into the partition table.**
Order: start from the compile-time default layout, then try to load
and validate the device-side record; a valid record overrides the
default, anything else (absent / bad magic / bad CRC / hw mismatch /
illegal slot table) keeps the default and stores a reason code that
the shell prints. Never returns an error: a device without a usable
layout still boots.
> 说明：Must be called after svcrt_ptable_init() (it writes partition-table
fields) and before the pool scan (the scan uses the mode).

### `void   svcrt_layout_apply_runtime(void);`

```c
void   svcrt_layout_apply_runtime(void);
```

**Apply the runtime knobs of the effective configuration.**
Called at the end of svcrt_layout_init(). Only the knobs this
kernel build really honours are applied -- today the log level
and the crash-restart budget, both of which the shared
partition table then carries for the Loader. Every other knob
in the record is rejected by svcrt_layout_validate() with
SVCRT_CFG_ERR_NOT_IMPL rather than accepted and ignored: a
configuration that says one thing and makes the device do
another is the hardest kind of problem to find later.

### `const svcrt_cfg_slot_t *svcrt_layout_slots(void);`

```c
const svcrt_cfg_slot_t *svcrt_layout_slots(void);
```

**Effective fixed-slot table (SVCRT_CFG_SLOT_MAX entries, type 0 = unused). */**

### `const svcrt_cfg_slot_t *svcrt_layout_slot(uint32 index);`

```c
const svcrt_cfg_slot_t *svcrt_layout_slot(uint32 index);
```

**One entry of the effective table, or 0 when index is out of range. */**

### `int32  svcrt_layout_slot_by_base(uint32 addr);`

```c
int32  svcrt_layout_slot_by_base(uint32 addr);
```

**Index of the fixed slot whose Flash range contains addr.**
**返回**：Slot index, or -1 when no fixed slot covers addr.

### `const char *svcrt_layout_reason_text(uint32 reason);`

```c
const char *svcrt_layout_reason_text(uint32 reason);
```

**Human-readable name of a validation reason code (ASCII, for the shell). */**

### `uint32 svcrt_layout_validate(const svcrt_cfg_record_t *rec);`

```c
uint32 svcrt_layout_validate(const svcrt_cfg_record_t *rec);
```

**Validate a record: identity, CRC, hardware match and the whole slot table.**
**返回**：SVCRT_CFG_OK or one of the SVCRT_CFG_ERR_x codes.
> 说明：The slot table is checked against the kernel's own pool and RAM bounds,
so a record prepared for another board or another memory map is rejected
instead of being applied.

### `int32  svcrt_layout_write(const uint8 *record, uint32 len, uint32 *p_e`

```c
int32  svcrt_layout_write(const uint8 *record, uint32 len, uint32 *p_erased);
```

**Append a validated record to the CONFIG region and adopt it.**
- `record`：exactly SVCRT_CFG_RECORD_SIZE bytes as produced by tools/svcrt_layout.py
- `len`：must equal SVCRT_CFG_RECORD_SIZE
- `p_erased`：optional: set to 1 when the region had to be erased first
**返回**：>= 0 = record index written, < 0 = error (see the SVCRT_LAYOUT_ERR_x codes)
The kernel stamps seq and hw_compat_id itself and recomputes the CRC,
so a host that sends a slightly stale hw id still gets a record that
matches this kernel. If the region is full it is erased once and
writing restarts at offset 0; a reset during that single erase falls
back to the default layout instead of losing the device.

### `int32  svcrt_layout_erase(void);`

```c
int32  svcrt_layout_erase(void);
```

**Erase the CONFIG region and fall back to the compile-time default layout.**
**返回**：0 on success, < 0 on error.

### `int32  svcrt_layout_read(svcrt_cfg_record_t *out, uint32 *p_seq);`

```c
int32  svcrt_layout_read(svcrt_cfg_record_t *out, uint32 *p_seq);
```

**Read the live record (highest valid seq) from the CONFIG region.**
- `out`：destination buffer
- `p_seq`：optional: seq of the record that was read
**返回**：0 on success, < 0 when the region holds no valid record.

### `uint32 svcrt_layout_crc(const svcrt_cfg_record_t *rec);`

```c
uint32 svcrt_layout_crc(const svcrt_cfg_record_t *rec);
```

**CRC32 of a record with its crc32 field taken as zero (zlib-compatible). */**

## kernelsrc/include/svcrt_layout_def.h

**On-device layout + runtime configuration record. This is the single**
persistent place where a device's install strategy (fixed slots vs.
automatic placement), its fixed slot table and a small set of runtime
knobs are stored, so a customer can configure a board without

### `typedef struct {`

```c
typedef struct {
```

**One fixed slot: Flash placement + RAM window of a headed or bare image. */**

### `typedef struct {`

```c
typedef struct {
```

**The persistent layout + runtime configuration record (512 bytes, no padding). */**

## kernelsrc/include/svcrt_loader.h

**SVCrtOS App 镜像加载器（分区内加载 / 校验 / 启动 / 搬移 / 卸载）**
负责把 App 镜像写入池内空闲空间，并按镜像头信息校验后拉起为任务。
镜像格式见 svcrt_app_image.h；分区布局运行期从共享内存分区表获取
（见 svcrt_ptable.h），本模块不硬编码任何地址。

### `int32 svcrt_loader_load_buffer(const uint8 *image, uint32 image_len);`

```c
int32 svcrt_loader_load_buffer(const uint8 *image, uint32 image_len);
```

**从内存缓冲区加载完整 App 镜像**
- `image`：指向镜像起始（含 256 字节头）的缓冲区
- `image_len`：缓冲区长度（字节），必须 >= 镜像总长
**返回**：成功返回槽位号（>=0），失败返回 SVCRT_LOADER_ERR_x
与设备流式路径共用同一套判定：校验头 -> 找空闲区 -> 登记槽位 ->
应用重定位并写入 -> CRC 复核 -> 事务提交 -> 置 LOADED。

### `int32 svcrt_loader_load_dev(int32 dev, uint32 image_len);`

```c
int32 svcrt_loader_load_dev(int32 dev, uint32 image_len);
```

**从设备流式加载完整镜像（自行同步并读取镜像头）**
- `dev`：已打开的设备句柄（数据从镜像头之前开始）
- `image_len`：期望的镜像总长（含头）；传 0 表示由镜像头中的 image_size 决定
**返回**：成功返回槽位号（>=0），失败返回 SVCRT_LOADER_ERR_x
> 说明：边读边写 Flash，仅使用固定大小分块缓冲，不要求整镜像驻留 RAM。

### `uint32 svcrt_loader_scan(void);`

```c
uint32 svcrt_loader_scan(void);
```

**扫描整个镜像池，把 Flash 中已存在且校验通过的镜像标记为 LOADED**
**返回**：有效（可启动）的 App 数量
槽位状态原本只存在于共享 RAM，重启后会丢失；本函数在启动时按
镜像头魔数 + 硬件兼容签名 + CRC32 重新认定槽位，使“先烧录镜像、
再上电运行”的最小闭环成立。
扫描按分配粒度（SVCRT_POOL_ALLOC_UNIT）线性遍历整个池：
遇到带头镜像则按头的 type 归类为 App / 驱动；遇到裸镜像则查编译期
开发槽位表（SVCRT_DEV_SLOTn_*）判定类型与跨度。
同时执行断电恢复：
- state = UNCOMMITTED 的副本一律作废（搬移中途掉电）；
- 同一 image_id 出现两份有效副本时保留地址较低的那份。
两者都登记为 INVALID，随后由 svcrt_loader_reclaim() 擦除回收。

### `uint32 svcrt_loader_scan_driver(void);`

```c
uint32 svcrt_loader_scan_driver(void);
```

**扫描镜像池，返回其中的驱动数量**
**返回**：有效（可启动）的驱动数量，0=无有效驱动
与 svcrt_loader_scan() 共用同一份扫描结果（同一次扫描的驱动视图），
因此两者不区分调用顺序，可各自独立调用。

### `void  svcrt_loader_slot_hint_set(int32 index);`

```c
void  svcrt_loader_slot_hint_set(int32 index);
```

**指定下一次安装落在配置槽表的哪一条（-1 = 让内核按类型自己挑）**
- `index`：配置槽表下标（0 .. svcrt_layout_slot_count()-1），-1 表示不指定
只在固定槽位模式下起作用：这条提示只对**下一次**安装有效，
安装一开始就被消费掉（无论成功与否），不会泄漏到后续安装。
自动选址模式忽略它（同时也会消费掉，避免留下一个陈旧的值）。
> 说明：安装窗口由 shell 调用，提示也由 shell 设置；本接口不加锁，
调用者必须保证同一时刻只有一次安装在进行。

### `int32 svcrt_loader_slot_hint_get(void);`

```c
int32 svcrt_loader_slot_hint_get(void);
```

**读取当前的安装槽位提示（-1 = 未指定）。 @see svcrt_loader_slot_hint_set */**

### `int32 svcrt_loader_reclaim(void);`

```c
int32 svcrt_loader_reclaim(void);
```

**回收死区：把「不含活镜像的扇区」擦成 0xFF，使空间重新可用**
**返回**：本次擦除的扇区数；负值为 SVCRT_LOADER_ERR_x
Flash 擦除粒度是整扇区，因此一个扇区里只要有活镜像就不能擦。
本函数对每个含死字节的扇区执行：
1) 扇区内已无活镜像 -> 直接擦；
2) 扇区内仍有活镜像 -> 先把这些镜像搬到池内最低的已擦除空位
（应用重定位，走 UNCOMMITTED -> VALID 事务），再擦。
搬不动的镜像（正在运行、或没有足够空位）会让该扇区保留死区，
函数跳过它并继续处理其它扇区。卸载/安装后调用一次即可。

### `uint32 svcrt_loader_pool_free(uint32 *p_largest);`

```c
uint32 svcrt_loader_pool_free(uint32 *p_largest);
```

**查询池内还剩多少可装镜像的物理空间**
- `p_largest`：非空时写入最长的一段连续空闲字节数
**返回**：所有 0xFF 连续段的总字节数

### `int32 svcrt_loader_uninstall(uint32 slot);`

```c
int32 svcrt_loader_uninstall(uint32 slot);
```

**卸载一个镜像：停止任务、注销记录，然后回收它占用的空间**
- `slot`：槽位记录号
**返回**：0=成功，负值为 SVCRT_LOADER_ERR_x
正在运行的任务会被先停止（镜像字节仍在 Flash，可再次启动）；
随后注销槽位记录并调用 svcrt_loader_reclaim() 尽量把空间收回来。
> 说明：卸载是破坏性操作：擦除后镜像不可恢复，需要重新安装。

### `int32 svcrt_loader_start_driver_slot(uint32 slot);`

```c
int32 svcrt_loader_start_driver_slot(uint32 slot);
```

**启动指定驱动槽位的驱动**
- `slot`：驱动槽位号
**返回**：成功返回任务号（>0），失败返回 SVCRT_LOADER_ERR_x
栈从该镜像分配到的 RAM 块顶部切出；RAM 块由伙伴分配器按镜像头
声明的 ram_size 现算，因此不同镜像永不共用 RAM。

### `int32 svcrt_loader_start_driver(void);`

```c
int32 svcrt_loader_start_driver(void);
```

**启动 0 号驱动槽（兼容包装）**
**返回**：成功返回任务号（>0），失败返回 SVCRT_LOADER_ERR_x

### `int32 svcrt_loader_stop_driver_slot(uint32 slot);`

```c
int32 svcrt_loader_stop_driver_slot(uint32 slot);
```

**停止指定驱动槽位的任务（镜像仍保留在 Flash）**
- `slot`：驱动槽位号
**返回**：0=成功，负值为 SVCRT_LOADER_ERR_x

### `uint32 svcrt_loader_state_driver(uint32 slot);`

```c
uint32 svcrt_loader_state_driver(uint32 slot);
```

**查询驱动槽位状态**
- `slot`：驱动槽位号
**返回**：SVCRT_APP_SLOT_x；槽位非法返回 0xffffffff

### `int32 svcrt_loader_load_dev_hdr(int32 dev, const svcrt_app_header_t *p`

```c
int32 svcrt_loader_load_dev_hdr(int32 dev, const svcrt_app_header_t *p_hdr, uint32 image_len);
```

**从设备流式加载（镜像头已由调用方读出）**
- `dev`：已打开的设备句柄，位置正好在镜像头之后
- `p_hdr`：已读出的镜像头（调用方已完成魔数同步）
- `image_len`：期望镜像总长（含头），传 0 表示由镜像头决定
**返回**：成功返回槽位号（>=0），失败返回 SVCRT_LOADER_ERR_x
本函数是**唯一的安装实现**，其余安装入口均为其薄包装。流程：
1) 校验头部（魔数 / 类型 / 兼容签名 / 长度 / 入口 / 重定位表）；
2) 在池内找一段足够大的已擦除空间（首适配，天然紧邻排列）；
3) 登记槽位（INSTALLING）；
4) 写入镜像头（state=UNCOMMITTED）；
5) 流式接收重定位表并落盘；
6) 流式接收负载，逐块按 delta 打补丁后落盘（每块回一个 ACK）；
7) CRC32 复核（头按 crc32/state 归零计算）；
8) 唯一的一个字写入：state = VALID，提交完成；
9) 置 LOADED 并清零故障计数，返回槽位号；自启标志不在本函数内
写入，由调用方（安装模块）按镜像头 flags 填槽位表。
供安装任务使用：先逐字节同步到镜像头魔数，再把头交本函数延续。

### `int32 svcrt_loader_load_driver(int32 dev, uint32 image_len);`

```c
int32 svcrt_loader_load_driver(int32 dev, uint32 image_len);
```

**从设备安装驱动镜像（自行读头）**
- `dev`：已打开的设备句柄，位置在镜像头之前
- `image_len`：期望镜像总长（含头），传 0 表示由镜像头决定
**返回**：成功返回驱动槽位号（>=0），失败返回 SVCRT_LOADER_ERR_x
> 说明：镜像头的 type 必须为 SVCRT_APP_TYPE_DRIVER。

### `int32 svcrt_loader_load_driver_dev(int32 dev, const svcrt_app_header_t`

```c
int32 svcrt_loader_load_driver_dev(int32 dev, const svcrt_app_header_t *p_hdr, uint32 image_len);
```

**从设备安装驱动镜像（镜像头已由调用方读出）**
**返回**：成功返回驱动槽位号（>=0），失败返回 SVCRT_LOADER_ERR_x
校验 hdr.type 为 DRIVER 后转交 svcrt_loader_load_dev_hdr()。

### `int32 svcrt_loader_on_fault(int32 task_id);`

```c
int32 svcrt_loader_on_fault(int32 task_id);
```

**按“崩溃重启上限”策略处理 App 任务故障**
- `task_id`：发生故障的任务号
**返回**：1=已达上限并已禁用该 App，0=未达上限（调用方应恢复/重启该 App），
-1=不属于任何槽位（内核任务，调用方沿用默认处理）
计数按槽位累计（从分区表反查 task_id 归属）：故障一次加一，
达到 APP_CRASH_RESTART_MAX 后将该槽位置为 INVALID 并让任务脱离调度。
重新安装镜像时计数清零。
> 说明：计数保存在共享 RAM，掉电即清零，因此当前可挡住“App 反复崩溃重启”，
但挡不住“崩溃导致整机复位”的启动环——那需要把计数持久化（如备份寄存器）。

### `int32 svcrt_loader_start(uint32 slot);`

```c
int32 svcrt_loader_start(uint32 slot);
```

**把已加载的槽位拉起为任务（按槽位类型自动分流 App / 驱动）**
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

### `uint32 svcrt_loader_start_autostart_driver(void);`

```c
uint32 svcrt_loader_start_autostart_driver(void);
```

**按各驱动槽的自启标志批量启动驱动**
**返回**：实际启动的驱动数量
自启标志在扫描时由镜像头 flags 回填（裸镜像取开发槽位表配置）。
未设自启的驱动不会被拉起，需由上层（如 shell 的 drv start）显式启动。
必须在 svcrt_loader_scan_driver() 之后调用。

### `uint32 svcrt_loader_start_autostart(void);`

```c
uint32 svcrt_loader_start_autostart(void);
```

**按各 App 槽的自启标志批量启动 App**
**返回**：实际启动的 App 数量
与 svcrt_loader_start_autostart_driver() 同口径。
必须在 svcrt_loader_scan() 之后调用。

## kernelsrc/include/svcrt_log.h

**SVCrtOS 内核日志：分级输出 + 运行期过滤（内核侧接口）**
设计沿用 mcu_framework 的 log 组件思路，并补上 SVCrtOS 需要的一环：
- 级别：NONE(0) < ERROR(1) < WARNING(2) < INFO(3) < DEBUG(4)，
数值越大越啰嗦（定义见 svcrt_log_defs.h，与用户态共用）。

### `void   svcrt_log_init(void);`

```c
void   svcrt_log_init(void);
```

**日志模块初始化（打开控制台串口，装载配置里的初始级别）**
需在设备框架初始化之后、调度启动之前调用；重复调用无害。

### `void   svcrt_log_set_level(uint32 level);`

```c
void   svcrt_log_set_level(uint32 level);
```

**设置运行期日志级别**
- `level`：SVCRT_LOG_NONE..SVCRT_LOG_DEBUG
> 说明：超出范围的值不改变当前级别（避免误配导致全体静音）。

### `uint32 svcrt_log_get_level(void);`

```c
uint32 svcrt_log_get_level(void);
```

**取当前运行期日志级别**

### `void   svcrt_log_emit(uint32 level, const char *tag, uint32 line, cons`

```c
void   svcrt_log_emit(uint32 level, const char *tag, uint32 line, const char *msg);
```

**输出一条已完成格式化的日志（内核内部与 SVC 0x19 共用此出口）**
- `level`：级别；高于当前运行期级别时直接丢弃
- `tag`：标签（短字符串，通常是大写模块名）
- `line`：源码行号；用户态传 0 时不打印行号
- `msg`：正文（已格式化，为空时直接返回）

### `int32  svcrt_log_svc(uint32 level, const char *tag, const char *msg);`

```c
int32  svcrt_log_svc(uint32 level, const char *tag, const char *msg);
```

**SVC 0x19 的服务入口（用户态 App / 驱动调用）**
- `level`：级别
- `tag`：标签字符串地址（已由分发层校验）
- `msg`：正文地址（已由分发层校验）
**返回**：0=已处理（含被级别过滤），-1=参数非法

## kernelsrc/include/svcrt_ptable.h

**SVCrtOS 分区表运行期接口（内核侧）**
把 config/svcrt_partition.h 中的编译期布局，在启动时镜像到共享内存的
svcrt_partition_table_t 中，供内核、Loader、App 在运行期统一读取；
并集中维护统一镜像池的**空闲区分配**与镜像 RAM 池的**伙伴分配**。

### `void svcrt_ptable_init(void);`

```c
void svcrt_ptable_init(void);
```

**初始化共享内存中的分区表**
用编译期布局填充布局字段，并把所有槽位记录清为空。
必须在任务调度启动之前调用（建议在 svcrt_kernel_init 阶段）。

### `svcrt_partition_table_t *svcrt_ptable_get(void);`

```c
svcrt_partition_table_t *svcrt_ptable_get(void);
```

**获取分区表指针**
**返回**：指向共享内存中分区表的指针；未初始化时同样返回有效地址

### `int32 svcrt_ptable_range_check(uint32 base, uint32 size);`

```c
int32 svcrt_ptable_range_check(uint32 base, uint32 size);
```

**校验一段镜像区间是否落在可分配范围内**
- `base`：镜像起始地址
- `size`：镜像占用的字节数
**返回**：0=合法；-1=越界或未按分配粒度对齐
只检查「池内 + 不越过压实余量 + 落点按 SVCRT_POOL_ALLOC_UNIT 对齐」。
是否与其它镜像重叠由 svcrt_ptable_alloc 判定。

### `int32 svcrt_ptable_find_free(uint32 size, uint32 *p_base, uint32 *p_al`

```c
int32 svcrt_ptable_find_free(uint32 size, uint32 *p_base, uint32 *p_aligned);
```

**在池里找一段能放下 size 字节的空闲区间（首适配）**
- `size`：需要的字节数
- `p_base`：输出落点
- `p_aligned`：输出落点是否是「2 的幂对齐落点」（1=是，可以构造精确 MPU 窗口；
0=落点只是为了塞进缺口，ROM 窗口要退化为包含窗口）
**返回**：0=找到；-1=没有足够大的空闲区间
落点策略（对应「优先精确隔离、退让给容量」）：
1. 优先把落点向上取整到 2^n（2^n >= 区间长度）后放置，使镜像
自身的跨度成为 2 的幂且基址按该跨度对齐 —— MPU 单区域可精确覆盖；
2. 只有当对齐落点放不下而缺口起点放得下时，才用缺口起点做落点，
并把 p_aligned 置 0。

### `int32 svcrt_ptable_alloc(uint32 type, uint32 base, uint32 size);`

```c
int32 svcrt_ptable_alloc(uint32 type, uint32 base, uint32 size);
```

**在池内登记一段镜像区间（不碰 Flash）**
- `type`：SVCRT_SLOT_APP / SVCRT_SLOT_DRIVER
- `base`：镜像起始地址
- `size`：镜像占用的字节数
**返回**：成功返回槽位记录号（>=0），失败返回 -1（区间非法/重叠/无空闲记录）
登记成功的记录状态为 INSTALLING：写入完成前不可启动，掉电则由扫描
依 CRC 判定为 INVALID。同一区间重复登记会复用原记录（覆盖安装）。
安装失败时调用方必须 svcrt_ptable_free()。

### `int32 svcrt_ptable_alloc_raw(uint32 type, uint32 base, uint32 size, ui`

```c
int32 svcrt_ptable_alloc_raw(uint32 type, uint32 base, uint32 size, uint32 entry);
```

**登记一段「不参与分配」的区间（开发期裸镜像）**
- `type`：SVCRT_SLOT_APP / SVCRT_SLOT_DRIVER
- `base`：区间起始地址
- `size`：区间字节数
- `entry`：入口地址（裸镜像入口就是区间基址 | Thumb）
**返回**：槽位记录号（>=0）或 -1
状态置为 SVCRT_APP_SLOT_RAW：分配器把它当已占用，压实逻辑跳过它。

### `void svcrt_ptable_free(uint32 slot);`

```c
void svcrt_ptable_free(uint32 slot);
```

**释放一条槽位记录（同时释放它占用的 Flash 区间与 RAM 块）**
- `slot`：槽位记录号

### `int32 svcrt_ptable_move(uint32 slot, uint32 new_base);`

```c
int32 svcrt_ptable_move(uint32 slot, uint32 new_base);
```

**把一条记录的 Flash 落点改到别处（搬移镜像时用）**
- `slot`：槽位记录号
- `new_base`：新落点（必须已通过区间校验且不与其它记录重叠）
**返回**：0=成功，-1=失败
搬移时镜像内容被重新写到新落点，入口地址也要跟着平移，
由调用方负责（entry 不在这里算，因为只有 loader 知道负载偏移）。

### `int32 svcrt_ptable_find_addr(uint32 addr, uint32 *p_base, uint32 *p_si`

```c
int32 svcrt_ptable_find_addr(uint32 addr, uint32 *p_base, uint32 *p_size);
```

**查找区间内的任意地址所属的槽位记录**
- `addr`：待查地址
- `p_base`：输出该记录的起始地址（可为 0）
- `p_size`：输出该记录的字节数（可为 0）
**返回**：槽位记录号（>=0），未命中返回 -1

### `int32 svcrt_ptable_find_task(uint32 task_id);`

```c
int32 svcrt_ptable_find_task(uint32 task_id);
```

**查找任务号对应的槽位记录**
**返回**：槽位记录号（>=0），未命中返回 -1

### `int32 svcrt_ptable_slot_info(uint32 slot, uint32 *p_type, uint32 *p_ba`

```c
int32 svcrt_ptable_slot_info(uint32 slot, uint32 *p_type, uint32 *p_base, uint32 *p_size);
```

**读取槽位记录的类型 / 基址 / 字节数**
**返回**：0=成功，-1=槽位非法

### `int32 svcrt_ptable_read_header(uint32 slot, svcrt_app_header_t *out);`

```c
int32 svcrt_ptable_read_header(uint32 slot, svcrt_app_header_t *out);
```

**读取指定槽位 Flash 中的镜像头**
- `slot`：槽位记录号
- `out`：输出镜像头（可为 0，仅做存在性校验）
**返回**：0=成功且镜像有效，-1=槽位非法或该槽位是裸镜像，-2=镜像头魔数错误
> 说明：Flash 已被映射到地址空间，可直接按地址读取。

### `int32 svcrt_ptable_set_slot(uint32 slot, uint32 state, uint32 entry, u`

```c
int32 svcrt_ptable_set_slot(uint32 slot, uint32 state, uint32 entry, uint32 task_id);
```

**更新槽位运行期状态**
- `slot`：槽位记录号
- `state`：SVCRT_APP_SLOT_x
- `entry`：入口地址（直接赋值，清空槽位时传 0）
- `task_id`：任务号（直接赋值，未启动时传 0）
**返回**：0=成功，-1=槽位非法
> 说明：三个字段在自旋锁保护下作为一组更新：故障处理路径（可能运行在异常
上下文）与安装任务都可能同时改写同一个槽位，否则读者会看到
state/entry/task_id 互相不匹配的中间态。

### `int32 svcrt_ptable_get_slot(uint32 slot, uint32 *p_state, uint32 *p_en`

```c
int32 svcrt_ptable_get_slot(uint32 slot, uint32 *p_state, uint32 *p_entry, uint32 *p_task_id);
```

**原子读取一个槽位的状态三元组**
- `slot`：槽位记录号
- `p_state`：输出状态（可为 0）
- `p_entry`：输出入口地址（可为 0）
- `p_task_id`：输出任务号（可为 0）
**返回**：0=成功，-1=槽位非法

### `int32 svcrt_ptable_ram_alloc(uint32 bytes, uint32 *p_base, uint32 *p_s`

```c
int32 svcrt_ptable_ram_alloc(uint32 bytes, uint32 *p_base, uint32 *p_size);
```

**从镜像 RAM 池里按 2 的幂分配一块**
- `bytes`：镜像头声明的 ram_size
- `p_base`：输出块基址
- `p_size`：输出块大小（2 的幂字节数）
**返回**：0=成功，-1=没有合适的块
块大小 = max(2^n >= bytes, slot_ram_min_block)，并受 slot_ram_max_block 限制。
落点必须按块大小对齐 —— 否则单个 MPU region 覆盖不了整块 RAM。

### `int32 svcrt_ptable_ram_bind(uint32 slot, uint32 base, uint32 size);`

```c
int32 svcrt_ptable_ram_bind(uint32 slot, uint32 base, uint32 size);
```

**把 RAM 块记到槽位记录上**
**返回**：0=成功，-1=槽位非法

### `int32 svcrt_ptable_ram_info(uint32 slot, uint32 *p_base, uint32 *p_siz`

```c
int32 svcrt_ptable_ram_info(uint32 slot, uint32 *p_base, uint32 *p_size);
```

**读出一条记录已绑定的 RAM 块**
**返回**：0=成功，-1=槽位非法（未绑定时 *p_base / *p_size 输出 0）

### `int32 svcrt_ptable_stats(uint32 *p_used, uint32 *p_total, uint32 *p_fr`

```c
int32 svcrt_ptable_stats(uint32 *p_used, uint32 *p_total, uint32 *p_frag);
```

**取池的统计值（供 shell 的 info 命令展示）**
- `p_used`：已占用字节数（不含压实余量与裸镜像？含全部占用，便于对账）
- `p_total`：池可分配总字节数（= pool_usable_size）
- `p_frag`：空闲区间个数（>1 表示存在碎片）
**返回**：0=成功

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

### `#define SVCRT_PARTITION_VERSION   (7u)`

```c
#define SVCRT_PARTITION_VERSION   (7u)
```

**分区表结构版本。**
v1 -> v2: driver 区状态由「单槽四个平铺字段」改为「与 App 同构的槽位数组」。
v2 -> v3: 末尾追加每槽 autostart 标志。
v3 -> v4: 驱动区与 App 区合并为**统一镜像池**，槽位表按「镜像」而非
「固定分区槽」组织：每条记录带自己的类型（App/驱动）、基址与
占用单元数。
v4 -> v5: 池分配改为细粒度字节粒度（pool_alloc_unit），镜像可被搬移与
压实（新增 pool_usable_size / pool_reserve_sectors）；记录的
「占用单元数」换成「字节数」；镜像 RAM 由伙伴分配器按镜像
声明的 ram_size 现算，因此每条记录新增 ram_base / ram_size，
且槽位数组长度从 8 扩到 16。布局字段与记录字段再次全变，
版本必须升位。
v5 -> v6: 新增设备端布局配置区（安装模式 + 固定槽位表），分区表追加
layout_mode / layout_source / config_base / config_size /
cfg_slot_count 五个字段；Flash 布局变为
BOOT -> KERNEL -> CONFIG -> IMAGE_POOL，池基址随之后移
（旧镜像与旧工具必须同步升级），硬件兼容签名低 16 位升到 5。
v6 -> v7: 设备端配置区里原本只能写 0 的两个旋钮开始生效——
boot_delay_ms（自启前的调试器挂接窗口）与 flags 的
RAW_ALLOW 位（是否接受池内直接烧录的裸镜像）。分区表末尾
追加 cfg_boot_delay_ms / cfg_raw_allow 两个生效值，
让上位机能一眼看出设备到底按哪套配置在跑。

### `#define SVCRT_SLOT_ARRAY_MAX      (16u)`

```c
#define SVCRT_SLOT_ARRAY_MAX      (16u)
```

**槽位数组的固定长度（ABI 形状常量）。**
实际使用的槽位数由运行期字段 slot_max 决定，必须 <= 本值；
数组长度写死是为了让结构体尺寸与偏移跨版本稳定。 */

### `#define SVCRT_SLOT_FREE           (0u)   /* 记录未使用，对应 Flash 区间可被分配 */ #`

```c
#define SVCRT_SLOT_FREE           (0u)   /* 记录未使用，对应 Flash 区间可被分配 */ #define SVCRT_SLOT_APP            (1u)   /* 用户应用镜像 */ #define SVCRT_SLOT_DRIVER         (2u)   /* 用户驱动镜像 */
```

**槽位类型（与 svcrt_app_image.h 的 SVCRT_APP_TYPE_x 同值） */**

### `#define SVCRT_APP_SLOT_EMPTY      (0u)    /* 空槽位 */ #define SVCRT_APP_`

```c
#define SVCRT_APP_SLOT_EMPTY      (0u)    /* 空槽位 */ #define SVCRT_APP_SLOT_LOADED     (1u)    /* 已写入镜像并通过校验 */ #define SVCRT_APP_SLOT_RUNNING    (2u)    /* 已注册为任务并运行 */ #define SVCRT_APP_SLOT_INVALID    (3u)    /* 槽位有内容但校验失败（魔数/兼容签名/CRC 不符） */ #define SVCRT_APP_SLOT_INSTALLING (4u)    /* 正在安装：写入未完成，不可启动（掉电后由 CRC 判定为 INVALID） */ #define SVCRT_APP_SLOT_RAW        (5u)    /* 开发期裸镜像（无镜像头）：占用区间不参与分配，也不参与压实 */
```

**槽位状态 */**

### `typedef struct {`

```c
typedef struct {
```

**分区表（存放于共享内存起始处）**
由内核在启动时填充静态布局，运行期由 Loader 更新槽位状态。
Loader / App 只读布局字段，写状态字段前须确认自己拥有该槽位。
Flash 统一镜像池：pool_base 起、pool_size 字节，按
pool_alloc_unit 粒度做首适配分配（镜像向后紧邻排列），
尾部 pool_reserve_sectors 个扇区始终留空作为压实的搬移余量。
pool_sector 只是芯片的物理擦除单位，与分配粒度无关。
RAM：镜像的 .data/.bss/栈由伙伴分配器从 slot_ram_base 起、
slot_ram_total 字节的池里按镜像头声明的 ram_size 切块，
块大小落在 [slot_ram_min_block, slot_ram_max_block]。
每条记录带自己的 ram_base / ram_size，两块镜像永不共用 RAM。

## kernelsrc/include/svcrt_shell.h

**SVCrtOS 内核 Shell 控制台（ark-shell 移植）对外接口**
控制台任务独占 SHELL_DEV_NAME 串口，提供 App/驱动/任务的启停与
查询、故障读数、串口安装等人机命令。
串口归属约定：开启 shell 后不再注册常驻安装任务（见

### `int32 svcrt_shell_init(void);`

```c
int32 svcrt_shell_init(void);
```

**初始化并注册内核 Shell 控制台任务**
**返回**：成功返回任务号（>0）；SHELL_ENABLE 为 0 时返回 0
需在调度启动之前调用（紧跟 svcrt_loader_start_autostart*() 之后）。

### `int32 svcrt_shell_uart_handle(void);`

```c
int32 svcrt_shell_uart_handle(void);
```

**取控制台已打开的串口句柄**
**返回**：设备句柄；尚未打开时返回 -1
安装窗口复用同一个句柄收镜像，语义上比再 open 一次更清楚。

### `int32 svcrt_shell_uart_open(void);`

```c
int32 svcrt_shell_uart_open(void);
```

**打开控制台串口（已打开则直接返回既有句柄）**
**返回**：设备句柄；打开失败返回 -1

### `int32 svcrt_shell_ext_register(const svcrt_ushell_cmd_t *cmd);`

```c
int32 svcrt_shell_ext_register(const svcrt_ushell_cmd_t *cmd);
```

**往内核控制台注册一条用户态（App / 驱动）命令**
- `cmd`：用户态描述符；name / help 会被复制进内核 RAM，
所以镜像卸载后不会留下指向已擦除 Flash 的悬垂指针
**返回**：SVCRT_USHELL_OK 或负错误码（见 svcrt_ushell.h）
供内核分发层（SVC 0x1A 子命令 1）调用；用户态请用
svcrt_shell_cmd_register()，不要直接调用本函数。

### `int32 svcrt_shell_ext_unregister(const char *name);`

```c
int32 svcrt_shell_ext_unregister(const char *name);
```

**注销一条用户态命令（按名字，不区分大小写）**
**返回**：SVCRT_USHELL_OK 或负错误码

### `int32 svcrt_shell_print_n(const char *msg, uint32 len);`

```c
int32 svcrt_shell_print_n(const char *msg, uint32 len);
```

**向控制台输出一段有界长度的文本（不追加换行）**
- `msg`：文本首地址
- `len`：输出长度；由分发层先探测上限，避免在内核侧做无界 strlen
**返回**：SVCRT_USHELL_OK 或负错误码
SVC 处理运行在用户任务的 PSP 上（App 栈 4K、驱动栈 1K），
因此这里用内核静态缓冲分段输出，不在调用者栈上开临时数组。

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

### `#define SVCRT_SPINLOCK_INIT          { 0u, 0xffffffffu, 0u }`

```c
#define SVCRT_SPINLOCK_INIT          { 0u, 0xffffffffu, 0u }
```

**Static initialiser for a global lock object.**
> 说明：Must match svcrt_spin_init(): owner 0xffffffff means "no owner", so a
freshly defined lock cannot be mistaken for one held by CPU 0. */

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

## kernelsrc/include/svcrt_svc_call.h

**Toolchain-independent SVC entry helpers for the App / Driver SDK.**
The unprivileged side (App, Driver) never touches kernel internals:
every system service is reached with an SVC instruction carrying the
service number as its 8-bit immediate. How that instruction is

### `#define SVCRT_SVC_DECL_1(ret, num, name, t0)          ret __svc(num) n`

```c
#define SVCRT_SVC_DECL_1(ret, num, name, t0)          ret __svc(num) name(t0);
```

Declare an SVC service that takes one argument and returns a value. */

### `#define SVCRT_SVC_DECL_2(ret, num, name, t0, t1)      ret __svc(num) n`

```c
#define SVCRT_SVC_DECL_2(ret, num, name, t0, t1)      ret __svc(num) name(t0, t1);
```

Declare an SVC service that takes two arguments and returns a value. */

### `#define SVCRT_SVC_DECL_3(ret, num, name, t0, t1, t2)  ret __svc(num) n`

```c
#define SVCRT_SVC_DECL_3(ret, num, name, t0, t1, t2)  ret __svc(num) name(t0, t1, t2);
```

Declare an SVC service that takes three arguments and returns a value. */

### `#define SVCRT_SVC_DECL_V1(num, name, t0)              void __svc(num) `

```c
#define SVCRT_SVC_DECL_V1(num, name, t0)              void __svc(num) name(t0);
```

Declare an SVC service that takes one argument and returns nothing. */

### `#define SVCRT_SVC_DECL_V2(num, name, t0, t1)          void __svc(num) `

```c
#define SVCRT_SVC_DECL_V2(num, name, t0, t1)          void __svc(num) name(t0, t1);
```

Declare an SVC service that takes two arguments and returns nothing. */

### `#define SVCRT_SVC_DECL_1(ret, num, name, t0)                          `

```c
#define SVCRT_SVC_DECL_1(ret, num, name, t0)                          \ SVCRT_INLINE ret name(t0 a0)                                     \ {                                                                 \ register unsigned int r0 __asm("r0") = (unsigned int)a0;      \ __asm volatile ("svc #" SVCRT_STR(num) : "+r"(r0) : : "memory"); \ return (ret)r0;                                               \
```

Declare an SVC service that takes one argument and returns a value. */

### `#define SVCRT_SVC_DECL_2(ret, num, name, t0, t1)                      `

```c
#define SVCRT_SVC_DECL_2(ret, num, name, t0, t1)                      \ SVCRT_INLINE ret name(t0 a0, t1 a1)                              \ {                                                                 \ register unsigned int r0 __asm("r0") = (unsigned int)a0;      \ register unsigned int r1 __asm("r1") = (unsigned int)a1;      \ __asm volatile ("svc #" SVCRT_STR(num)                        \
```

Declare an SVC service that takes two arguments and returns a value. */

### `#define SVCRT_SVC_DECL_3(ret, num, name, t0, t1, t2)                  `

```c
#define SVCRT_SVC_DECL_3(ret, num, name, t0, t1, t2)                  \ SVCRT_INLINE ret name(t0 a0, t1 a1, t2 a2)                       \ {                                                                 \ register unsigned int r0 __asm("r0") = (unsigned int)a0;      \ register unsigned int r1 __asm("r1") = (unsigned int)a1;      \ register unsigned int r2 __asm("r2") = (unsigned int)a2;      \
```

Declare an SVC service that takes three arguments and returns a value. */

### `#define SVCRT_SVC_DECL_V1(num, name, t0)                              `

```c
#define SVCRT_SVC_DECL_V1(num, name, t0)                              \ SVCRT_INLINE void name(t0 a0)                                    \ {                                                                 \ register unsigned int r0 __asm("r0") = (unsigned int)a0;      \ __asm volatile ("svc #" SVCRT_STR(num) : "+r"(r0) : : "memory"); \ }
```

Declare an SVC service that takes one argument and returns nothing. */

### `#define SVCRT_SVC_DECL_V2(num, name, t0, t1)                          `

```c
#define SVCRT_SVC_DECL_V2(num, name, t0, t1)                          \ SVCRT_INLINE void name(t0 a0, t1 a1)                             \ {                                                                 \ register unsigned int r0 __asm("r0") = (unsigned int)a0;      \ register unsigned int r1 __asm("r1") = (unsigned int)a1;      \ __asm volatile ("svc #" SVCRT_STR(num)                        \
```

Declare an SVC service that takes two arguments and returns nothing. */

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

### `void   svcrt_sched_pri_cache_drop(void);`

```c
void   svcrt_sched_pri_cache_drop(void);
```

**作废调度器的优先级缓存（任务表发生变化时调用）**
缓存的内容是「所有非 INVALID 任务的最小 / 次小基准优先级」，
供 svcrt_sched_next() 的 O(1) 快速路径判断当前任务能否被抢占。
任务注册后必须作废：新任务可能带着更高的优先级出现，
用旧缓存会让快速路径误判成「无人能抢占」。作废只是关掉快速路径
（退回全表扫描），不会算错。每个节拍的扫描末尾会自动重建缓存。

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

## kernelsrc/include/svcrt_ulog.h

**SVCrtOS 用户态服务接口：日志（SVC 0x19）与控制台 Shell（SVC 0x1A）**
这是 App / 驱动固件侧唯一需要包含的两个「内核外设」头文件之一
（另一个是 svcrt.h / svcrt_driver_sdk.h）。它同时解决两件事：
1) 日志

### `SVCRT_UFMT_INLINE int svcrt_ufmt_v(char *out, uint32 size, const char `

```c
SVCRT_UFMT_INLINE int svcrt_ufmt_v(char *out, uint32 size, const char *fmt, va_list ap) {
```

**格式化核心：参数已取成 va_list（供带 ... 的包装函数复用）**
**返回**：实际写入的字符数（不含结尾 '\0'）

### `SVCRT_UFMT_INLINE int svcrt_ufmt(char *out, uint32 size, const char *f`

```c
SVCRT_UFMT_INLINE int svcrt_ufmt(char *out, uint32 size, const char *fmt, ...) {
```

**把格式化结果写入 out（最多 size-1 个字符 + '\0'）**
**返回**：实际写入的字符数（不含结尾 '\0'）

### `int32 svcrt_log_print(uint32 level, const char *tag, const char *msg);`

```c
int32 svcrt_log_print(uint32 level, const char *tag, const char *msg);
```

**输出一条日志（级别过滤在核内统一完成）**
- `level`：SVCRT_LOG_NONE..SVCRT_LOG_DEBUG
- `tag`：标签（短字符串，NUL 结尾，位于调用者自身固件或 RAM 均可）
- `msg`：正文（NUL 结尾）
**返回**：0=已提交内核；-1=参数被内核拒绝

### `SVCRT_UFMT_INLINE int32 svcrt_log_printf(uint32 level, const char *tag`

```c
SVCRT_UFMT_INLINE int32 svcrt_log_printf(uint32 level, const char *tag, const char *fmt, ...) {
```

**格式化并输出一条日志**
- `level`：级别
- `tag`：标签
- `fmt`：格式串（子集见文件头说明）
> 说明：在用户侧完成格式化，只需一条 SVC；不需要额外的符号实现。

### `SVCRT_UFMT_INLINE int32 svcrt_shell_printf(const char *fmt, ...) {`

```c
SVCRT_UFMT_INLINE int32 svcrt_shell_printf(const char *fmt, ...) {
```

**格式化并打印到内核 Shell 控制台**
> 说明：与 svcrt_log_printf 同样在用户侧完成格式化。

## kernelsrc/include/svcrt_ushell.h

**用户态 Shell 控制台服务：描述符与接口（SVC 0x1A）**
内核分发层需要按同样的内存布局读取用户传来的命令描述符，
用户侧要按同样的布局填写它，所以结构体与长度上限定义在这一个
头文件里，两侧共同包含，避免「各写一份、改一处忘一处」。

### `typedef struct {`

```c
typedef struct {
```

**用户态 Shell 命令描述符**
> 说明：注册时内核会复制 name / help 指向的字符串，所以用户侧可以用
字符串常量，也可以在注册完成后释放自己的缓冲。
func 必须是注册者自身固件区内的函数：内核会按「调用者自己的
ROM 区间」校验，防止借 Shell 的特权态去执行别人的代码。

### `int32 svcrt_shell_cmd_register(const svcrt_ushell_cmd_t *cmd);`

```c
int32 svcrt_shell_cmd_register(const svcrt_ushell_cmd_t *cmd);
```

**在内核 Shell 控制台上注册一条命令**
- `cmd`：描述符（必须位于用户可见 RAM）
**返回**：SVCRT_USHELL_OK 或负错误码

### `int32 svcrt_shell_cmd_unregister(const char *name);`

```c
int32 svcrt_shell_cmd_unregister(const char *name);
```

**注销一条命令（按名字，不区分大小写）**
**返回**：SVCRT_USHELL_OK 或负错误码

### `int32 svcrt_shell_print(const char *msg);`

```c
int32 svcrt_shell_print(const char *msg);
```

**向内核 Shell 控制台输出一段文本**
- `msg`：NUL 结尾字符串；是否换行由调用者决定（要换行就带 "\r\n"）
**返回**：SVCRT_USHELL_OK 或负错误码

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

## kernelsrc/sdk/posix/errno.h

**POSIX errno for SVCrtOS Apps.**
errno is per thread. SVCrtOS reaches the kernel through SVC, but
errno itself is pure userspace state - no kernel capability is
involved - so it is stored here, in the App's own RAM, in one cell

### `int32 *svcrt_posix_errno_location(void);`

```c
int32 *svcrt_posix_errno_location(void);
```

Address of the calling thread's errno cell; never NULL. */

## kernelsrc/sdk/posix/pthread.h

**POSIX threads for SVCrtOS Apps.**
SVCrtOS has threads, a scheduler and synchronization, but no user
mode way to create a thread: a thread needs a TCB and a scheduler
slot, and only the kernel owns those. pthread_create therefore goes

### `typedef struct {`

```c
typedef struct {
```

Thread attributes. Anything left at 0 takes the built-in default, so a
plain pthread_create(t, NULL, fn, arg) call needs no attributes at all. */

## kernelsrc/sdk/posix/semaphore.h

**POSIX unnamed semaphores for SVCrtOS Apps.**
Backed by the kernel semaphore service (SVC 0x15). sem_t holds the
kernel handle, so sem_init / sem_destroy are the only lifecycle
calls and no static storage is needed.

### `typedef struct {`

```c
typedef struct {
```

Opaque to the caller: only svcrt_posix.c knows what is inside. */

## kernelsrc/sdk/posix/svcrt_posix_types.h

**Base POSIX types for the SVCrtOS compatibility layer.**
Kept in one place so every POSIX header agrees on the width of
ssize_t / off_t / time_t. The widths are fixed (int32 / uint32)
because SVCrtOS is a 32-bit only kernel and pretending otherwise

### `#define SVCRT_CLOCK_REALTIME   0 #define SVCRT_CLOCK_MONOTONIC  1`

```c
#define SVCRT_CLOCK_REALTIME   0 #define SVCRT_CLOCK_MONOTONIC  1
```

POSIX clock ids. Only MONOTONIC is backed by anything here: SVCrtOS has a
boot relative tick, there is no wall clock, so REALTIME is aliased to it
rather than reported as a different (and wrong) time base. */

## kernelsrc/sdk/posix/unistd.h

**Minimal <unistd.h> for SVCrtOS Apps.**
Maps the POSIX names a portable C program actually uses onto the
App SDK: read/write/close act on a device handle, sleep/usleep map
onto the scheduler, and there is no process model at all (fork,

### `uint32 svcrt_posix_sleep(uint32 seconds);`

```c
uint32 svcrt_posix_sleep(uint32 seconds);
```

Suspend the calling thread for whole seconds (scheduler based). */

### `int    svcrt_posix_usleep(svcrt_useconds_t usec);`

```c
int    svcrt_posix_usleep(svcrt_useconds_t usec);
```

Suspend the calling thread for microseconds. SVCrtOS waits in ms, so the
value is rounded UP: a sleep never returns early. */

### `int    svcrt_posix_nanosleep(const struct timespec *req, struct timesp`

```c
int    svcrt_posix_nanosleep(const struct timespec *req, struct timespec *rem);
```

Suspend until the given absolute time on the chosen clock. */

### `svcrt_time_t svcrt_posix_time(svcrt_time_t *tloc);`

```c
svcrt_time_t svcrt_posix_time(svcrt_time_t *tloc);
```

Wall clock seconds since boot - SVCrtOS has no RTC. */

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

**SVCrtOS 内核内安装模块实现（常驻任务 + 命令触发一次性窗口）**
安装流程：
1) 打开镜像接收设备（默认 COM1）；
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

### `static int32 svcrt_installer_commit(int32 dev) {`

```c
static int32 svcrt_installer_commit(int32 dev) {
```

**把已收全的镜像头对应的负载落盘，并按自启标志决定是否立即启动**
- `dev`：设备句柄（位置正好在镜像头之后）
**返回**：成功返回槽位号（>=0），失败返回 SVCRT_LOADER_ERR_x
「开机是否自启」只由镜像头 flags 决定（tools/pack_app.py --autostart /
--no-autostart 写入，裸镜像走 svcrt_loader_identify() 的全局默认）。
安装路径与开机扫描路径用同一套判据，避免同一个镜像「烧录自启、
安装不自启」这种不一致。

## kernelsrc/src/svcrt_loader.c

**在池内找一段足够长、且不越过压实余量的已擦除空间**
- `size`：需要的字节数
- `skip_base`：需要避开的区间起点（0 = 不回避）
- `skip_size`：需要避开的区间长度

### `#include "svcrt_loader.h" #include "svcrt_ptable.h" #include "svcrt_sh`

```c
#include "svcrt_loader.h" #include "svcrt_ptable.h" #include "svcrt_share.h" #include "svcrt_layout_def.h"   /* strategy constants (reclaim mode) */ #include "svcrt_layout.h"       /* effective mode + fixed slot table */ #include "svcrt_app_image.h"
```

**SVCrtOS 镜像加载器实现（动态装载：安装 / 扫描 / 重定位 / 搬移 / 回收 / 启停）**
加载流程：
1) 校验镜像头（魔数 / 兼容签名 / 长度 / 入口 / 重定位表）；
2) 在池里找第一段足够长的**已擦除**空间（首适配），
因此镜像按实际长度紧邻排列，密度 = 实际占用之和；
3) 分配镜像 RAM 块（2 的幂、按块对齐），登记槽位记录；
4) 写入镜像头（state = UNCOMMITTED）；
5) 流式接收重定位表并落盘；
6) 流式接收负载，逐块按 ROM / RAM 增量就地打补丁后落盘；
7) CRC32 复核；成功后唯一的一个字写入：state = VALID（提交）；
8) 置 LOADED，返回槽位号。
启动时把入口地址注册为内核任务（优先级 / 栈 / 周期按镜像类型取分区配置）。
为什么分配不维护任何元数据：Flash 里「连续 0xFF 段」就是可用空间，
而活镜像必然含非 0xFF 字节（镜像头魔数），所以一次物理扫描同时
完成了「找空位」与「避开已装镜像」。重启后不需要恢复分配器状态，
也不会出现「元数据与实际内容不一致」这种只能靠猜的故障。
回收同样受物理规律约束：擦除粒度是整扇区，因此一个扇区里只要还有
活镜像就不能擦。svcrt_loader_reclaim() 先把该扇区的活镜像搬到池内
更低处的已擦除空位（应用重定位，走 UNCOMMITTED -> VALID 事务），
再擦扇区。搬不动的（正在运行 / 无空位）跳过，不影响其它扇区。
> 说明：本模块只能由内核特权态调用（经 SVC 0x18 分发）。

### `static uint32 svcrt_loader_mov_imm16(uint16 h1, uint16 h2) {`

```c
static uint32 svcrt_loader_mov_imm16(uint16 h1, uint16 h2) {
```

**从 MOVW/MOVT 的一对半字里取出 imm16**
- `h1`：前缀半字（11110 i 10 0100 imm4）
- `h2`：第二个半字（0 imm3 Rd imm8）

### `static void svcrt_loader_mov_set_imm16(uint16 *p_h1, uint16 *p_h2, uin`

```c
static void svcrt_loader_mov_set_imm16(uint16 *p_h1, uint16 *p_h2, uint32 imm16) {
```

**把 imm16 写回 MOVW/MOVT 的一对半字（Rd 与指令种类保持不变）**

### `static void svcrt_loader_reloc_movw(uint8 *p, uint32 delta) {`

```c
static void svcrt_loader_reloc_movw(uint8 *p, uint32 delta) {
```

**给一对相邻的 MOVW+MOVT 立即数打补丁**
- `p`：指向 MOVW 指令（半字对齐）
- `delta`：要叠加到这条 32 位地址上的增量
编译器把 32 位绝对地址拆成「MOVW 低 16 位 + MOVT 高 16 位」两条指令时，
链接期填进去的是指令内的立即数，字节变化不是「整字加增量」，只能解码
成 32 位值、加上增量、再重新编码。低 16 位会被进位的连带影响，所以两条
必须一起算，不能拆成两个独立表项。

### `static uint32 svcrt_loader_reloc_apply(uint8 *buf, uint32 buf_off, uin`

```c
static uint32 svcrt_loader_reloc_apply(uint8 *buf, uint32 buf_off, uint32 len, const svcrt_app_header_t *p_hdr, const uint32 *p_rel, uint32 *p_idx, uint32 delta_rom, uint32 delta_ram) {
```

**给一段负载打重定位补丁，并给出本块可以安全落盘的末尾**
- `buf`：装载缓冲（对应镜像内偏移 buf_off 起的连续 len 字节）
- `buf_off`：该缓冲在镜像内的起始偏移
- `len`：缓冲长度
- `p_hdr`：镜像头（提供表项数与两条标称基址）
- `p_rel`：重定位表表体地址（流式安装指向刚落盘的 Flash 表，
池内搬移指向旧副本的表，RAM 缓冲路径指向缓冲里的表）
- `idx`：重定位表前进指针（跨块调用时必须复用同一个变量）
- `delta_rom`：本次要给 ROM 类表项叠加的增量
- `delta_ram`：本次要给 RAM 类表项叠加的增量
**返回**：本块可以落盘的末尾偏移（镜像内）。正常等于 buf_off + len；若块尾
恰好切开一条指令类表项，则等于该表项的起始偏移，调用者必须把这段
之后的字节留到下一块开头一起处理。
表项按偏移升序，只要记住走到第几条，就能边收边改，不必把整表读进
RAM。数据类表项是整字、4 字节对齐，永远不会跨块；指令类表项
（MOVW/MOVT）长 8 字节且只保证半字对齐，「表项不跨块」这条对它们
不成立，所以要把可提交长度交回调用者。

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
