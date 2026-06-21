/**
* @brief SVCrtOS ????????????
* @details ???????????????????
*          - ???????count>0 ? wait ???? 1?????????????post ?????????????
*          - ??????????? owner??????????????????????????????????????????§µ???
*            ?????????????????????????????
*          ??????????? svcrt_task_wait_internal??????????????????§Ý???
* @author xw
* @date 2026.05.30
*/

#include "svcrt_sync.h"
#include "svcrt_hal.h"

static svcrt_sem_obj_t svcrt_sems[SVCRT_SEM_NUM];
static svcrt_mtx_obj_t svcrt_mtxs[SVCRT_MTX_NUM];

void svcrt_sync_module_init(void)
{
    int32 i, j;
    for(i = 0; i < SVCRT_SEM_NUM; i++)
    {
        svcrt_sems[i].name[0] = 0;
        svcrt_sems[i].count   = 0;
        svcrt_sems[i].used    = 0;
        for(j = 0; j < SVCRT_MAX_SYNC_WAITERS; j++)
            svcrt_sems[i].waiters[j] = 0;
    }
    for(i = 0; i < SVCRT_MTX_NUM; i++)
    {
        svcrt_mtxs[i].name[0]       = 0;
        svcrt_mtxs[i].owner         = 0;
        svcrt_mtxs[i].used          = 0;
        svcrt_mtxs[i].orig_priority = 0;
        for(j = 0; j < SVCRT_MAX_SYNC_WAITERS; j++)
            svcrt_mtxs[i].waiters[j] = 0;
    }
}

static void svcrt_sync_copy_name(char *dst, char *src)
{
    int32 i;
    for(i = 0; i < 7; i++)
    {
        dst[i] = src[i];
        if(src[i] == 0)
            break;
    }
    dst[7] = 0;
}

static int32 svcrt_waiters_add(svcrt_task_t **waiters, svcrt_task_t *p_tsk)
{
    int32 j;
    for(j = 0; j < SVCRT_MAX_SYNC_WAITERS; j++)
    {
        if(waiters[j] == 0)
        {
            waiters[j] = p_tsk;
            return 0;
        }
    }
    return -1;
}

/* ???????§Ò????????????priority ???§³??????????? */
static svcrt_task_t *svcrt_waiters_pop_highest(svcrt_task_t **waiters)
{
    int32 j;
    int32 best = -1;
    uint8 best_pri = 255;
    for(j = 0; j < SVCRT_MAX_SYNC_WAITERS; j++)
    {
        if(waiters[j] != 0 && waiters[j]->priority <= best_pri)
        {
            best_pri = waiters[j]->priority;
            best = j;
        }
    }
    if(best < 0)
        return 0;
    {
        svcrt_task_t *p = waiters[best];
        waiters[best] = 0;
        return p;
    }
}

/* ============================================================
 * ?????
 * ============================================================ */
int32 svcrt_sem_create_internal(char *name, int32 init_count)
{
    int32 i;
    SVCRT_DISABLE_IRQ();
    for(i = 0; i < SVCRT_SEM_NUM; i++)
    {
        if(svcrt_sems[i].used == 0)
        {
            svcrt_sync_copy_name(svcrt_sems[i].name, name);
            svcrt_sems[i].count = init_count;
            svcrt_sems[i].used  = 1;
            SVCRT_ENABLE_IRQ();
            return (i | SVCRT_SEM_HANDLE_FLAG);
        }
    }
    SVCRT_ENABLE_IRQ();
    return -1;
}

int32 svcrt_sem_wait_internal(int32 handle, int32 timeout_ms)
{
    int32 idx = handle & SVCRT_HANDLE_RELMASK;
    svcrt_task_t *p_tsk;

    if(SVCRT_SEM_HANDLE_FLAG != (handle & SVCRT_HANDLE_MASK))
        return -1;
    if(idx >= SVCRT_SEM_NUM || svcrt_sems[idx].used == 0)
        return -1;

    SVCRT_DISABLE_IRQ();
    if(svcrt_sems[idx].count > 0)
    {
        svcrt_sems[idx].count--;
        SVCRT_ENABLE_IRQ();
        return 0;
    }

    p_tsk = svcrt_task_get_current();
    if(p_tsk == 0)
    {
        SVCRT_ENABLE_IRQ();
        return -1;
    }

    if(svcrt_waiters_add(svcrt_sems[idx].waiters, p_tsk) < 0)
    {
        SVCRT_ENABLE_IRQ();
        return -1;
    }
    SVCRT_ENABLE_IRQ();

    if(timeout_ms > 0)
        svcrt_task_wait_internal(timeout_ms);
    else
        svcrt_task_block_internal();

    return 0;
}

int32 svcrt_sem_post_internal(int32 handle)
{
    int32 idx = handle & SVCRT_HANDLE_RELMASK;
    svcrt_task_t *p_wake;

    if(SVCRT_SEM_HANDLE_FLAG != (handle & SVCRT_HANDLE_MASK))
        return -1;
    if(idx >= SVCRT_SEM_NUM || svcrt_sems[idx].used == 0)
        return -1;

    SVCRT_DISABLE_IRQ();
    p_wake = svcrt_waiters_pop_highest(svcrt_sems[idx].waiters);
    if(p_wake != 0)
    {
        p_wake->wait_time = 0;
        p_wake->status = SVCRT_TASK_READY;
    }
    else
    {
        svcrt_sems[idx].count++;
    }
    SVCRT_ENABLE_IRQ();
    SVCRT_SWITCH_TASK();
    return 0;
}

