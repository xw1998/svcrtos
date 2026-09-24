/**
* @file svcrt.h
* @brief SVCrtOS 应用 API 头文件（用户态唯一需要包含的头文件）
* @details 应用程序（用户态）包含本文件即可使用全部操作系统接口。
*          所有调用最终通过 SVC 指令陷入内核态执行，对用户透明，
*          用法与普通函数一致。
* @note 本文件只含声明；实现分别位于 kernelsrc/app/oslib.c（内核工程内应用）
*       与 kernelsrc/sdk/app_sdk/svcrt_oslib.c（独立应用固件）。
* @author xw
* @date 2026.05.03
*/

#ifndef __SVCRT_H__
#define __SVCRT_H__

#include "svcrt_types.h"
#include "svcrt_ulog.h"
#include "svcrt_version.h"

/** @defgroup sync_ret 同步原语返回码
 *  @{
 *  与内核 svcrt_def.h 保持同一组数值。内核头文件对应用不可见，
 *  而这些数值是接口契约的一部分（调用方要据此判断“本次没拿到资源”），
 *  所以在这里再给一份，避免文档里提到的名字在应用侧拿不到。
 *  内核编译单元会同时包含两个头文件，#ifndef 保证不会重定义。
 */
#ifndef SVCRT_SYNC_OK
#define SVCRT_SYNC_OK             (0)
#endif
#ifndef SVCRT_SYNC_ERR_PARAM
#define SVCRT_SYNC_ERR_PARAM      (-1)
#endif
#ifndef SVCRT_SYNC_ERR_TIMEOUT
#define SVCRT_SYNC_ERR_TIMEOUT    (-2)
#endif
#ifndef SVCRT_SYNC_ERR_DELETED
#define SVCRT_SYNC_ERR_DELETED    (-3)
#ifndef SVCRT_SYNC_ERR_WOULDBLOCK
#define SVCRT_SYNC_ERR_WOULDBLOCK (-4)
#endif
#endif
/** @} */

/** @defgroup task 任务管理
 *  @{ */

/**
* @brief 当前任务睡眠指定毫秒（阻塞调度）
* @param ms 睡眠时间，单位毫秒
* @note 睡眠期间任务状态为等待，CPU 交给其他就绪任务。
*/
void   svcrt_task_wait(uint32 ms);

/**
* @brief 等待当前任务的周期结束（周期任务同步用）
* @note 任务须在配置表中设置了周期；未到期即阻塞，到期自动唤醒。
*/
void   svcrt_task_wait_period(void);

/**
* @brief 微秒级忙等延时（不引起任务切换）
* @param us 延时时间，单位微秒
* @note 占用 CPU，只适合极短延时（如外设时序），长延时应使用 svcrt_task_wait。
*/
void   svcrt_task_delay(uint32 us);

/**
* @brief 终止当前任务
* @note 仅能终止自身；任务槽位转为 INVALID，可经 svcrt_task_recover_req 恢复。
*/
void   svcrt_task_kill(void);
/** @} */

/** @defgroup sysinfo 系统信息
 *  @{ */

/**
* @brief 获取系统运行时间
* @return 自内核启动以来的毫秒数
*/
uint32 svcrt_get_time_ms(void);

/**
* @brief 获取 CPU 空闲率
* @return 空闲率（百分比 0~100）
* @note 需开启 SVCRT_USE_CPU_LOAD 配置。
*/
uint32 svcrt_get_cpu_usage(void);
/** @} */

/** @defgroup event 事件
 *  @{ */

/**
* @brief 创建命名事件
* @param name 事件名（字符串）
* @return 事件句柄，失败返回负值
*/
int32  svcrt_event_create(char *name);

/**
* @brief 等待事件触发
* @param handle  事件句柄
* @param timeout 超时时间（ms），0 表示不等待，负值表示永久等待
*/
int32  svcrt_event_wait(int32 handle, int32 timeout);

/**
* @brief 触发事件，唤醒所有等待该事件的任务
* @param handle 事件句柄
*/
void   svcrt_event_set(int32 handle);
/** @} */

