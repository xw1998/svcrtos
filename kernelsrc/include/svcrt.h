/**
* @brief SVCrtOS 对外API头文件
* @details 供应用程序(分区)使用的操作系统接口定义
*          应用程序只需包含此文件即可使用所有OS接口
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
 * 设备IO
 * ============================================================ */
int32  svcrt_dev_open(char *name, uint32 param);
int32  svcrt_dev_read(int32 handle, void *pdata, int32 len);
int32  svcrt_dev_write(int32 handle, void *pdata, int32 len);
int32  svcrt_dev_ctrl(int32 handle, uint32 code, uint32 value);

#endif
