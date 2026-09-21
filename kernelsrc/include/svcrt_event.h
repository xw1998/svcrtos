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
    char   name[16];
    uint8  used;                                        /* 该事件对象是否已被占用 */
    uint8  flag;                                        /* 自动复位式置位标记：1=已置位 */
    int32  creator_id;                                  /* creator task id; 0 = kernel. Dropped together with its creator's image */
    svcrt_task_t *waiting_tasks[SVCRT_MAX_EVENT_WAITERS];
} svcrt_event_obj_t;

void svcrt_event_module_init(void);
int32 svcrt_event_create_internal(char *name);
int32 svcrt_event_wait_internal(int32 handle, int32 timeout_ms);
void svcrt_event_set_internal(int32 handle);

/* 任务下线收尸：把任务从所有事件的等待队列中摘除。
 * 调用方需自行保证临界区；一般经 svcrt_task_release_resources() 调用。 */
void svcrt_event_release_task(int32 task_id);

#endif