/** @defgroup sem 信号量
 *  @{ */

/**
* @brief 创建计数信号量
* @param name       信号量名
* @param init_count 初始计数值
* @return 信号量句柄，失败返回负值
*/
int32  svcrt_sem_create(char *name, int32 init_count);

/**
* @brief 等待信号量（计数减一，计数为 0 时阻塞）
* @param handle  信号量句柄
* @param timeout 超时时间（ms），0 表示不等待，负值表示永久等待
* @return 0=成功；SVCRT_SYNC_ERR_TIMEOUT(-2)=等待超时（未获得资源）；其它负值=参数错误
*/
int32  svcrt_sem_wait(int32 handle, int32 timeout);

/**
* @brief 释放信号量（唤醒等待者或计数加一）
* @param handle 信号量句柄
* @return 0=成功，负值=失败
*/
int32  svcrt_sem_post(int32 handle);

/**
* @brief 删除信号量
* @param handle 信号量句柄
* @return 0=成功，负值=失败
*/
int32  svcrt_sem_delete(int32 handle);
/** @} */

/** @defgroup mutex 互斥锁
 *  @{ */

/**
* @brief 创建互斥锁（带优先级继承，防止优先级反转）
* @param name 互斥锁名
* @return 互斥锁句柄，失败返回负值
*/
int32  svcrt_mutex_create(char *name);

/**
* @brief 加锁
* @param handle  互斥锁句柄
* @param timeout 超时时间（ms），0 表示不等待，负值表示永久等待
* @return 0=成功；SVCRT_SYNC_ERR_TIMEOUT(-2)=等待超时（未获得资源）；其它负值=参数错误
*/
int32  svcrt_mutex_lock(int32 handle, int32 timeout);

/**
* @brief 解锁（仅持有者可解锁）
* @param handle 互斥锁句柄
* @return 0=成功，负值=失败
*/
int32  svcrt_mutex_unlock(int32 handle);

/**
* @brief 删除互斥锁
* @param handle 互斥锁句柄
* @return 0=成功，负值=失败
*/
int32  svcrt_mutex_delete(int32 handle);
/** @} */

/** @defgroup cond 条件变量
 *  @{ */

/**
* @brief 创建条件变量
* @param name 条件变量名（最长 7 个字符）
* @return 条件变量句柄，失败返回负值
*/
int32  svcrt_cond_create(char *name);

/**
* @brief 等待条件（原子地释放互斥锁并挂起，醒来后重新持有互斥锁）
* @param cond_handle  条件变量句柄
* @param mutex_handle 当前任务已持有的互斥锁句柄
* @param timeout      超时时间（ms），负值表示永久等待；0 不支持
*                     （条件变量必须“释放互斥量后等待”，给 0 只会得到一个参数错误）
* @return 0=被 signal/broadcast 唤醒；SVCRT_SYNC_ERR_TIMEOUT(-2)=超时；
*         SVCRT_SYNC_ERR_DELETED(-3)=等待期间对象被删除；其它负值=参数错误
* @note 无论返回什么，返回时都已重新持有 mutex_handle（POSIX 契约）。
* @note 调用前必须先持有 mutex_handle，否则返回参数错误；
*       释放锁与入等待队列在内核内部一次做完，不会丢失 signal。
*/
int32  svcrt_cond_wait(int32 cond_handle, int32 mutex_handle, int32 timeout);

/**
* @brief 唤醒一个等待者（没有等待者时不算错误）
* @param handle 条件变量句柄
* @return 0=成功，负值=失败
*/
int32  svcrt_cond_signal(int32 handle);

/**
* @brief 唤醒全部等待者
* @param handle 条件变量句柄
* @return 0=成功，负值=失败
*/
int32  svcrt_cond_broadcast(int32 handle);

/**
* @brief 删除条件变量（唤醒全部等待者并告知对象已删除）
* @param handle 条件变量句柄
* @return 0=成功，负值=失败
*/
int32  svcrt_cond_delete(int32 handle);
/** @} */

/** @defgroup dev 设备 IO
 *  @{ */

