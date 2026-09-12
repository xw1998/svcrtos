/**
* @brief SVCrtOS 任务调度与系统调用分发实现
* @details 实现任务调度器、上下文切换的 C 侧逻辑、SVC 系统调用分发，以及任务控制接口。
*          中断入口（SysTick_Handler/HardFault_Handler）在 board/ 中实现，
*          它们最终调用本文件的 svcrt_kernel_tick_handler / svcrt_hardfault_handler。
*          微秒级延时由 port 层 svcrt_port_delay_us() 实现，依赖系统主频 SystemCoreClock。
* @author xw
* @date 2026.05.03
*/

#include "svcrt_cfg.h"
#include "svcrt_task.h"
#include "svcrt_event.h"
#include "svcrt_hal.h"
#include "svcrt_dev.h"
#include "svcrt_sync.h"
#include "svcrt_def.h"
#include "svcrt_config.h"
#include "svcrt_mq.h"
#include "svcrt_timer.h"
#include "svcrt_fault.h"
#include "svcrt_ptable.h"
#include "svcrt_loader.h"


volatile uint32 svcrt_interrupt_nest = 0;
uint32 svcrt_kernel_tick = 0;
int32  svcrt_current_task_id = 0;
#if (SVCRT_USE_SCHED_LOCK == 1)
/* 调度器锁嵌套计数：>0 表示当前禁止任务切换（svcrt_sched_lock/unlock 维护） */
volatile uint32 svcrt_sched_lock_nest = 0;
#endif

#if (SVCRT_USE_CPU_LOAD == 1)
uint16 svcrt_cpu_load_counter = 0;
uint16 svcrt_cpu_idle_millis = 0;
#endif

static uint32 svcrt_idle_stack_ptr = 0;

static void svcrt_tick_tasks(svcrt_task_t *p_task);
static void svcrt_task_recover_pending(void);

