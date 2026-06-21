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

#endif