/**
* @brief 打开设备
* @param name  设备名（内置驱动或独立驱动注册的名字）
* @param param 设备相关参数（由驱动解释，无参数填 0）
* @return 设备句柄，失败返回负值
*/
int32  svcrt_dev_open(char *name, uint32 param);

/**
* @brief 关闭设备
* @param handle 设备句柄
* @return 0=成功，负值=失败
*/
int32  svcrt_dev_close(int32 handle);

/**
* @brief 从设备读取数据
* @param handle 设备句柄
* @param pdata  接收缓冲区
* @param len    期望读取长度（字节）
* @return 实际读取长度，负值表示错误
*/
int32  svcrt_dev_read(int32 handle, void *pdata, int32 len);

/**
* @brief 向设备写入数据
* @param handle 设备句柄
* @param pdata  发送缓冲区
* @param len    写入长度（字节）
* @return 实际写入长度，负值表示错误
*/
int32  svcrt_dev_write(int32 handle, void *pdata, int32 len);

/**
* @brief 设备控制命令
* @param handle 设备句柄
* @param code   控制命令码（由驱动定义）
* @param value  命令参数
* @return 0 或正值=成功，负值=失败
*/
int32  svcrt_dev_ctrl(int32 handle, uint32 code, uint32 value);
/** @} */

/** @defgroup mq 消息队列
 *  @{ */

/**
* @brief 创建消息队列
* @param name 队列名
* @return 消息队列句柄，失败返回负值
* @note 队列深度与单条消息长度由 SVCRT_MQ_DEPTH / SVCRT_MQ_MSG_WORDS 配置。
*/
int32  svcrt_mq_create(char *name);

/**
* @brief 发送消息（消息按字拷贝入队）
* @param handle    消息队列句柄
* @param buf       消息缓冲区
* @param len_words 消息长度（以 32 位字为单位）
* @param timeout   队列满时的等待时间（ms），0 表示不等待，负值表示永久等待
* @return 0=成功，负值=失败
*/
int32  svcrt_mq_send(int32 handle, void *buf, int32 len_words, int32 timeout);

/**
* @brief 接收消息（阻塞式）
* @param handle    消息队列句柄
* @param buf       接收缓冲区
* @param len_words 缓冲区可容纳的字数
* @param timeout   超时时间（ms），0 表示不等待，负值表示永久等待
* @return 实际收到的字数，负值=超时或错误
*/
int32  svcrt_mq_recv(int32 handle, void *buf, int32 len_words, int32 timeout);

/**
* @brief 删除消息队列
* @param handle 消息队列句柄
* @return 0=成功，负值=失败
*/
int32  svcrt_mq_delete(int32 handle);
/** @} */

/** @defgroup timer 软定时器
 *  @{ */

/**
* @brief 创建软定时器
* @param name 定时器名
* @return 定时器句柄，失败返回负值
*/
int32  svcrt_timer_create(char *name);

/**
* @brief 启动定时器
* @param handle   定时器句柄
* @param period_ms 定时周期（ms）
* @param mode     0=单次触发，1=周期触发（SVCRT_TIMER_MODE_x）
* @param cb       到期回调函数
* @param arg      回调参数
* @return 0=成功，负值=失败
* @note 回调在专用定时器任务上下文中执行，不在中断内运行，
*       因此回调中可以安全调用其它系统调用。
*/
int32  svcrt_timer_start(int32 handle, uint32 period_ms, uint32 mode,
                         void (*cb)(void *), void *arg);

/**
* @brief 停止定时器
* @param handle 定时器句柄
* @return 0=成功，负值=失败
*/
int32  svcrt_timer_stop(int32 handle);

/**
* @brief 删除定时器
* @param handle 定时器句柄
* @return 0=成功，负值=失败
*/
int32  svcrt_timer_delete(int32 handle);
/** @} */

/** @defgroup diag 任务状态与故障诊断
 *  @{ */

/**
* @brief 查询任务状态
* @param task_id 任务号（从 1 开始）
* @return 任务状态枚举值（svcrt_task_status_t），-1 表示任务号非法
*/
int32  svcrt_task_status_get(int32 task_id);

