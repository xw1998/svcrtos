/**
* @brief SVCrtOS 应用 API 头文件
* @details 应用程序（用户态）包含本文件即可使用所有操作系统接口。
*          所有调用最终通过 SVC 指令陷入内核态执行，对用户透明，用法与普通函数一致。
* @author xw
* @date 2026.05.03
*/

#ifndef __SVCRT_H__
#define __SVCRT_H__

#include "svcrt_types.h"

/* ============================================================
 * 任务管理
 * ============================================================ */
void   svcrt_task_wait(uint32 ms);
void   svcrt_task_wait_period(void);
void   svcrt_task_delay(uint32 us);
void   svcrt_task_kill(void);

/* ============================================================
 * 系统信息
 * ============================================================ */
uint32 svcrt_get_time_ms(void);
uint32 svcrt_get_cpu_usage(void);

/* ============================================================
 * 事件
 * ============================================================ */
int32  svcrt_event_create(char *name);
void   svcrt_event_wait(int32 handle, int32 timeout);
void   svcrt_event_set(int32 handle);

/* ============================================================
 * 信号量
 * ============================================================ */
int32  svcrt_sem_create(char *name, int32 init_count);
int32  svcrt_sem_wait(int32 handle, int32 timeout);
int32  svcrt_sem_post(int32 handle);
int32  svcrt_sem_delete(int32 handle);

/* ============================================================
 * 互斥锁
 * ============================================================ */
int32  svcrt_mutex_create(char *name);
int32  svcrt_mutex_lock(int32 handle, int32 timeout);
int32  svcrt_mutex_unlock(int32 handle);
int32  svcrt_mutex_delete(int32 handle);

/* ============================================================
 * 设备 IO
 * ============================================================ */
int32  svcrt_dev_open(char *name, uint32 param);
int32  svcrt_dev_close(int32 handle);
int32  svcrt_dev_read(int32 handle, void *pdata, int32 len);
int32  svcrt_dev_write(int32 handle, void *pdata, int32 len);
int32  svcrt_dev_ctrl(int32 handle, uint32 code, uint32 value);

/* ============================================================
 * 消息队列（SVC 0x16）
 * ============================================================ */
int32  svcrt_mq_create(char *name);
int32  svcrt_mq_send(int32 handle, void *buf, int32 len_words, int32 timeout);
int32  svcrt_mq_recv(int32 handle, void *buf, int32 len_words, int32 timeout);
int32  svcrt_mq_delete(int32 handle);

/* ============================================================
 * 软定时器（SVC 0x17，回调在定时器服务任务上下文执行）
 * ============================================================ */
int32  svcrt_timer_create(char *name);
int32  svcrt_timer_start(int32 handle, uint32 period_ms, uint32 mode,
                         void (*cb)(void *), void *arg);
int32  svcrt_timer_stop(int32 handle);
int32  svcrt_timer_delete(int32 handle);

/* ============================================================
 * 任务状态查询与故障恢复（SVC 0x11 子命令）
 * ============================================================ */
int32  svcrt_task_status_get(int32 task_id);
int32  svcrt_task_recover_req(int32 task_id);
int32  svcrt_fault_record_count(void);
int32  svcrt_fault_record_read(int32 index, uint32 *out3);

/* ============================================================
 * 中断上下文安全 API（仅限 ISR/特权态直接调用，不经 SVC）
 * ============================================================ */
int32  svcrt_sem_post_from_isr(int32 handle);
int32  svcrt_event_set_from_isr(int32 handle);
int32  svcrt_mq_send_from_isr(int32 handle, void *buf, int32 len_words);

/* ============================================================
 * 调度器锁（用户态临界区，SVC 0x11 子命令 7~9）
 * @brief 禁止任务切换（不关闭中断），相当于 RT-Thread 的 rt_enter_critical。
 *        支持嵌套调用，须与 unlock 成对使用；
 *        临界区内不得调用阻塞接口（会被忽略并记录故障）。
 * ============================================================ */
void   svcrt_sched_lock(void);
void   svcrt_sched_unlock(void);
int32  svcrt_sched_lock_count(void);

/* ============================================================
 * 任务栈用量查询（SVC 0x11 子命令 10）
 * @brief 查询指定任务的栈使用峰值（填充图案 + 最高水位两种手段取大值）
 * @param task_id 任务号（从 1 开始）
 * @param out3    用户缓冲区（3 个字）：[0]=栈总字节，[1]=峰值已用字节，[2]=剩余字节
 * @return 0=成功，-1=参数非法
 * ============================================================ */
int32  svcrt_task_stack_info(int32 task_id, uint32 *out3);

#endif
