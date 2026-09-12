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
void   svcrt_event_wait(int32 handle, int32 timeout);

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
* @param timeout 超时时间（ms），非正值（<=0）表示永久等待
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
* @param timeout 超时时间（ms），非正值（<=0）表示永久等待
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
* @param timeout   队列满时的等待时间（ms），负值表示永久等待
* @return 0=成功，负值=失败
*/
int32  svcrt_mq_send(int32 handle, void *buf, int32 len_words, int32 timeout);

/**
* @brief 接收消息（阻塞式）
* @param handle    消息队列句柄
* @param buf       接收缓冲区
* @param len_words 缓冲区可容纳的字数
* @param timeout   超时时间（ms），负值表示永久等待
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
* @return 0=成功，负值为错误码（见 svcrt_loader.h）
* @note 镜像头的 type 必须为驱动（SVCRT_APP_TYPE_DRIVER）；
*       驱动区为单入口，写入前会整体擦除目标区间。
*/
int32  svcrt_driver_load(int32 dev, uint32 image_len);
/** @} */

#endif
