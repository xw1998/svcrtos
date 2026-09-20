/**
* @brief SVCrtOS 软定时器模块实现
* @details 架构：
*          - 定时器对象为静态池（SVCRT_TIMER_NUM），对象创建/start/stop/delete 走 SVC
*          - svcrt_timer_tick_handler() 在 tick 中断里对激活定时器做倒计时，
*            到期时打 expired 标记并唤醒定时器服务任务
*          - 定时器服务任务（优先级 SVCRT_TIMER_TASK_PRI，可被业务任务抢占）
*            被唤醒后逐个执行到期回调；单次定时器回调后自动停止
*          - 用户回调永远运行在任务上下文，不占用中断时间、不破坏特权隔离
*/

#include "svcrt_timer.h"
#include "svcrt_hal.h"
#include "svcrt_cfg.h"

#if (SVCRT_USE_TIMER == 1)

static svcrt_timer_obj_t svcrt_timers[SVCRT_TIMER_NUM];

/* 定时器服务任务的栈与任务号（install 时分配） */
static uint32 svcrt_timer_task_stack[SVCRT_TIMER_TASK_STACK_WORDS];
static int32  svcrt_timer_task_id = -1;

void svcrt_timer_module_init(void)
{
    int32 i;
    for(i = 0; i < SVCRT_TIMER_NUM; i++)
    {
        svcrt_timers[i].name[0]  = 0;
        svcrt_timers[i].used     = 0;
        svcrt_timers[i].active   = 0;
        svcrt_timers[i].mode     = SVCRT_TIMER_MODE_PERIODIC;
        svcrt_timers[i].expired  = 0;
        svcrt_timers[i].period_ticks = 0;
        svcrt_timers[i].remain_ticks = 0;
        svcrt_timers[i].cb       = 0;
        svcrt_timers[i].arg      = 0;
    }
    svcrt_timer_task_id = -1;
}

/* 定时器服务任务主体：无事件时永久阻塞，被 tick 唤醒后执行到期回调 */
static void svcrt_timer_daemon(void)
{
    while(1)
    {
        int32 i;
        int32 has_work = 0;

        SVCRT_DISABLE_IRQ();
        for(i = 0; i < SVCRT_TIMER_NUM; i++)
        {
            if(svcrt_timers[i].used == 1 && svcrt_timers[i].expired == 1)
            {
                has_work = 1;
                break;
            }
        }
        SVCRT_ENABLE_IRQ();

        if(has_work == 0)
        {
            svcrt_task_block_internal();    /* 等待下一次到期唤醒 */
            continue;
        }

        /* 逐个执行到期回调（快照 cb/arg 后清除标记，允许回调内 stop/delete 自身） */
        for(i = 0; i < SVCRT_TIMER_NUM; i++)
        {
            void (*p_cb)(void *);
            void *p_arg;

            SVCRT_DISABLE_IRQ();
            if(svcrt_timers[i].used == 1 && svcrt_timers[i].expired == 1)
            {
                p_cb = svcrt_timers[i].cb;
                p_arg = svcrt_timers[i].arg;
                svcrt_timers[i].expired = 0;
                if(svcrt_timers[i].mode == SVCRT_TIMER_MODE_ONESHOT)
                {
                    svcrt_timers[i].active = 0;
                }
                SVCRT_ENABLE_IRQ();
            }
            else
            {
                SVCRT_ENABLE_IRQ();
                continue;
            }

            if(p_cb != 0)
            {
                p_cb(p_arg);
            }
        }
    }
}

/* 在内核初始化（cfg_load 之后、调度启动之前）调用，把服务任务注册进任务表 */
void svcrt_timer_task_install(void)
{
    svcrt_timer_task_id = svcrt_task_register(svcrt_timer_daemon,
                                              svcrt_timer_task_stack,
                                              sizeof(svcrt_timer_task_stack),
                                              (uint8)SVCRT_TIMER_TASK_PRI,
                                              0);
}