int32 svcrt_sem_delete_internal(int32 handle)
{
    int32 idx = handle & SVCRT_HANDLE_RELMASK;

    if(SVCRT_SEM_HANDLE_FLAG != (handle & SVCRT_HANDLE_MASK))
        return -1;
    if(idx >= SVCRT_SEM_NUM)
        return -1;

    SVCRT_DISABLE_IRQ();
    svcrt_sems[idx].used    = 0;
    svcrt_sems[idx].name[0] = 0;
    svcrt_sems[idx].count   = 0;
    {
        int32 j;
        for(j = 0; j < SVCRT_MAX_SYNC_WAITERS; j++)
            svcrt_sems[idx].waiters[j] = 0;
    }
    SVCRT_ENABLE_IRQ();
    return 0;
}

/* ============================================================
 * ??????
 * ============================================================ */
int32 svcrt_mtx_create_internal(char *name)
{
    int32 i;
    SVCRT_DISABLE_IRQ();
    for(i = 0; i < SVCRT_MTX_NUM; i++)
    {
        if(svcrt_mtxs[i].used == 0)
        {
            svcrt_sync_copy_name(svcrt_mtxs[i].name, name);
            svcrt_mtxs[i].owner = 0;
            svcrt_mtxs[i].used  = 1;
            SVCRT_ENABLE_IRQ();
            return (i | SVCRT_MTX_HANDLE_FLAG);
        }
    }
    SVCRT_ENABLE_IRQ();
    return -1;
}

int32 svcrt_mtx_lock_internal(int32 handle, int32 timeout_ms)
{
    int32 idx = handle & SVCRT_HANDLE_RELMASK;
    svcrt_task_t *p_tsk;

    if(SVCRT_MTX_HANDLE_FLAG != (handle & SVCRT_HANDLE_MASK))
        return -1;
    if(idx >= SVCRT_MTX_NUM || svcrt_mtxs[idx].used == 0)
        return -1;

    SVCRT_DISABLE_IRQ();
    p_tsk = svcrt_task_get_current();
    if(p_tsk == 0)
    {
        SVCRT_ENABLE_IRQ();
        return -1;
    }

    if(svcrt_mtxs[idx].owner == 0)
    {
        svcrt_mtxs[idx].owner         = p_tsk;
        svcrt_mtxs[idx].orig_priority = p_tsk->priority;
        SVCRT_ENABLE_IRQ();
        return 0;
    }

    if(svcrt_mtxs[idx].owner == p_tsk)
    {
        /* ????§µ??????????? */
        SVCRT_ENABLE_IRQ();
        return -1;
    }

    /* ???????§µ?????????????????????????????????????? */
    if(svcrt_mtxs[idx].owner->priority > p_tsk->priority)
    {
        svcrt_mtxs[idx].owner->priority = p_tsk->priority;
    }

    if(svcrt_waiters_add(svcrt_mtxs[idx].waiters, p_tsk) < 0)
    {
        SVCRT_ENABLE_IRQ();
        return -1;
    }
    SVCRT_ENABLE_IRQ();

    if(timeout_ms > 0)
        svcrt_task_wait_internal(timeout_ms);
    else
        svcrt_task_block_internal();

    return 0;
}

int32 svcrt_mtx_unlock_internal(int32 handle)
{
    int32 idx = handle & SVCRT_HANDLE_RELMASK;
    svcrt_task_t *p_tsk;
    svcrt_task_t *p_next;

    if(SVCRT_MTX_HANDLE_FLAG != (handle & SVCRT_HANDLE_MASK))
        return -1;
    if(idx >= SVCRT_MTX_NUM || svcrt_mtxs[idx].used == 0)
        return -1;

    SVCRT_DISABLE_IRQ();
    p_tsk = svcrt_task_get_current();
    if(p_tsk == 0 || svcrt_mtxs[idx].owner != p_tsk)
    {
        /* ??§Ô?????????? */
        SVCRT_ENABLE_IRQ();
        return -1;
    }

    /* ?????????????????????????????§µ? */
    p_tsk->priority = svcrt_mtxs[idx].orig_priority;

    p_next = svcrt_waiters_pop_highest(svcrt_mtxs[idx].waiters);
    if(p_next != 0)
    {
        svcrt_mtxs[idx].owner         = p_next;
        svcrt_mtxs[idx].orig_priority = p_next->priority;
        p_next->wait_time = 0;
        p_next->status = SVCRT_TASK_READY;
    }
    else
    {
        svcrt_mtxs[idx].owner = 0;
    }
    SVCRT_ENABLE_IRQ();
    SVCRT_SWITCH_TASK();
    return 0;
}

int32 svcrt_mtx_delete_internal(int32 handle)
{
    int32 idx = handle & SVCRT_HANDLE_RELMASK;

    if(SVCRT_MTX_HANDLE_FLAG != (handle & SVCRT_HANDLE_MASK))
        return -1;
    if(idx >= SVCRT_MTX_NUM)
        return -1;

    SVCRT_DISABLE_IRQ();
    svcrt_mtxs[idx].used    = 0;
    svcrt_mtxs[idx].name[0] = 0;
    svcrt_mtxs[idx].owner   = 0;
    {
        int32 j;
        for(j = 0; j < SVCRT_MAX_SYNC_WAITERS; j++)
            svcrt_mtxs[idx].waiters[j] = 0;
    }
    SVCRT_ENABLE_IRQ();
    return 0;
}