void svcrt_kernel_tick_handler(void)
{
    svcrt_kernel_tick++;
    #if (SVCRT_USE_TIMER == 1)
    /* 软定时器倒计时与到期唤醒（不在中断里执行用户回调） */
    svcrt_timer_tick_handler();
    #endif
    #if (SVCRT_USE_SCHED_LOCK == 1)
    if(svcrt_sched_lock_nest == 0u)         /* 调度器锁定期间不触发任务切换 */
    #endif
    {
        SVCRT_SWITCH_TASK();
    }

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

void SVC_Server(void *p_svc_ctx)
{
    uint32 svc_num = SVCRT_SVC_NUM(p_svc_ctx);
    uint32 *p;

    switch(svc_num)
    {
    case SVCRT_SVC_EVENT_CTRL:
        p = (uint32 *)SVCRT_SVC_ARG(p_svc_ctx, 0);
        switch(p[0])
        {
            case 1:
                SVCRT_SVC_RET(p_svc_ctx, svcrt_event_create_internal((char *)p[1]));
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
        switch(SVCRT_SVC_ARG(p_svc_ctx, 0))
        {
            case 1:
                SVCRT_SVC_RET(p_svc_ctx, svcrt_kernel_get_time());
                break;
            #if (SVCRT_USE_CPU_LOAD == 1)
            case 2:
                SVCRT_SVC_RET(p_svc_ctx, svcrt_kernel_get_cpu_idle());
                break;
            #endif
            case 3:
                /* 故障记录条数 */
                SVCRT_SVC_RET(p_svc_ctx, (uint32)svcrt_fault_record_count_internal());
                break;
            case 4:
                /* 读取第 r1 条故障记录到 r2 指向的用户缓冲（3 个字） */
                {
                    const svcrt_fault_record_t *p_rec = svcrt_fault_record_get((int32)SVCRT_SVC_ARG(p_svc_ctx, 1));
                    if(p_rec != 0 && SVCRT_SVC_ARG(p_svc_ctx, 2) != 0)
                    {
                        ((uint32 *)SVCRT_SVC_ARG(p_svc_ctx, 2))[0] = p_rec->type;
                        ((uint32 *)SVCRT_SVC_ARG(p_svc_ctx, 2))[1] = (uint32)p_rec->task_id;
                        ((uint32 *)SVCRT_SVC_ARG(p_svc_ctx, 2))[2] = p_rec->tick;
                        SVCRT_SVC_RET(p_svc_ctx, 0);
                    }
                    else
                    {
                        SVCRT_SVC_RET(p_svc_ctx, (uint32)(-1));
                    }
                }
                break;
            default:
                break;
        }
        break;

    case SVCRT_SVC_TASK_CTRL:
        switch(SVCRT_SVC_ARG(p_svc_ctx, 0))
        {
            case 1:
                svcrt_task_wait_internal(SVCRT_SVC_ARG(p_svc_ctx, 1));
                break;
            case 2:
                svcrt_task_wait_period_internal();
                break;
            case 3:
                svcrt_task_delay_internal(SVCRT_SVC_ARG(p_svc_ctx, 1));
                break;
            case 4:
                svcrt_task_kill_internal();
                break;
            case 5:
                SVCRT_SVC_RET(p_svc_ctx, (uint32)svcrt_task_status_get_internal((int32)SVCRT_SVC_ARG(p_svc_ctx, 1)));
                break;
            case 6:
                SVCRT_SVC_RET(p_svc_ctx, (uint32)svcrt_task_recover((int32)SVCRT_SVC_ARG(p_svc_ctx, 1)));
                break;
            #if (SVCRT_USE_SCHED_LOCK == 1)
            case 7:
                svcrt_sched_lock_internal();
                break;
            case 8:
                SVCRT_SVC_RET(p_svc_ctx, svcrt_sched_unlock_internal());
                break;
            case 9:
                SVCRT_SVC_RET(p_svc_ctx, (uint32)svcrt_sched_lock_count_internal());
                break;
            #endif
            #if (SVCRT_USE_STACK_USAGE == 1)
            case 10:
                /* r1=任务号，r2=用户缓冲区（3 个字：总字节/峰值已用/剩余） */
                SVCRT_SVC_RET(p_svc_ctx, (uint32)svcrt_task_stack_info_internal(
                                  (int32)SVCRT_SVC_ARG(p_svc_ctx, 1),
                                  (uint32 *)SVCRT_SVC_ARG(p_svc_ctx, 2)));
                break;
            #endif
            default:
                break;
        }
        break;

    case SVCRT_SVC_DEV_IO:
        p = (uint32 *)SVCRT_SVC_ARG(p_svc_ctx, 0);
        switch(p[0])
        {
            case 1:
                SVCRT_SVC_RET(p_svc_ctx, svcrt_dev_open_internal((char *)p[1], p[2]));
                break;
            case 2:
                SVCRT_SVC_RET(p_svc_ctx, svcrt_dev_read_internal(p[1], (uint8 *)p[2], p[3]));
                break;
            case 3:
                SVCRT_SVC_RET(p_svc_ctx, svcrt_dev_write_internal(p[1], (uint8 *)p[2], p[3]));
                break;
            case 4:
                SVCRT_SVC_RET(p_svc_ctx, svcrt_dev_ctrl_internal(p[1], p[2], p[3]));
                break;
            case 5:
                SVCRT_SVC_RET(p_svc_ctx, svcrt_dev_close_internal(p[1]));
                break;
            default:
                SVCRT_SVC_RET(p_svc_ctx, 0);
                break;
        }
        break;

    case SVCRT_SVC_DRV_MGR:
        p = (uint32 *)SVCRT_SVC_ARG(p_svc_ctx, 0);
        switch(p[0])
        {
            case 1:
                SVCRT_SVC_RET(p_svc_ctx, svcrt_dev_register((char *)p[1], (svcrt_dev_drv_t *)p[2], p[3]));
                break;
            case 2:
                SVCRT_SVC_RET(p_svc_ctx, svcrt_dev_unregister((char *)p[1]));
                break;
            case 3:
                SVCRT_SVC_RET(p_svc_ctx, svcrt_dev_get_count());
                break;
            default:
                SVCRT_SVC_RET(p_svc_ctx, (uint32)(-1));
                break;
        }
        break;

    case SVCRT_SVC_SYNC_CTRL:
        p = (uint32 *)SVCRT_SVC_ARG(p_svc_ctx, 0);
        switch(p[0])
        {
            case 1:
                SVCRT_SVC_RET(p_svc_ctx, svcrt_sem_create_internal((char *)p[1], (int32)p[2]));
                break;
            case 2:
                SVCRT_SVC_RET(p_svc_ctx, svcrt_sem_wait_internal((int32)p[1], (int32)p[2]));
                break;
            case 3:
                SVCRT_SVC_RET(p_svc_ctx, svcrt_sem_post_internal((int32)p[1]));
                break;
            case 4:
                SVCRT_SVC_RET(p_svc_ctx, svcrt_sem_delete_internal((int32)p[1]));
                break;
            case 5:
                SVCRT_SVC_RET(p_svc_ctx, svcrt_mtx_create_internal((char *)p[1]));
                break;
            case 6:
                SVCRT_SVC_RET(p_svc_ctx, svcrt_mtx_lock_internal((int32)p[1], (int32)p[2]));
                break;
            case 7:
                SVCRT_SVC_RET(p_svc_ctx, svcrt_mtx_unlock_internal((int32)p[1]));
                break;
            case 8:
                SVCRT_SVC_RET(p_svc_ctx, svcrt_mtx_delete_internal((int32)p[1]));
                break;
            default:
                SVCRT_SVC_RET(p_svc_ctx, (uint32)(-1));
                break;
        }
        break;

    case SVCRT_SVC_MQ_CTRL:
        p = (uint32 *)SVCRT_SVC_ARG(p_svc_ctx, 0);
        switch(p[0])
        {
        case 1:
            SVCRT_SVC_RET(p_svc_ctx, svcrt_mq_create_internal((char *)p[1]));
            break;
        case 2:
            SVCRT_SVC_RET(p_svc_ctx, svcrt_mq_send_internal((int32)p[1], (uint32 *)p[2], (int32)p[3], (int32)p[4]));
            break;
        case 3:
            SVCRT_SVC_RET(p_svc_ctx, svcrt_mq_recv_internal((int32)p[1], (uint32 *)p[2], (int32)p[3], (int32)p[4]));
            break;
        case 4:
            SVCRT_SVC_RET(p_svc_ctx, svcrt_mq_delete_internal((int32)p[1]));
            break;
        default:
            SVCRT_SVC_RET(p_svc_ctx, (uint32)(-1));
            break;
        }
        break;

    case SVCRT_SVC_TIMER_CTRL:
        p = (uint32 *)SVCRT_SVC_ARG(p_svc_ctx, 0);
        switch(p[0])
        {
        case 1:
            SVCRT_SVC_RET(p_svc_ctx, svcrt_timer_create_internal((char *)p[1]));
            break;
        case 2:
            SVCRT_SVC_RET(p_svc_ctx, svcrt_timer_start_internal((int32)p[1], p[2], (uint8)p[3],
                                                       (void (*)(void *))p[4], (void *)p[5]));
            break;
        case 3:
            SVCRT_SVC_RET(p_svc_ctx, svcrt_timer_stop_internal((int32)p[1]));
            break;
        case 4:
            SVCRT_SVC_RET(p_svc_ctx, svcrt_timer_delete_internal((int32)p[1]));
            break;
        default:
            SVCRT_SVC_RET(p_svc_ctx, (uint32)(-1));
            break;
        }
        break;

    case SVCRT_SVC_APP_MGR:
        p = (uint32 *)SVCRT_SVC_ARG(p_svc_ctx, 0);
        switch(p[0])
        {
        case 2:     /* 从设备流式加载 App 镜像到空闲槽位 */
            SVCRT_SVC_RET(p_svc_ctx, (uint32)svcrt_loader_load_dev((int32)p[1], p[2]));
            break;
        case 3:     /* 启动槽位中的 App */
            SVCRT_SVC_RET(p_svc_ctx, (uint32)svcrt_loader_start(p[1]));
            break;
        case 4:     /* 停止槽位中的 App */
            SVCRT_SVC_RET(p_svc_ctx, (uint32)svcrt_loader_stop(p[1]));
            break;
        case 5:     /* 查询槽位状态 */
            SVCRT_SVC_RET(p_svc_ctx, svcrt_loader_state(p[1]));
            break;
        case 6:     /* 从设备安装驱动镜像到驱动区 */
            SVCRT_SVC_RET(p_svc_ctx, (uint32)svcrt_loader_load_driver((int32)p[1], p[2]));
            break;
        default:
            SVCRT_SVC_RET(p_svc_ctx, (uint32)(-1));
            break;
        }
        break;

    default:
        break;
    }
}

/* ============================================================
 * CPU 异常统一处理入口
 * @brief HardFault / MemManage / BusFault / UsageFault 共用
 * @details 任务上下文中发生异常时做任务级恢复（重建栈帧重启该任务），
 *          连续故障达到 APP_CRASH_RESTART_MAX 的应用由 svcrt_loader_on_fault()
 *          禁用，不再无限重启；内核/中断上下文异常则记录后停机等调试器接管。
 *          注意：CFSR/HFSR 保持不清，便于用调试器定位故障原因。
 * ============================================================ */
void svcrt_cpu_fault_handler(uint32 fault_type)
{
    if(svcrt_current_task_id > 0)
    {
        svcrt_fault_record(fault_type, svcrt_current_task_id);
        #if (SVCRT_USE_FAULT_RECOVER == 1)
        /* 任务级故障恢复：重建栈帧并重新调度，等效于任务复位重启。
         * 但连续故障达到 APP_CRASH_RESTART_MAX 的 App 会被禁用（不再重启），
         * 避免一个坏应用把整机拖进“崩溃-重启”死循环。 */
        if(svcrt_loader_on_fault(svcrt_current_task_id) != 0)
        {
            SVCRT_SWITCH_TASK();
        }
        else
        {
            svcrt_task_recover(svcrt_current_task_id);
        }
        #else
        svcrt_task_table[svcrt_current_task_id - 1].status = SVCRT_TASK_INVALID;
        SVCRT_SWITCH_TASK();
        #endif
    }
    else
    {
        /* 内核上下文异常：记录后停机，等待调试器接管 */
        svcrt_fault_record(fault_type, 0);
        while(1)
        {
        }
    }
}

void svcrt_hardfault_handler(void)
{
    svcrt_cpu_fault_handler(SVCRT_FAULT_HARDFAULT);
}

int32 svcrt_sched_is_switching(void)
{
    int32 new_idx = svcrt_sched_next() + 1;

    #if (SVCRT_USE_SCHED_LOCK == 1)
    /* 调度器锁定期间不切换任务；超时处理已在 svcrt_sched_next 内完成 */
    if(svcrt_sched_lock_nest != 0u)
    {
        return -1;
    }
    #endif

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

        #if (SVCRT_USE_STACK_USAGE == 1)
        /* 记录历史最低栈指针（栈向低地址增长），用于峰值栈用量统计 */
        if(old_psp < svcrt_task_table[tid].stack_peak_low)
        {
            svcrt_task_table[tid].stack_peak_low = old_psp;
        }
        #endif


        if(svcrt_task_table[tid].status == SVCRT_TASK_RUNNING)
        {
            #if (SVCRT_USE_STACK_CHECK == 1)
            /* 栈溢出检测：保存的 PSP 若已低于栈底地址，说明栈越界了。
             * 仅在 RUNNING 状态下检测，避免对 WAIT 任务误判（详见移植手册已知问题）。 */
            if(old_psp < (uint32)svcrt_task_table[tid].stack_bottom)
            {
                svcrt_task_table[tid].status = SVCRT_TASK_INVALID;
            }
            else
            #endif
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
        svcrt_port_mpu_set_app(&svcrt_task_table[tid].mpu);
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

    /* execute pending task recovery (rebuild stack frame) */
    svcrt_task_recover_pending();

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
    int32 escape_tick = (int32)(used_tick - p_task->tim_tick);

    if(p_task->status == SVCRT_TASK_INVALID)
    {
        return;
    }

    p_task->tim_tick = used_tick;

    if(escape_tick <= 0)
    {
        return;
    }

    if(p_task->status == SVCRT_TASK_RUNNING)
    {
        return;
    }

    /* wait_time < 0：无限阻塞（信号量/互斥锁用），只能被 post/unlock 显式唤醒，
     * tick 不递减、也不走 period_time 周期逻辑。 */
    if(p_task->wait_time < 0)
    {
        return;
    }

    if(p_task->wait_time > 0)
    {
        p_task->wait_time -= escape_tick;
        if(p_task->wait_time <= 0)
        {
            p_task->wait_time = 0;
            p_task->wake_reason = 1;
            if(p_task->status == SVCRT_TASK_WAIT)
            {
                p_task->status = SVCRT_TASK_READY;
            }
        }
        return;
    }

    p_task->period_time -= escape_tick;
    if(p_task->period_time <= 0)
    {
        p_task->period_time += p_task->period;
        if(p_task->period_time <= 0)
        {
            p_task->period_time = p_task->period;
        }
        if(p_task->status == SVCRT_TASK_WAIT)
        {
            p_task->status = SVCRT_TASK_READY;
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

#if (SVCRT_USE_SCHED_LOCK == 1)
/**
* @brief 检查调度器锁是否被持有（阻塞类接口的编程错误防护）
* @return 1=已锁定（调用方应忽略本次阻塞请求），0=未锁定
* @details 在调度器锁定期间调用阻塞接口会破坏任务状态一致性，
*          此处忽略请求并记录一条故障记录，便于定位问题代码。
*/
static int32 svcrt_sched_lock_blocks(void)
{
    if(svcrt_sched_lock_nest != 0u)
    {
        #if (SVCRT_USE_FAULT_RECOVER == 1)
        svcrt_fault_record(SVCRT_FAULT_SCHEDLOCK, svcrt_current_task_id);
        #endif
        return 1;
    }
    return 0;
}
#endif


void svcrt_task_wait_internal(uint32 ms)
{
    svcrt_task_t *p_tsk;
    SVCRT_DISABLE_IRQ();
    #if (SVCRT_USE_SCHED_LOCK == 1)
    if(svcrt_sched_lock_blocks())       /* 锁定期间禁止阻塞 */
    {
        SVCRT_ENABLE_IRQ();
        return;
    }
    #endif

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
    #if (SVCRT_USE_SCHED_LOCK == 1)
    if(svcrt_sched_lock_blocks())       /* 锁定期间禁止阻塞 */
    {
        SVCRT_ENABLE_IRQ();
        return;
    }
    #endif

    if(svcrt_current_task_id > 0)
    {
        p_tsk = &svcrt_task_table[svcrt_current_task_id - 1];
        p_tsk->status = SVCRT_TASK_WAIT;
        SVCRT_SWITCH_TASK();
    }
    SVCRT_ENABLE_IRQ();
}

void svcrt_task_block_internal(void)
{
    svcrt_task_t *p_tsk;
    SVCRT_DISABLE_IRQ();
    #if (SVCRT_USE_SCHED_LOCK == 1)
    if(svcrt_sched_lock_blocks())       /* 锁定期间禁止阻塞 */
    {
        SVCRT_ENABLE_IRQ();
        return;
    }
    #endif

    if(svcrt_current_task_id > 0)
    {
        p_tsk = &svcrt_task_table[svcrt_current_task_id - 1];
        p_tsk->wait_time = -1;          /* 无限阻塞，仅能被 post/unlock 唤醒 */
        p_tsk->status = SVCRT_TASK_WAIT;
        SVCRT_SWITCH_TASK();
    }
    SVCRT_ENABLE_IRQ();
}

void svcrt_task_delay_internal(uint32 us)
{
    svcrt_port_delay_us(us);
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

/* 任务状态查询：task_id 从 1 开始；非法任务号返回 -1 */
int32 svcrt_task_status_get_internal(int32 task_id)
{
    if(task_id <= 0 || task_id > svcrt_task_count)
        return -1;
    return (int32)svcrt_task_table[task_id - 1].status;
}

/* 任务故障恢复：重建栈帧、复位状态后重新调度，
 * 鐩稿綋浜庝换鍔＄骇鈥滆蒋澶嶄綅鈥濓紝閬垮厤鏁呴殰鍚庝换鍔℃Ы姘镐箙涓㈠け */
int32 svcrt_task_recover(int32 task_id)
{
    svcrt_task_t *p_task;

    if(task_id <= 0 || task_id > svcrt_task_count)
        return -1;

    p_task = &svcrt_task_table[task_id - 1];

    /* 涓ら樁娈垫仮澶嶏細
     * 阶段1：标记 INVALID + recover_pending，先脱离调度。
     * 若在 HardFault 路径中调用，随后 PendSV 保存旧现场时
     * 会把损坏的 PSP 写回 stack_ptr，不能在此处重建栈帧。
     * 阶段2：下一次 svcrt_sched_next() 扫描时执行重建并置 READY。 */
    SVCRT_DISABLE_IRQ();
    p_task->recover_pending = 1;
    p_task->status          = SVCRT_TASK_INVALID;
    SVCRT_ENABLE_IRQ();

    #if (SVCRT_USE_FAULT_RECOVER == 1)
    svcrt_fault_record(SVCRT_FAULT_RECOVER, task_id);
    #endif

    SVCRT_SWITCH_TASK();
    return 0;
}

/* 阶段2：处理待恢复任务（在调度扫描中调用，旧任务现场已保存完毕，重建栈帧安全） */
static void svcrt_task_recover_pending(void)
{
    int32 idx;
    svcrt_task_t *p_task;

    for(idx = 0; idx < svcrt_task_count; idx++)
    {
        p_task = &svcrt_task_table[idx];
        if(p_task->status == SVCRT_TASK_INVALID && p_task->recover_pending == 1)
        {
            p_task->recover_pending = 0;
            svcrt_task_stack_init(p_task, p_task->entry, p_task->stack_bottom, p_task->stack_size);
            p_task->wait_time   = 0;
            p_task->wake_reason = 0;
            p_task->period_time = p_task->period;
            p_task->status      = SVCRT_TASK_READY;
        }
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
    #if (SVCRT_USE_SCHED_LOCK == 1)
    if(svcrt_sched_lock_nest != 0u)
    {
        return;                 /* 调度器已锁定，延后到解锁时统一切换 */
    }
    #endif


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

#if (SVCRT_USE_SCHED_LOCK == 1)
/**
* @brief 获取调度器锁（用户任务经 SVC 调用）
* @details 累加嵌套计数后禁止任务切换，但不关闭中断。
*          临界区内不得调用阻塞接口（会被忽略并记录故障）。
*/
void svcrt_sched_lock_internal(void)
{
    uint32 state = SVCRT_ENTER_CRITICAL();

    if(svcrt_sched_lock_nest < 0xffffffffu)
    {
        svcrt_sched_lock_nest++;
    }

    SVCRT_EXIT_CRITICAL(state);
}

/**
* @brief 释放调度器锁
* @return 剩余嵌套层数（0 表示已完全解锁）
* @details 计数归零时主动触发一次任务切换，补偿锁定期间被推迟的调度。
*/
uint32 svcrt_sched_unlock_internal(void)
{
    uint32 state = SVCRT_ENTER_CRITICAL();
    uint32 nest;

    if(svcrt_sched_lock_nest > 0u)
    {
        svcrt_sched_lock_nest--;
    }

    nest = svcrt_sched_lock_nest;
    SVCRT_EXIT_CRITICAL(state);

    if(nest == 0u)
    {
        SVCRT_SWITCH_TASK();        /* 补偿锁定期间被推迟的任务切换 */
    }

    return nest;
}

/**
* @brief 查询调度器锁嵌套层数
* @return 当前嵌套层数（0 表示未锁定）
*/
int32 svcrt_sched_lock_count_internal(void)
{
    return (int32)svcrt_sched_lock_nest;
}
#endif /* SVCRT_USE_SCHED_LOCK */

#if (SVCRT_USE_STACK_USAGE == 1)
/**
* @brief 查询任务栈使用情况（峰值法）
* @param task_id 任务号（从 1 开始）
* @param out3    输出数组：out3[0]=总字节，out3[1]=峰值已用字节，out3[2]=剩余字节
* @return 0=成功，-1=参数非法
* @details 同时使用两种手段并取较大值：
*          1) 扫描填充图案得到“从未触及”区域；
*          2) 上下文切换记录的历史最低栈指针。
*/
int32 svcrt_task_stack_info_internal(int32 task_id, uint32 *out3)
{
    svcrt_task_t *p_task;
    uint32 words;
    uint32 i;
    uint32 free_words = 0u;
    uint32 used_fill;
    uint32 used_psp;
    uint32 used;

    if((out3 == 0) || (task_id <= 0) || (task_id > svcrt_task_count))
    {
        return -1;
    }

    p_task = &svcrt_task_table[task_id - 1];
    words  = p_task->stack_size / 4u;

    /* 从栈底（跳过保护字）向上扫描连续未使用的图案区 */
    for(i = 1u; i < words; i++)
    {
        if(p_task->stack_bottom[i] != SVCRT_STACK_FILL_PATTERN)
        {
            break;
        }
        free_words++;
    }

    used_fill = p_task->stack_size - free_words * 4u;
    used_psp  = (p_task->stack_top > p_task->stack_peak_low) ?
                (p_task->stack_top - p_task->stack_peak_low) : 0u;

    used = (used_fill > used_psp) ? used_fill : used_psp;
    if(used > p_task->stack_size)
    {
        used = p_task->stack_size;
    }

    out3[0] = p_task->stack_size;
    out3[1] = used;
    out3[2] = p_task->stack_size - used;

    return 0;
}
#endif /* SVCRT_USE_STACK_USAGE */