/**
* @brief 请求恢复（重启）指定任务
* @param task_id 任务号（从 1 开始）
* @return 0=成功，-1=参数非法
* @note 重建任务栈帧并重新调度，用于任务级故障恢复。
*/
int32  svcrt_task_recover_req(int32 task_id);

/**
* @brief 获取故障记录条数
* @return 当前环形缓冲区中已记录的故障条数
*/
int32  svcrt_fault_record_count(void);

/**
* @brief 读取一条故障记录
* @param index 记录序号（从 0 开始）
* @param out3  用户缓冲区（3 个字）：[0]=故障类型，[1]=任务号，[2]=发生时刻 tick
* @return 0=成功，负值=失败
*/
int32  svcrt_fault_record_read(int32 index, uint32 *out3);
/** @} */

/** @defgroup isr 中断安全 API（仅限中断服务程序/特权态调用）
 *  @{ */

/**
* @brief 中断中释放信号量（不阻塞、不引起切换）
* @param handle 信号量句柄
* @return 0=成功，负值=失败
* @note 该接口直接在内核态执行，不经 SVC，只允许中断服务程序调用。
*/
int32  svcrt_sem_post_from_isr(int32 handle);

/**
* @brief 中断中触发事件
* @param handle 事件句柄
* @return 0=成功，负值=失败
*/
int32  svcrt_event_set_from_isr(int32 handle);

/**
* @brief 中断中发送消息
* @param handle    消息队列句柄
* @param buf       消息缓冲区
* @param len_words 消息长度（字）
* @return 0=成功，负值=失败
*/
int32  svcrt_mq_send_from_isr(int32 handle, void *buf, int32 len_words);
/** @} */

/** @defgroup crit 调度器锁（用户态临界区）
 *  @{ */

/**
* @brief 进入临界区（禁止任务切换，不关闭中断）
* @details 相当于 RT-Thread 的 rt_enter_critical，支持嵌套调用。
*          适用于保护较长的共享数据访问，开销小于关中断。
* @note 临界区内禁止调用阻塞接口（如 wait/sem_wait），
*       否则阻塞请求会被忽略并记录一条 SVCRT_FAULT_SCHEDLOCK 故障记录。
*/
void   svcrt_sched_lock(void);

/**
* @brief 退出临界区
* @note 须与 svcrt_sched_lock 成对调用；计数归零时恢复任务切换。
*/
void   svcrt_sched_unlock(void);

/**
* @brief 查询调度器锁嵌套层数
* @return 当前嵌套层数（0 表示未处于临界区）
*/
int32  svcrt_sched_lock_count(void);
/** @} */

/** @defgroup stack 任务栈使用量分析
 *  @{ */

/**
* @brief 查询任务栈使用峰值
* @param task_id 任务号（从 1 开始）
* @param out3    用户缓冲区（3 个字）：
*                [0]=栈总字节数，[1]=峰值已用字节数，[2]=剩余字节数
* @return 0=成功，-1=参数非法
* @details 采用两种手段并取较大值：栈填充图案扫描 + 上下文切换最低栈指针记录。
*          建议按峰值用量保留 30% 以上余量再减小任务栈配置。
* @note 需开启 SVCRT_USE_STACK_USAGE 配置。
*/
int32  svcrt_task_stack_info(int32 task_id, uint32 *out3);
/** @} */

/** @defgroup appmgr App 镜像与分区管理
 *  @{ */

/**
* @brief 从设备加载 App 镜像到空闲槽位
* @param dev       已打开的设备句柄（数据从镜像头开始）
* @param image_len 期望镜像总长（含头），传 0 表示由镜像头决定
* @return 成功返回槽位号(>=0)，失败返回负错误码（见 svcrt_loader.h）
* @note 镜像会先擦后写并做 CRC 回读校验。
*/
int32  svcrt_app_load(int32 dev, uint32 image_len);

/**
* @brief 启动槽位中的 App
* @param slot 槽位号
* @return 成功返回任务号(>0)，失败返回负错误码
*/
int32  svcrt_app_start(uint32 slot);

