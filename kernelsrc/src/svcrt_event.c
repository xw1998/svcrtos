/**
* @brief SVCrtOS 事件模块实现
* @details 实现事件的创建、等待和设置功能
* @author xw
* @date 2026.05.03
*/

#include "svcrt_event.h"
#include "svcrt_hal.h"
#include "svcrt_cfg.h"

static svcrt_event_obj_t svcrt_events[SVCRT_EVENT_NUM];

static int32 svcrt_event_waiter_add(svcrt_task_t **waiters, svcrt_task_t *p_tsk)
{
    int32 j;
    for(j = 0; j < SVCRT_MAX_EVENT_WAITERS; j++)
    {
        if(waiters[j] == 0)
        {
            waiters[j] = p_tsk;
            return 0;
        }
    }
    return -1;
}

static int32 svcrt_event_waiter_remove(svcrt_task_t **waiters, svcrt_task_t *p_tsk)
{
    int32 j;
    for(j = 0; j < SVCRT_MAX_EVENT_WAITERS; j++)
    {
        if(waiters[j] == p_tsk)
        {
            waiters[j] = 0;
            return 0;
        }
    }
    return -1;
}

void svcrt_event_module_init(void)
{
    int32 i, j;
    for(i = 0; i < SVCRT_EVENT_NUM; i++)
    {
        for(j = 0; j < 16; j++)
            svcrt_events[i].name[j] = 0;
        svcrt_events[i].used = 0;
        svcrt_events[i].flag = 0;
        for(j = 0; j < SVCRT_MAX_EVENT_WAITERS; j++)
            svcrt_events[i].waiting_tasks[j] = 0;
    }
}

/* 任务下线收尸：把任务从所有事件的等待队列摘除（调用方需已持临界区） */
void svcrt_event_release_task(int32 task_id)
{
    svcrt_task_t *p_tsk;
    int32 i;

    if(task_id <= 0 || task_id > svcrt_task_count)
    {
        return;
    }

    p_tsk = &svcrt_task_table[task_id - 1];

    for(i = 0; i < SVCRT_EVENT_NUM; i++)
    {
        if(svcrt_events[i].used == 0)
        {
            continue;
        }
        (void)svcrt_event_waiter_remove(svcrt_events[i].waiting_tasks, p_tsk);
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
    int32 idx = -1;

    if(name == 0)
    {
        return -1;
    }

    SVCRT_DISABLE_IRQ();
    for(i = 0; i < SVCRT_EVENT_NUM; i++)
    {
        if(svcrt_events[i].used == 0)
        {
            int32 j;
            for(j = 0; j < 15; j++)
            {
                svcrt_events[i].name[j] = name[j];
                if(name[j] == 0)
                    break;
            }
            svcrt_events[i].name[15]  = 0;
            svcrt_events[i].used      = 1;
            svcrt_events[i].flag      = 0;
            for(j = 0; j < SVCRT_MAX_EVENT_WAITERS; j++)
            {
                svcrt_events[i].waiting_tasks[j] = 0;
            }
            idx = i;
            break;
        }
    }
    SVCRT_ENABLE_IRQ();

    if(idx < 0)
    {
        return -1;
    }
    return (idx | SVCRT_EVENT_HANDLE_FLAG);
}

int32 svcrt_event_wait_internal(int32 event_handle, int32 timeout_ms)
{
    int32 idx = event_handle & SVCRT_HANDLE_RELMASK;
    svcrt_task_t *p_tsk;
    int32 reason;

    if(SVCRT_EVENT_HANDLE_FLAG != (event_handle & SVCRT_HANDLE_MASK))
        return -1;
    if(idx >= SVCRT_EVENT_NUM || svcrt_events[idx].used == 0)
        return -1;

    SVCRT_DISABLE_IRQ();

    /* 已经置位过：直接消费掉，不阻塞（否则“先 set 后 wait”会丢掉这次置位） */
    if(svcrt_events[idx].flag != 0u)
    {
        svcrt_events[idx].flag = 0u;
        SVCRT_ENABLE_IRQ();
        return 0;
    }

    p_tsk = svcrt_task_get_current();
    if(p_tsk == 0)
    {
        SVCRT_ENABLE_IRQ();
        return -1;
    }

    /* 等待队列满时必须报错返回：原先只跳过登记仍然睡下去，
     * set 时遍历不到它，该任务会永久阻塞。 */
    if(svcrt_event_waiter_add(svcrt_events[idx].waiting_tasks, p_tsk) < 0)
    {
        SVCRT_ENABLE_IRQ();
        return -1;
    }

    /* 登记与置 WAIT 同处一个临界区（关中断进入、关中断返回），消除丢唤醒窗口 */
    reason = svcrt_task_block_in_critical((uint32)timeout_ms);

    if(reason < 0)
    {
        (void)svcrt_event_waiter_remove(svcrt_events[idx].waiting_tasks, p_tsk);
        SVCRT_ENABLE_IRQ();
        return -1;
    }

    if(reason == SVCRT_WAKE_OBJ_DELETED)
    {
        SVCRT_ENABLE_IRQ();
        return -1;
    }

    if(reason == 1)
    {
        (void)svcrt_event_waiter_remove(svcrt_events[idx].waiting_tasks, p_tsk);
        SVCRT_ENABLE_IRQ();
        return SVCRT_SYNC_ERR_TIMEOUT;
    }

    SVCRT_ENABLE_IRQ();
    return 0;
}

void svcrt_event_set_internal(int32 event_handle)
{
    int32 idx = event_handle & SVCRT_HANDLE_RELMASK;
    int32 j;

    if(SVCRT_EVENT_HANDLE_FLAG != (event_handle & SVCRT_HANDLE_MASK))
        return;
    if(idx >= SVCRT_EVENT_NUM || svcrt_events[idx].used == 0)
        return;

    SVCRT_DISABLE_IRQ();

    /* 记录置位：没有等待者时留给下一次 wait 消费，避免“先 set 后 wait”丢事件 */
    svcrt_events[idx].flag = 1u;

    for(j = 0; j < SVCRT_MAX_EVENT_WAITERS; j++)
    {
        svcrt_task_t *p_w = svcrt_events[idx].waiting_tasks[j];

        if(p_w != 0)
        {
            p_w->wait_time   = 0;
            p_w->wake_reason = SVCRT_WAKE_NORMAL;
            p_w->status      = SVCRT_TASK_READY;
            svcrt_events[idx].waiting_tasks[j] = 0;
        }
    }

    SVCRT_ENABLE_IRQ();

    /* 原先只置 READY 不触发切换，被唤醒的任务要等到下一次 tick 才运行 */
    SVCRT_SWITCH_TASK();
}
