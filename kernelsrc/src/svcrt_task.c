/**
* @brief SVCrtOS ??????????????
* @details ?????????????????????????SVC??????????????
* @author xw
* @date 2026.05.03
*/

#include "svcrt_cfg.h"
#include "svcrt_task.h"
#include "svcrt_event.h"
#include "svcrt_mpu.h"
#include "svcrt_dev.h"
#include "svcrt_def.h"

uint32 svcrt_kernel_tick = 0;
int32  svcrt_current_task_id = 0;
#if (SVCRT_USE_CPU_LOAD == 1)
uint16 svcrt_cpu_load_counter = 0;
uint16 svcrt_cpu_idle_millis = 0;
#endif

static uint32 svcrt_idle_stack_ptr = 0;

static void svcrt_tick_tasks(svcrt_task_t *p_task);

void svcrt_kernel_tick_handler(void)
{
    svcrt_kernel_tick++;
    SVCRT_SWITCH_TASK();

    #if (SVCRT_USE_CPU_LOAD == 1)
    if(svcrt_current_task_id > 0)
    {
        svcrt_cpu_load_counter++;
        if((svcrt_kernel_tick & 0x3ff) == 0)
        {
            svcrt_cpu_idle_millis = svcrt_cpu_load_counter;
            svcrt_cpu_load_counter = 0;
        }
    }
    #endif
}

void SVC_Server(svcrt_svc_context_t *p_svc_ctx)
{
    uint32 svc_num = ((char *)p_svc_ctx->pc)[-2];
    uint32 *p;

    switch(svc_num)
    {
    case SVCRT_SVC_EVENT_CTRL:
        p = (uint32 *)p_svc_ctx->r0;
        switch(p[0])
        {
            case 1:
                p_svc_ctx->r0 = svcrt_event_create_internal((char *)p[1]);
                break;
            case 2:
                svcrt_event_wait_internal(p[1], p[2]);
                break;
            case 3:
                svcrt_event_set_internal(p[1]);
                break;
            default:
                break;
        }
        break;

    case SVCRT_SVC_SYS_INFO:
        switch(p_svc_ctx->r0)
        {
            case 1:
                p_svc_ctx->r0 = svcrt_kernel_get_time();
                break;
            #if (SVCRT_USE_CPU_LOAD == 1)
            case 2:
                p_svc_ctx->r0 = svcrt_kernel_get_cpu_idle();
                break;
            #endif
            default:
                break;
        }
        break;

    case SVCRT_SVC_TASK_CTRL:
        switch(p_svc_ctx->r0)
        {
            case 1:
                svcrt_task_wait_internal(p_svc_ctx->r1);
                break;
            case 2:
                svcrt_task_wait_period_internal();
                break;
            case 3:
                svcrt_task_delay_internal(p_svc_ctx->r1);
                break;
            case 4:
                svcrt_task_kill_internal();
                break;
            default:
                break;
        }
        break;

    case SVCRT_SVC_DEV_IO:
        p = (uint32 *)p_svc_ctx->r0;
        switch(p[0])
        {
            case 1:
                p_svc_ctx->r0 = svcrt_dev_open_internal((char *)p[1], p[2]);
                break;
            case 2:
                p_svc_ctx->r0 = svcrt_dev_read_internal(p[1], (uint8 *)p[2], p[3]);
                break;
            case 3:
                p_svc_ctx->r0 = svcrt_dev_write_internal(p[1], (uint8 *)p[2], p[3]);
                break;
            case 4:
                p_svc_ctx->r0 = svcrt_dev_ctrl_internal(p[1], p[2], p[3]);
                break;
            case 5:
                p_svc_ctx->r0 = svcrt_dev_close_internal(p[1]);
                break;
            default:
                p_svc_ctx->r0 = 0;
                break;
        }
        break;

    default:
        break;
    }
}

void svcrt_hardfault_handler(void)
{
    if(svcrt_current_task_id > 0)
    {
        svcrt_task_table[svcrt_current_task_id - 1].status = SVCRT_TASK_INVALID;
        SVCRT_SWITCH_TASK();
    }
    else
    {
        while(1)
        {
        }
    }
}

int32 svcrt_sched_is_switching(void)
{
    int32 new_idx = svcrt_sched_next() + 1;
    if(new_idx == svcrt_current_task_id)
    {
        if(new_idx > 0)
        {
            svcrt_task_table[new_idx - 1].touch_tick = svcrt_kernel_tick;
        }
        return -1;
    }
    else
    {
        return new_idx;
    }
}

int32 svcrt_sched_activate(int32 new_task, uint32 old_psp)
{
    int32 tid = svcrt_current_task_id - 1;

    if(svcrt_current_task_id == 0)
    {
        svcrt_idle_stack_ptr = old_psp;
    }
    else
    {
        svcrt_task_table[tid].stack_ptr = old_psp;

        #if (SVCRT_USE_STACK_CHECK == 1)
        if(*svcrt_task_table[tid].stack_bottom != SVCRT_STACK_END_FLAG_VAL)
        {
            svcrt_task_table[tid].status = SVCRT_TASK_INVALID;
        }
        else
        #endif
        {
            if(svcrt_task_table[tid].status == SVCRT_TASK_RUNNING)
            {
                svcrt_task_table[tid].status = SVCRT_TASK_READY;
            }
        }
    }

    svcrt_current_task_id = new_task;
    if(svcrt_current_task_id > 0)
    {
        tid = svcrt_current_task_id - 1;
        svcrt_task_table[tid].touch_tick = svcrt_kernel_tick;
        svcrt_task_table[tid].status = SVCRT_TASK_RUNNING;
        svcrt_mpu_set_app(&svcrt_task_table[tid]);
        return svcrt_task_table[tid].stack_ptr;
    }
    else
    {
        svcrt_current_task_id = 0;
        return svcrt_idle_stack_ptr;
    }
}

