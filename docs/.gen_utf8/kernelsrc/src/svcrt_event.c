/**
* @brief SVCrtOS 事件模块实现
* @details 实现事件的创建、等待和设置功能
* @author xw
* @date 2026.05.03
*/

#include "svcrt_event.h"

static svcrt_event_obj_t svcrt_events[SVCRT_EVENT_NUM];

void svcrt_event_module_init(void)
{
    int32 i, j;
    for(i = 0; i < SVCRT_EVENT_NUM; i++)
    {
        for(j = 0; j < 8; j++)
            svcrt_events[i].name[j] = 0;
        for(j = 0; j < SVCRT_MAX_EVENT_WAITERS; j++)
            svcrt_events[i].waiting_tasks[j] = 0;
    }
}

int32 svcrt_event_set_from_isr(int32 event_handle)
{
    /* 中断上下文安全：set 内部不阻塞 */
    svcrt_event_set_internal(event_handle);
    return 0;
}

int32 svcrt_event_create_internal(char *name)
{
    int32 i;
    for(i = 0; i < SVCRT_EVENT_NUM; i++)
    {
        if(svcrt_events[i].name[0] == 0)
        {
            int32 j;
            for(j = 0; j < 15; j++)
            {
                svcrt_events[i].name[j] = name[j];
                if(name[j] == 0)
                    break;
            }
            svcrt_events[i].name[15] = 0;
            return (i | SVCRT_EVENT_HANDLE_FLAG);
        }
    }
    return -1;
}

int32 svcrt_event_wait_internal(int32 event_handle, int32 timeout_ms)
{
    int32 idx = event_handle & SVCRT_HANDLE_RELMASK;
    int32 j;
    svcrt_task_t *p_tsk;

    if(idx >= SVCRT_EVENT_NUM)
        return -1;

    p_tsk = svcrt_task_get_current();
    if(p_tsk == 0)
        return -1;

    for(j = 0; j < SVCRT_MAX_EVENT_WAITERS; j++)
    {
        if(svcrt_events[idx].waiting_tasks[j] == 0)
        {
            svcrt_events[idx].waiting_tasks[j] = p_tsk;
            p_tsk->wake_reason = 0;
            break;
        }
    }

    if(timeout_ms > 0)
    {
        svcrt_task_wait_internal(timeout_ms);
    }
    else
    {
        svcrt_task_wait_period_internal();
    }
    return 0;
}

void svcrt_event_set_internal(int32 event_handle)
{
    int32 idx = event_handle & SVCRT_HANDLE_RELMASK;
    int32 j;

    if(idx >= SVCRT_EVENT_NUM)
        return;

    for(j = 0; j < SVCRT_MAX_EVENT_WAITERS; j++)
    {
        if(svcrt_events[idx].waiting_tasks[j] != 0)
        {
            svcrt_events[idx].waiting_tasks[j]->wake_reason = 0;
            svcrt_events[idx].waiting_tasks[j]->status = SVCRT_TASK_READY;
            svcrt_events[idx].waiting_tasks[j] = 0;
        }
    }
}
