/**
* @brief SVCrtOS ????API????
* @details ????®Æ???(????)???????????????
*          ??®Æ?????????????????????????OS???
* @author xw
* @date 2026.05.03
*/

#ifndef __SVCRT_H__
#define __SVCRT_H__

#include "svcrt_types.h"

/* ============================================================
 * ???????
 * ============================================================ */
void   svcrt_task_wait(uint32 ms);
void   svcrt_task_wait_period(void);
void   svcrt_task_delay(uint32 us);
void   svcrt_task_kill(void);

/* ============================================================
 * ?????
 * ============================================================ */
uint32 svcrt_get_time_ms(void);
uint32 svcrt_get_cpu_usage(void);

/* ============================================================
 * ???
 * ============================================================ */
int32  svcrt_event_create(char *name);
void   svcrt_event_wait(int32 handle, int32 timeout);
void   svcrt_event_set(int32 handle);

/* ============================================================
 * ?????
 * ============================================================ */
int32  svcrt_sem_create(char *name, int32 init_count);
int32  svcrt_sem_wait(int32 handle, int32 timeout);
int32  svcrt_sem_post(int32 handle);
int32  svcrt_sem_delete(int32 handle);

/* ============================================================
 * ??????
 * ============================================================ */
int32  svcrt_mutex_create(char *name);
int32  svcrt_mutex_lock(int32 handle, int32 timeout);
int32  svcrt_mutex_unlock(int32 handle);
int32  svcrt_mutex_delete(int32 handle);

/* ============================================================
 * ?ıÙIO
 * ============================================================ */
int32  svcrt_dev_open(char *name, uint32 param);
int32  svcrt_dev_close(int32 handle);
int32  svcrt_dev_read(int32 handle, void *pdata, int32 len);
int32  svcrt_dev_write(int32 handle, void *pdata, int32 len);
int32  svcrt_dev_ctrl(int32 handle, uint32 code, uint32 value);

#endif