/**
* @brief 停止槽位中的 App（镜像保留在 Flash）
* @param slot 槽位号
* @return 0=成功，负值为错误码
*/
int32  svcrt_app_stop(uint32 slot);

/**
* @brief 查询槽位状态
* @param slot 槽位号
* @return 0=空（EMPTY）,1=已加载（LOADED）,2=运行中（RUNNING）；
*         槽位非法返回 0xffffffff
*/
uint32 svcrt_app_status(uint32 slot);

/**
* @brief 从设备安装驱动镜像到驱动区
* @param dev       已打开的设备句柄（数据从镜像头开始）
* @param image_len 期望镜像总长（含头），传 0 表示由镜像头决定
* @return 成功返回驱动槽位号（>=0），负值为错误码（见 svcrt_loader.h）
* @note 镜像头的 type 必须为驱动（SVCRT_APP_TYPE_DRIVER）；
*       镜像头的 load_addr 必须等于某个驱动槽位基址，写入前只擦除该槽区间。
*/
int32  svcrt_driver_load(int32 dev, uint32 image_len);

/** @defgroup thread 用户态线程服务（SVC 0x1B）
 *  @{
 */

/**
* @brief 在当前 App/驱动的 RAM 里创建一个线程
* @param entry      线程入口，必须位于调用者自己的固件窗口内
* @param stack      栈底地址，整段栈必须位于调用者自己的 RAM 窗口内
* @param stack_size 栈字节数（最小 128）
* @param priority   内核优先级（1~254，数值越小越高；255 是调度器哨兵，会被拒绝）
* @param period_ms  0 = 事件驱动（默认）；非 0 = 周期任务
* @return 任务号（>0），失败返回负值
* @note 内核只接受"调用者自己的"入口与栈：不能借别人的 RAM 建栈，
*       也不能把入口指到内核或别的 App 里去。
*/
int32  svcrt_thread_create(void (*entry)(void), void *stack, uint32 stack_size,
                           uint32 priority, uint32 period_ms);

/**
* @brief 取当前线程的内核任务号
* @return 任务号（>0）；在内核自带任务上返回 0
*/
int32  svcrt_thread_self(void);

/**
* @brief 结束当前线程，不再返回
*/
void   svcrt_thread_exit(void);
/** @} */

/** @} */


/** @defgroup file 文件服务（SVC 0x1C）
 *  @{ */

/*
* The file system lives in the kernel; an App only sees paths. Every call
* is path based (open / move the bytes / close), so no kernel side handle
* survives the call and nothing has to be cleaned up when an App is
* stopped. Mounting and formatting stay with the shell and the kernel -
* an App can use the volume that is already mounted, nothing else.
*
* Errors: 0 on success, a negative value on failure. Values at or below
* -1000 are SVCrtOS codes (see svcrt_fs.h), anything above is a littlefs
* error passed through as-is, so print the number instead of guessing a
* cause.
*/

/**
* @brief Write a whole file (create or truncate).
* @param path absolute path inside the mounted volume, e.g. "/cfg/a.txt"
* @param data bytes to write (may be NULL when len is 0)
* @param len  byte count, 0 creates an empty file
* @return 0 on success, negative error code on failure
*/
int32  svcrt_file_write(const char *path, const void *data, uint32 len);

/**
* @brief Read a whole file into a buffer of the caller's own RAM.
* @param path absolute path inside the mounted volume
* @param buf  destination buffer
* @param max  capacity of that buffer in bytes
* @return bytes read (>= 0), or a negative error code
*/
int32  svcrt_file_read(const char *path, void *buf, uint32 max);

/**
* @brief Remove a file.
* @return 0 on success, negative error code on failure
*/
int32  svcrt_file_remove(const char *path);

/**
* @brief Size and type of one entry.
* @param path   absolute path
* @param size   receives the size in bytes (may be NULL)
* @param is_dir receives 1 for a directory, 0 for a file (may be NULL)
* @return 0 on success, negative error code on failure
*/
int32  svcrt_file_stat(const char *path, uint32 *size, uint32 *is_dir);

