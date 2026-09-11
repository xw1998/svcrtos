/**
* @brief SVCrtOS 事件模块（内核内部）
* @details 定义事件对象结构和内核内部事件操作函数
*          此文件仅供内核内部使用，应用程序应使用 svcrt.h
*/

#ifndef __SVCRT_EVENT_H__
#define __SVCRT_EVENT_H__

#include "svcrt_def.h"
#include "svcrt_task.h"
#include "svcrt_config.h"

typedef struct {
    char name[16];
    svcrt_task_t *waiting_tasks[SVCRT_MAX_EVENT_WAITERS];
} svcrt_event_obj_t;

void svcrt_event_module_init(void);
int32 svcrt_event_create_internal(char *name);
int32 svcrt_event_wait_internal(int32 handle, int32 timeout_ms);
void svcrt_event_set_internal(int32 handle);

#endif