int32 svcrt_sched_next(void)
{
    uint8 tmp_pri = 255;
    uint32 tmp_touch = 0xffffffff;
    uint8 idx;
    int32 r = -1;

    for(idx = 0; idx < svcrt_task_count; idx++)
    {
        svcrt_tick_tasks(&svcrt_task_table[idx]);

        if((svcrt_task_table[idx].status == SVCRT_TASK_READY) ||
            (svcrt_task_table[idx].status == SVCRT_TASK_RUNNING))
        {
            if(svcrt_task_table[idx].priority < tmp_pri)
            {
                tmp_pri = svcrt_task_table[idx].priority;
                tmp_touch = svcrt_task_table[idx].touch_tick;
                r = idx;
            }
            else if(svcrt_task_table[idx].priority == tmp_pri)
            {
                if(svcrt_task_table[idx].touch_tick < tmp_touch)
                {
                    tmp_touch = svcrt_task_table[idx].touch_tick;
                    r = idx;
                }
            }
        }
    }
    return r;
}

static void svcrt_tick_tasks(svcrt_task_t *p_task)
{
    uint32 used_tick = svcrt_kernel_tick;
    int32 escape_tick = used_tick - p_task->tim_tick;

    if((p_task->status == SVCRT_TASK_RUNNING) ||
        (p_task->status == SVCRT_TASK_INVALID))
    {
        return;
    }

    p_task->tim_tick = used_tick;
    p_task->period_time -= escape_tick;
    if(p_task->period_time < 0)
    {
        p_task->period_time += p_task->period;
        if(p_task->status == SVCRT_TASK_WAIT)
        {
            p_task->status = SVCRT_TASK_READY;
        }
    }

    if(p_task->wait_time > 0)
    {
        p_task->wait_time -= escape_tick;
        if(p_task->wait_time <= 0)
        {
            if(p_task->status == SVCRT_TASK_WAIT)
            {
                p_task->status = SVCRT_TASK_READY;
            }
        }
        else
        {
            if(p_task->status != SVCRT_TASK_INVALID)
            {
                p_task->status = SVCRT_TASK_WAIT;
            }
        }
    }
}

svcrt_task_t *svcrt_task_get_current(void)
{
    svcrt_task_t *p_tsk = 0;
    SVCRT_DISABLE_IRQ();
    if(svcrt_current_task_id > 0)
    {
        p_tsk = &svcrt_task_table[svcrt_current_task_id - 1];
    }
    SVCRT_ENABLE_IRQ();
    return p_tsk;
}

void svcrt_task_wait_internal(uint32 ms)
{
    svcrt_task_t *p_tsk;
    SVCRT_DISABLE_IRQ();
    if(svcrt_current_task_id > 0)
    {
        p_tsk = &svcrt_task_table[svcrt_current_task_id - 1];
        p_tsk->wait_time = SVCRT_MS_TO_TICK(ms);
        p_tsk->status = SVCRT_TASK_WAIT;
        SVCRT_SWITCH_TASK();
    }
    SVCRT_ENABLE_IRQ();
}

void svcrt_task_wait_period_internal(void)
{
    svcrt_task_t *p_tsk;
    SVCRT_DISABLE_IRQ();
    if(svcrt_current_task_id > 0)
    {
        p_tsk = &svcrt_task_table[svcrt_current_task_id - 1];
        p_tsk->status = SVCRT_TASK_WAIT;
        SVCRT_SWITCH_TASK();
    }
    SVCRT_ENABLE_IRQ();
}

void svcrt_task_delay_internal(uint32 us)
{
    int32 tm_start = svcrt_port_get_systick_val();
    int32 wait_clk = us * (SystemCoreClock / 1000000);
    int32 tm_end;
    int32 tm_diff;

    while(wait_clk > 0)
    {
        tm_end = svcrt_port_get_systick_val();
        tm_diff = tm_start - tm_end;
        if(tm_diff < 0)
            tm_diff += svcrt_port_get_systick_load();
        tm_start = tm_end;

        wait_clk -= tm_diff;
    }
}

void svcrt_sched_switch(void)
{
    SVCRT_SWITCH_TASK();
    SVCRT_WFE();
}

void svcrt_task_kill_internal(void)
{
    if(svcrt_current_task_id > 0)
    {
        svcrt_task_table[svcrt_current_task_id - 1].status = SVCRT_TASK_INVALID;
    }

    {
        SVCRT_SWITCH_TASK();
        SVCRT_WFE();
    }
}

uint32 svcrt_kernel_get_time(void)
{
    return svcrt_kernel_tick / (1000 / SVCRT_TICK_PERIOD_US);
}

uint32 svcrt_kernel_get_tick(void)
{
    return svcrt_kernel_tick;
}

uint16 svcrt_kernel_get_cpu_idle(void)
{
    #if (SVCRT_USE_CPU_LOAD == 1)
    return svcrt_cpu_idle_millis;
    #else
    return 0;
    #endif
}

void svcrt_sched_activate_higher(uint8 ck_pri)
{
    if(svcrt_current_task_id > 0)
    {
        if(svcrt_task_table[svcrt_current_task_id - 1].priority > ck_pri)
        {
            SVCRT_SWITCH_TASK();
        }
    }
    else
    {
        SVCRT_SWITCH_TASK();
    }
}