/**
* @brief Capacity and usage of the mounted volume.
* @param total receives the total bytes
* @param used  receives the used bytes
* @return 0 on success, -1 when no volume is mounted
*/
/**
 * @brief Report in, and optionally declare how often this image will do so.
 * @param period_ms 0 = withdraw the contract; otherwise the longest gap the
 *        image promises to keep, in milliseconds. Must be >= 100 ms: a shorter
 *        promise cannot be checked by sampling, and the kernel refuses it
 *        rather than watch the image against numbers it cannot verify.
 * @return 0 accepted, -1 refused (bad period, or no slot to attribute it to).
 * @note The kernel checks the window only while this task is runnable; a task
 *       blocked in a kernel wait is exempt. A periodic task that never calls
 *       this is reported by `guard` as "no contract" - not as healthy.
 */
int32  svcrt_heartbeat(uint32 period_ms);

int32  svcrt_file_info(uint32 *total, uint32 *used);

/**
* @brief Mount the default volume (the same call as `fs mount`).
* @return 0 on success, negative error code on failure
* @note A mount does not survive a reset, so an App that needs the
*       volume asks for it instead of assuming it is still there.
*/
int32  svcrt_file_mount(void);

/**
* @brief Release the mounted volume.
* @return 0 on success, negative error code on failure
*/
int32  svcrt_file_unmount(void);

/**
* @brief Read len bytes starting at off, past EOF just stops short.
* @param path  absolute path inside the mounted volume
* @param buf   destination buffer
* @param len   bytes wanted
* @param off   byte offset to start from
* @return bytes read (>= 0), or a negative error code
* @note Same units as svcrt_file_read(): the count comes back in the
       return value, so a short read is visible without an extra out
       parameter an App could leave NULL.
*/
int32  svcrt_file_read_at(const char *path, void *buf, uint32 len, uint32 off);

/**
* @brief Rename or move one entry.
* @return 0 on success, negative error code on failure
*/
int32  svcrt_file_rename(const char *from, const char *to);

/**
* @brief List entry names of a directory into a caller buffer.
* @param dir      absolute path, "/" for the volume root
* @param out      destination buffer
* @param out_size capacity of that buffer in bytes
* @param count    receives how many names were stored (may be NULL)
* @return 0 on success, negative error code on failure
* @note Names are stored back to back, each NUL terminated, and a
*       name that does not fit in full is left out rather than cut.
*       The App gets a flat list of names, no types and no sizes.
*/
int32  svcrt_file_list_names(const char *dir, char *out, uint32 out_size,
                             uint32 *count);

/**
* @defgroup path The VFS namespace: open / read / write / stat / dirent
*  @{
*/

/*
* The group above works on one volume and eats a whole file per call. This
* one is the namespace the whole board lives in, and it keeps a handle open
* between calls - which is what a POSIX style program actually expects:
*
*     h = svcrt_path_open("/mnt/nor/a.txt",
*                         SVCRT_PATH_O_RDWR | SVCRT_PATH_O_CREAT);
*     if (h >= 0) { svcrt_path_write(h, "hi", 2); svcrt_path_close(h); }
*
* The tree:
*     /             volatile scratch area (ramfs, gone on reset)
*     /dev          the device registry, so /dev/uart0 opens like a file
*     /mnt/<name>   a persistent volume, mounted explicitly by the shell
*
* Paths must be absolute. A handle belongs to the task that opened it:
* another task cannot use it, and the kernel hands it back when the owner
* exits or is killed, so a crashed App does not keep the kernel's handle
* table (a small, fixed array) occupied.
*
* Errors: 0 on success, a negative value on failure. -1..-15 are the VFS
* error codes (no such file, is a directory, read only, ...); print the
* number rather than guessing a cause. -12 (ENOSYS) specifically means this
* build has no VFS at all - not "your path is missing".
*/

/* Open flags and seek origins. The values are POSIX's (the kernel pins its
 * own copy to the same numbers), so an App that also uses the POSIX layer
 * can pass <fcntl.h>'s O_* / SEEK_* straight through. */