void svcrt_timer_tick_handler(void)
{
    int32 i;
    int32 need_wake = 0;

    for(i = 0; i < SVCRT_TIMER_NUM; i++)
    {
        if(svcrt_timers[i].used == 1 && svcrt_timers[i].active == 1)
        {
            if(svcrt_timers[i].remain_ticks > 0)
            {
                svcrt_timers[i].remain_ticks--;
            }
            if(svcrt_timers[i].remain_ticks == 0)
            {
                svcrt_timers[i].expired = 1;
                if(svcrt_timers[i].mode == SVCRT_TIMER_MODE_PERIODIC)
                {
                    svcrt_timers[i].remain_ticks = svcrt_timers[i].period_ticks;
                }
                need_wake = 1;
            }
        }
    }

    if(need_wake == 1 && svcrt_timer_task_id > 0)
    {
        svcrt_task_t *p_tsk = &svcrt_task_table[svcrt_timer_task_id - 1];
        if(p_tsk->status == SVCRT_TASK_WAIT)
        {
            p_tsk->wait_time = 0;
            p_tsk->wake_reason = 0;
            p_tsk->status = SVCRT_TASK_READY;
            svcrt_ready_add(svcrt_timer_task_id - 1);
        }
    }
}

static void svcrt_timer_copy_name(char *dst, char *src)
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

int32 svcrt_timer_create_internal(char *name)
{
    int32 i;
    SVCRT_DISABLE_IRQ();
    for(i = 0; i < SVCRT_TIMER_NUM; i++)
    {
        if(svcrt_timers[i].used == 0)
        {
            svcrt_timer_copy_name(svcrt_timers[i].name, name);
            svcrt_timers[i].used    = 1;
            svcrt_timers[i].active  = 0;
            svcrt_timers[i].expired = 0;
            svcrt_timers[i].cb      = 0;
            svcrt_timers[i].arg     = 0;
            SVCRT_ENABLE_IRQ();
            return (i | SVCRT_TIMER_HANDLE_FLAG);
        }
    }
    SVCRT_ENABLE_IRQ();
    return -1;
}

int32 svcrt_timer_start_internal(int32 handle, uint32 period_ms, uint8 mode,
                                 void (*cb)(void *), void *arg)
{
    int32 idx = handle & SVCRT_HANDLE_RELMASK;
    uint32 ticks;

    if(SVCRT_TIMER_HANDLE_FLAG != (handle & SVCRT_HANDLE_MASK))
        return -1;
    if(idx >= SVCRT_TIMER_NUM || svcrt_timers[idx].used == 0)
        return -1;
    if(cb == 0 || period_ms == 0)
        return -1;
    if(mode != SVCRT_TIMER_MODE_ONESHOT && mode != SVCRT_TIMER_MODE_PERIODIC)
        return -1;

    ticks = SVCRT_MS_TO_TICK(period_ms);
    if(ticks == 0)
        ticks = 1;

    SVCRT_DISABLE_IRQ();
    svcrt_timers[idx].mode         = mode;
    svcrt_timers[idx].period_ticks = ticks;
    svcrt_timers[idx].remain_ticks = ticks;
    svcrt_timers[idx].expired      = 0;
    svcrt_timers[idx].cb           = cb;
    svcrt_timers[idx].arg          = arg;
    svcrt_timers[idx].active       = 1;
    SVCRT_ENABLE_IRQ();
    return 0;
}

int32 svcrt_timer_stop_internal(int32 handle)
{
    int32 idx = handle & SVCRT_HANDLE_RELMASK;

    if(SVCRT_TIMER_HANDLE_FLAG != (handle & SVCRT_HANDLE_MASK))
        return -1;
    if(idx >= SVCRT_TIMER_NUM || svcrt_timers[idx].used == 0)
        return -1;

    SVCRT_DISABLE_IRQ();
    svcrt_timers[idx].active  = 0;
    svcrt_timers[idx].expired = 0;
    SVCRT_ENABLE_IRQ();
    return 0;
}

int32 svcrt_timer_delete_internal(int32 handle)
{
    int32 idx = handle & SVCRT_HANDLE_RELMASK;

    if(SVCRT_TIMER_HANDLE_FLAG != (handle & SVCRT_HANDLE_MASK))
        return -1;
    if(idx >= SVCRT_TIMER_NUM)
        return -1;

    SVCRT_DISABLE_IRQ();
    svcrt_timers[idx].used    = 0;
    svcrt_timers[idx].active  = 0;
    svcrt_timers[idx].expired = 0;
    svcrt_timers[idx].name[0] = 0;
    svcrt_timers[idx].cb      = 0;
    svcrt_timers[idx].arg     = 0;
    SVCRT_ENABLE_IRQ();
    return 0;
}

#endif /* SVCRT_USE_TIMER */