#define SVCRT_PATH_O_RDONLY     (0x0000u)
#define SVCRT_PATH_O_WRONLY     (0x0001u)
#define SVCRT_PATH_O_RDWR       (0x0002u)
#define SVCRT_PATH_O_ACCMODE    (0x0003u)
#define SVCRT_PATH_O_CREAT      (0x0004u)
#define SVCRT_PATH_O_TRUNC      (0x0008u)
#define SVCRT_PATH_O_APPEND     (0x0010u)
#define SVCRT_PATH_O_DIRECTORY  (0x0020u)

#define SVCRT_PATH_SEEK_SET     (0u)
#define SVCRT_PATH_SEEK_CUR     (1u)
#define SVCRT_PATH_SEEK_END     (2u)

/* File type bits in svcrt_path_stat()'s mode - the usual POSIX S_IFMT set. */
#define SVCRT_PATH_S_IFCHR      (0x2000u)
#define SVCRT_PATH_S_IFDIR      (0x4000u)
#define SVCRT_PATH_S_IFREG      (0x8000u)
#define SVCRT_PATH_S_IFMT       (0xF000u)

/** @brief Longest accepted namespace path, NUL included. */
#define SVCRT_PATH_MAX          (64u)

/**
* @brief Open a file (or a directory) at an absolute namespace path.
* @param path  absolute path, e.g. "/mnt/nor/a.txt" or "/dev/uart0"
* @param flags SVCRT_PATH_O_* combination
* @return a handle (>= 0) on success, a negative error code on failure
* @note Directories open read only and take SVCRT_PATH_O_DIRECTORY; they are
*       there for svcrt_path_readdir(). Writing a directory is refused
*       rather than quietly ignored. A path that is neither in / nor under a
*       mounted point fails - the namespace does not invent a volume.
*/
int32  svcrt_path_open(const char *path, uint32 flags);

/**
* @brief Read from the current offset.
* @return bytes read (0 at end of file), or a negative error code
*/
int32  svcrt_path_read(int32 handle, void *buf, uint32 len);

/**
* @brief Write at the current offset.
* @return bytes written, or a negative error code
* @note The volume underneath allows one writer at a time; a second writer
*       gets -15 (busy) instead of a partial write.
*/
int32  svcrt_path_write(int32 handle, const void *buf, uint32 len);

/**
* @brief Move the read/write offset.
* @param whence SVCRT_PATH_SEEK_SET / _CUR / _END
* @return the new absolute offset (>= 0); a negative error code on failure,
*         in which case the offset is left where it was. Seeking past the end
*         of file is allowed (POSIX); whether anything can be written there is
*         up to the filesystem. The value is the offset the kernel tracks, so
*         it is the same on every volume - never read a file system's own
*         seek return value through this facade.
*/
int32  svcrt_path_seek(int32 handle, int32 off, uint32 whence);

/**
* @brief Take the next entry of a directory opened with SVCRT_PATH_O_DIRECTORY.
* @param name      buffer for one entry name (no path, one level only)
* @param name_size capacity of that buffer, including the NUL
* @param mode      receives the file type bits (SVCRT_PATH_S_IF*), may be NULL
* @param size      receives the file size in bytes, may be NULL
* @return 0 = got one; 1 = the directory is exhausted (NOT an error); negative
*         = a real error. A name that does not fit is reported as -13 rather
*         than handed back cut short.
*/
int32  svcrt_path_readdir(int32 handle, char *name, uint32 name_size,
                          uint32 *mode, uint32 *size);

/**
* @brief Return a handle to the kernel. Works for files and directories.
* @return 0 on success, a negative error code on failure
*/
int32  svcrt_path_close(int32 handle);

/**
* @brief Type and size of a path, without opening it.
* @param size receives the size in bytes (may be NULL)
* @param mode receives the type bits (may be NULL)
* @return 0 on success, a negative error code on failure
*/
int32  svcrt_path_stat(const char *path, uint32 *size, uint32 *mode);

/**
* @brief Remove one file.
* @return 0 on success, a negative error code on failure
* @note Files only: this is not the way to remove a directory.
*/
int32  svcrt_path_unlink(const char *path);

/** @} */

#endif
