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
#include "svcrt_log.h"
#include "svcrt_shell.h"
#include "svcrt_task.h"
#include "svcrt_event.h"
#include "svcrt_hal.h"
#include "svcrt_dev.h"
#include "svcrt_sync.h"
#include "svcrt_def.h"
#include "svcrt_config.h"
#include "svcrt_mq.h"
#include "svcrt_trace.h"
#include "svcrt_timer.h"
#include "svcrt_fault.h"
#include "svcrt_ptable.h"
#include "svcrt_mpu.h"
#include "svcrt_loader.h"
#include "svcrt_partition.h"    /* 用户可见内存窗口（共享区/App RAM/驱动 RAM/固件区） */

/* ------------------------------------------------------------------
 * 用户指针校验（SVC 可信边界）
 * SVC 分发把用户寄存器里的值直接当指针解引用（参数块、消息缓冲区、
 * 设备名、定时器回调……），不做校验时用户态可借这些接口读写内核任意地址、
 * 或让内核在特权态执行任意函数。
 * 这里按“地址必须落在用户可见窗口内”做最低限度校验：
 *   RAM 窗口：SHARE / SLOT_RAM（镜像 RAM 池，可写缓冲区允许的范围）
 *   固件窗口：IMAGE_POOL（用户代码与只读常量所在）
 * 内核 RAM（任务表、内核栈）与内核 Flash 一律拒绝。
 * ------------------------------------------------------------------ */
static uint8 svcrt_kernel_in_window(uint32 start, uint32 end, uint32 base, uint32 size)
{
    uint32 limit = base + size;

    if(limit <= base)                   /* 分区宏异常时不要误放行 */
    {
        return 0u;
    }
    return ((start >= base) && (end <= limit)) ? 1u : 0u;
}

/* Set only while svcrt_shell_ext_trampoline() runs an App callback; see the
 * trampoline in svcrt_shell.c for the reasoning. */
uint32 svcrt_kernel_ushell_active = 0;

static uint8 svcrt_kernel_ptr_ram_ok(const void *p, uint32 len)
{
    uint32 start = (uint32)p;
    uint32 end   = start + len;

    if(p == 0 || end < start)           /* 空指针或长度回绕 */
    {
        return 0u;
    }
    if(svcrt_kernel_ushell_active != 0u)
    {
        /* An App shell callback is running on the kernel shell task stack, so
         * its buffers are in kernel RAM and match none of the user windows.
         * Accept them for the duration of the call, otherwise print / log /
         * queue services invoked from the callback would all be rejected. */
        return 1u;
    }
    if(svcrt_kernel_in_window(start, end, SHARE_RAM_BASE, SHARE_RAM_SIZE) != 0u)
    {
        return 1u;
    }
    if(svcrt_kernel_in_window(start, end, SLOT_RAM_BASE, SLOT_RAM_TOTAL) != 0u)
    {
        return 1u;
    }
    return 0u;
}

static uint8 svcrt_kernel_ptr_flash_ok(const void *p, uint32 len)
{
    uint32 start = (uint32)p;
    uint32 end   = start + len;

    if(p == 0 || end < start)
    {
        return 0u;
    }
    if(svcrt_kernel_ptr_ram_ok(p, len) != 0u)
    {
        return 1u;
    }
    if(svcrt_kernel_in_window(start, end, IMAGE_POOL_BASE, IMAGE_POOL_SIZE) != 0u)
    {
        return 1u;
    }
    return 0u;
}

/* SVC 参数块（sub-cmd + 参数数组）必须位于用户可见 RAM */
static uint8 svcrt_kernel_svc_args_ok(const void *p, uint32 len)
{
    return svcrt_kernel_ptr_ram_ok(p, len);
}

/* 消息缓冲区：字数必须在 [1, SVCRT_MQ_MSG_WORDS] 内，且整块位于用户可见 RAM */
static uint8 svcrt_kernel_mq_buf_ok(const void *p, int32 len_words)
{
    if(len_words <= 0 || len_words > SVCRT_MQ_MSG_WORDS)
    {
        return 0u;
    }
    return svcrt_kernel_ptr_ram_ok(p, (uint32)len_words * 4u);
}

/* 名字类参数（设备名/信号量名/互斥锁名/消息队列名/定时器名/事件名）：
 * 内核最多读 len 字节（含 NUL），字符串常量通常在用户固件区，
 * 因此按“RAM 窗口 + 用户固件窗口”判定。
 * len 必须按目标对象名字段的真实大小给出：事件名是 char[16]（最多读 15 字节），
 * 其余对象是 char[8]。统一按 8 校验会让事件名有 7 个字节落在校验窗口之外。 */
static uint8 svcrt_kernel_user_name_ok(const void *p, uint32 len)
{
    return svcrt_kernel_ptr_flash_ok(p, len);
}

/* 定时器回调：必须落在调用者自身的固件区内。
 * 原判定只要求“不是内核 Flash”，于是某个 App 可以把回调指向驱动池或另一个
 * App 的槽位——等于借内核特权态去执行别人安装的代码。 */
static uint8 svcrt_kernel_cb_own_region_ok(const void *p)
{
    svcrt_task_t *p_tsk = svcrt_task_get_current();
    uint32 start;

    if(p == 0)
    {
        return 0u;
    }

    if((p_tsk != 0) && (p_tsk->rom_size != 0u))
    {
        start = (uint32)p;
        return ((start >= p_tsk->rom_start) &&
                (start < (p_tsk->rom_start + p_tsk->rom_size))) ? 1u : 0u;
    }

    /* 内核自身任务没有独立固件区（rom_size==0）：退回“用户可见代码区”判定 */
    return svcrt_kernel_ptr_flash_ok(p, 2u);
}

/* 设备读写缓冲区：长度必须为正，且整块落在用户可见 RAM 内。
 * 不校验时调用者能让内核向任意地址写入（dev_read）、
 * 或把任意地址的内容读出来送给外设（dev_write）。 */
/* Stack window handed to a user thread. The whole [base, base+size) range has
 * to sit inside the caller's own RAM window - the one the loader recorded for
 * this task - otherwise a thread could be given a stack that overlaps the
 * kernel stack or a neighbour App and quietly corrupt it.
 * svcrt_kernel_ushell_active is honoured for the same reason it is honoured
 * everywhere else: an App shell callback runs on the kernel shell stack. */
static uint8 svcrt_kernel_cb_own_ram_ok(const void *p, uint32 len)
{
    svcrt_task_t *p_tsk = svcrt_task_get_current();
    uint32 start = (uint32)p;
    uint32 end   = start + len;

    if((p == 0) || (len == 0u) || (end < start))
    {
        return 0u;
    }
    if(p_tsk == 0)
    {
        return 0u;
    }
    if(svcrt_kernel_ushell_active != 0u)
    {
        return 1u;
    }
    if(p_tsk->ram_size == 0u)
    {
        /* No user window was ever recorded for this task: nothing to donate. */
        return 0u;
    }
    return svcrt_kernel_in_window(start, end, p_tsk->ram_start, p_tsk->ram_size);
}

static uint8 svcrt_kernel_dev_buf_ok(const void *p, int32 len)
{
    if(len <= 0)
    {
        return 0u;
    }
    return svcrt_kernel_ptr_ram_ok(p, (uint32)len);
}

/* User read only buffer: the whole range sits either in the user visible RAM
 * or inside the caller's own firmware window. Used by the paths where the
 * kernel only READS the bytes (print / log / feed a device), so read only
 * string literals are legal and the feature matches its documented contract.
 * Paths where the kernel WRITES into the buffer (dev_read) must keep using
 * svcrt_kernel_dev_buf_ok below. */
static uint8 svcrt_kernel_user_ro_ok(const void *p, uint32 len)
{
    const char *s = (const char *)p;

    if(len == 0u)
    {
        return 0u;
    }
    if(svcrt_kernel_ptr_ram_ok(p, len) != 0u)
    {
        return 1u;
    }
    if(svcrt_kernel_cb_own_region_ok(p) == 0u)
    {
        return 0u;
    }
    return svcrt_kernel_cb_own_region_ok((const void *)&s[len - 1u]);
}

/* 驱动回调表：表本身位于用户可见 RAM，5 个回调指针必须落在用户代码区。
 * （这挡不住“用自己的代码提权”，但能挡住内核跳到内核 Flash 执行。） */
static uint8 svcrt_kernel_dev_drv_ok(const svcrt_dev_drv_t *drv)
{
    if(drv == 0)
    {
        return 0u;
    }
    if(svcrt_kernel_ptr_ram_ok(drv, (uint32)sizeof(svcrt_dev_drv_t)) == 0u)
    {
        return 0u;
    }
    if((drv->drv_open != 0) &&
       (svcrt_kernel_ptr_flash_ok((const void *)drv->drv_open, 2u) == 0u))
    {
        return 0u;
    }
    if((drv->drv_close != 0) &&
       (svcrt_kernel_ptr_flash_ok((const void *)drv->drv_close, 2u) == 0u))
    {
        return 0u;
    }
    if((drv->drv_read != 0) &&
       (svcrt_kernel_ptr_flash_ok((const void *)drv->drv_read, 2u) == 0u))
    {
        return 0u;
    }
    if((drv->drv_write != 0) &&
       (svcrt_kernel_ptr_flash_ok((const void *)drv->drv_write, 2u) == 0u))
    {
        return 0u;
    }
    if((drv->drv_ctrl != 0) &&
       (svcrt_kernel_ptr_flash_ok((const void *)drv->drv_ctrl, 2u) == 0u))
    {
        return 0u;
    }
    return 1u;
}


int32 svcrt_thread_create_internal(void (*entry)(void), uint32 *stack_bottom,
                                   uint32 stack_size, uint8 priority, uint32 period_ms)
{
    /* The entry point must be the caller's own code, never the kernel's and
     * never another App's image. */
    if(svcrt_kernel_cb_own_region_ok((const void *)entry) == 0u)
    {
        return -1;
    }
    /* The stack must be RAM the caller owns. */
    if(svcrt_kernel_cb_own_ram_ok((const void *)stack_bottom, stack_size) == 0u)
    {
        return -1;
    }
    /* 255 is the scheduler's "no candidate" sentinel, so a task carrying it
     * would never be picked. Refuse instead of silently clamping. */
    if(priority >= 255u)
    {
        return -1;
    }
    return svcrt_task_register(entry, stack_bottom, stack_size, priority, period_ms);
}

int32 svcrt_thread_self_internal(void)
{
    return (svcrt_current_task_id > 0) ? svcrt_current_task_id : 0;
}

void svcrt_thread_exit_internal(void)
{
    /* Self kill: the scheduler drops the task and switches away for good. */
    svcrt_task_kill_internal();
}

volatile uint32 svcrt_interrupt_nest = 0;
uint32 svcrt_kernel_tick = 0;
int32  svcrt_current_task_id = 0;
#if (SVCRT_USE_SCHED_LOCK == 1)
/* 调度器锁嵌套计数：>0 表示当前禁止任务切换（svcrt_sched_lock/unlock 维护） */
volatile uint32 svcrt_sched_lock_nest = 0;
#endif

#if (SVCRT_USE_CPU_LOAD == 1)
uint16 svcrt_cpu_load_counter = 0;
uint16 svcrt_cpu_busy_ticks = 0;   /* busy ticks of the last 1024-tick window */
#endif

static uint32 svcrt_idle_stack_ptr = 0;

static void svcrt_sched_tick_sweep(void);
static void svcrt_task_recover_mark(int32 task_id);
static void svcrt_task_recover_pending(void);


#if (SVCRT_USE_SCHED_STAT == 1)
/* 统计符号定义在 svcrt_trace.c（可观测性模块），这里只做本地声明 */
extern volatile uint32 svcrt_sched_stat_n;
extern volatile uint32 svcrt_sched_stat_sum;
extern volatile uint32 svcrt_sched_stat_min;
extern volatile uint32 svcrt_sched_stat_max;
extern void svcrt_sched_stat_close(uint32 t0);
#define SVCRT_SCHED_STAT_BEGIN()   uint32 stat_t0__ = svcrt_trace_last_cycles()
#define SVCRT_SCHED_STAT_END()     svcrt_sched_stat_close(stat_t0__)
#else
#define SVCRT_SCHED_STAT_BEGIN()   do { } while(0)
#define SVCRT_SCHED_STAT_END()     do { } while(0)
#endif

void svcrt_kernel_tick_handler(void)
{
    svcrt_kernel_tick++;
    #if (SVCRT_USE_CPU_LOAD == 1)
    /* sample the preempted context BEFORE switching tasks: count this tick */
    /* as busy only when a real task was on the CPU (idle = task id 0) */
    if(svcrt_current_task_id > 0)
    {
        svcrt_cpu_load_counter++;
    }
    #endif
    #if (SVCRT_USE_TIMER == 1)
    /* 软定时器倒计时与到期唤醒（不在中断里执行用户回调） */
    svcrt_timer_tick_handler();
    #endif

    /* 延时链到期处理：只处理链头已经到期的节点（通常是 0 个），
     * 不再全表扫倒计时。位置与原先等价：原先扫描发生在
     * svcrt_timer_tick_handler() 之后（sched_next 由 PendSV 触发），
     * 现在紧接其后、切换之前，保证本拍超时唤醒的任务当拍就能被选中。 */
    svcrt_sched_tick_sweep();

    #if (SVCRT_TIME_SLICE_TICKS > 0)
    /* 时间片：当前任务连续运行满 SVCRT_TIME_SLICE_TICKS 拍后让出同优先级队首。
     * 抢占不受片长影响：更高优先级任务就绪时位图直接给出它，本拍就切过去。 */
    if(svcrt_current_task_id > 0)
    {
        svcrt_task_t *p_cur = &svcrt_task_table[svcrt_current_task_id - 1];

        p_cur->slice_tick--;
        if(p_cur->slice_tick <= 0)
        {
            p_cur->slice_tick = (int32)SVCRT_TIME_SLICE_TICKS;
            svcrt_ready_rotate(svcrt_current_task_id - 1);
        }
    }
    #endif

    #if (SVCRT_USE_SCHED_LOCK == 1)
    if(svcrt_sched_lock_nest == 0u)         /* 调度器锁定期间不触发任务切换 */
    #endif
    {
        #if (SVCRT_USE_FAST_TICK_SWITCH == 1)
        /* 只有确实要换任务时才拉 PendSV。没人要切换时也走一整趟异常往返纯属
         * 浪费（F427@96MHz 实测 841 cycles/拍，2kHz 节拍下约 1.7% CPU）。
         * svcrt_sched_is_switching() 与 PendSV 用的是同一份判定
         * （都走 svcrt_sched_next() 的 O(1) 就绪队列 + 调度锁检查），可重复调用。 */
        if(svcrt_sched_is_switching() >= 0)
        {
            SVCRT_SWITCH_TASK();
        }
        #else
        SVCRT_SWITCH_TASK();
        #endif
    }

    #if (SVCRT_USE_CPU_LOAD == 1)
    /* close the window unconditionally so the snapshot stays periodic even */
    /* when the boundary tick happens to land on the idle task */
    if((svcrt_kernel_tick & 0x3ff) == 0)
    {
        svcrt_cpu_busy_ticks = svcrt_cpu_load_counter;
        svcrt_cpu_load_counter = 0;
    }
    #endif
}

void SVC_Server(void *p_svc_ctx)
{
    svcrt_trace_isr(SVCRT_TR_IRQ_SVC, SVCRT_TR_ISR_ENTER);
    uint32 svc_num = SVCRT_SVC_NUM(p_svc_ctx);
    uint32 *p;

    switch(svc_num)
    {
    case SVCRT_SVC_EVENT_CTRL:
        p = (uint32 *)SVCRT_SVC_ARG(p_svc_ctx, 0);
        /* 用户参数块必须先校验再解引用：内核 RAM / 内核 Flash 一律拒绝 */
        if(svcrt_kernel_svc_args_ok(p, 16u) == 0u)
        {
            SVCRT_SVC_RET(p_svc_ctx, (uint32)(-1));
            break;
        }
        switch(p[0])
        {
            case 1:
                /* 事件名字段是 char[16]，校验窗口必须给到 16 字节 */
                if(svcrt_kernel_user_name_ok((const void *)p[1], 16u) == 0u)
                {
                    SVCRT_SVC_RET(p_svc_ctx, (uint32)(-1));
                    break;
                }
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
                SVCRT_SVC_RET(p_svc_ctx, svcrt_kernel_get_cpu_busy_ticks());
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
            case 5:
                /* Pool space still available for installing images. r1, when
                 * non-zero, receives the longest free run so the caller can
                 * tell one big hole from the same total split into pieces. */
                {
                    uint32 pool_largest = 0u;
                    uint32 pool_total = svcrt_loader_pool_free(&pool_largest);

                    if(SVCRT_SVC_ARG(p_svc_ctx, 1) != 0u)
                    {
                        ((uint32 *)SVCRT_SVC_ARG(p_svc_ctx, 1))[0] = pool_largest;
                    }

                    SVCRT_SVC_RET(p_svc_ctx, pool_total);
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
                if(svcrt_kernel_ptr_ram_ok((const void *)SVCRT_SVC_ARG(p_svc_ctx, 2), 12u) == 0u)
                {
                    SVCRT_SVC_RET(p_svc_ctx, (uint32)(-1));
                    break;
                }
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
        /* 用户参数块必须先校验再解引用：内核 RAM / 内核 Flash 一律拒绝 */
        if(svcrt_kernel_svc_args_ok(p, 16u) == 0u)
        {
            SVCRT_SVC_RET(p_svc_ctx, (uint32)(-1));
            break;
        }
        switch(p[0])
        {
            case 1:
                if(svcrt_kernel_user_name_ok((const void *)p[1], 8u) == 0u)
                {
                    SVCRT_SVC_RET(p_svc_ctx, (uint32)(-1));
                    break;
                }
                SVCRT_SVC_RET(p_svc_ctx, svcrt_dev_open_internal((char *)p[1], p[2]));
                break;
            case 2:
                if(svcrt_kernel_dev_buf_ok((const void *)p[2], (int32)p[3]) == 0u)
                {
                    SVCRT_SVC_RET(p_svc_ctx, (uint32)(-1));
                    break;
                }
                SVCRT_SVC_RET(p_svc_ctx, svcrt_dev_read_internal(p[1], (uint8 *)p[2], p[3]));
                break;
            case 3:
                /* len > 0 : the kernel reads len bytes from the caller
                 *           buffer, so the range must be user visible.
                 * len <= 0: no payload is read; the value is a driver
                 *           defined command (led: 0=off, <0=toggle) and
                 *           must reach the driver like the internal
                 *           svcrt_dev_write_internal() path does. */
                if(((int32)p[3] > 0) &&
                   (svcrt_kernel_user_ro_ok((const void *)p[2], (uint32)p[3]) == 0u))
                {
                    SVCRT_SVC_RET(p_svc_ctx, (uint32)(-1));
                    break;
                }
                if(((int32)p[3] > 0) && (svcrt_console_is_handle(p[1]) != 0))
                {
                    /* Console: the whole buffer is one output unit, so a
                     * user mode write() can never be cut in half by a
                     * kernel log line. */
                    SVCRT_SVC_RET(p_svc_ctx, svcrt_console_write((const uint8 *)p[2], (uint32)p[3]));
                }
                else
                {
                    SVCRT_SVC_RET(p_svc_ctx, svcrt_dev_write_internal(p[1], (uint8 *)p[2], p[3]));
                }
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
        /* 用户参数块必须先校验再解引用：内核 RAM / 内核 Flash 一律拒绝 */
        if(svcrt_kernel_svc_args_ok(p, 16u) == 0u)
        {
            SVCRT_SVC_RET(p_svc_ctx, (uint32)(-1));
            break;
        }
        switch(p[0])
        {
            case 1:
                if(svcrt_kernel_user_name_ok((const void *)p[1], 8u) == 0u ||
                   svcrt_kernel_dev_drv_ok((const svcrt_dev_drv_t *)p[2]) == 0u)
                {
                    SVCRT_SVC_RET(p_svc_ctx, (uint32)(-1));
                    break;
                }
                SVCRT_SVC_RET(p_svc_ctx, svcrt_dev_register((char *)p[1], (svcrt_dev_drv_t *)p[2], p[3]));
                break;
            case 2:
                if(svcrt_kernel_user_name_ok((const void *)p[1], 8u) == 0u)
                {
                    SVCRT_SVC_RET(p_svc_ctx, (uint32)(-1));
                    break;
                }
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
        /* 用户参数块必须先校验再解引用：内核 RAM / 内核 Flash 一律拒绝 */
        if(svcrt_kernel_svc_args_ok(p, 16u) == 0u)
        {
            SVCRT_SVC_RET(p_svc_ctx, (uint32)(-1));
            break;
        }
        switch(p[0])
        {
            case 1:
                if(svcrt_kernel_user_name_ok((const void *)p[1], 8u) == 0u)
                {
                    SVCRT_SVC_RET(p_svc_ctx, (uint32)(-1));
                    break;
                }
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
                if(svcrt_kernel_user_name_ok((const void *)p[1], 8u) == 0u)
                {
                    SVCRT_SVC_RET(p_svc_ctx, (uint32)(-1));
                    break;
                }
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
        /* 用户参数块必须先校验再解引用：内核 RAM / 内核 Flash 一律拒绝 */
        if(svcrt_kernel_svc_args_ok(p, 16u) == 0u)
        {
            SVCRT_SVC_RET(p_svc_ctx, (uint32)(-1));
            break;
        }
        switch(p[0])
        {
        case 1:
            if(svcrt_kernel_user_name_ok((const void *)p[1], 8u) == 0u)
            {
                SVCRT_SVC_RET(p_svc_ctx, (uint32)(-1));
                break;
            }
            SVCRT_SVC_RET(p_svc_ctx, svcrt_mq_create_internal((char *)p[1]));
            break;
        case 2:
            if(svcrt_kernel_svc_args_ok(p, 24u) == 0u ||
               svcrt_kernel_mq_buf_ok((const void *)p[2], (int32)p[3]) == 0u)
            {
                SVCRT_SVC_RET(p_svc_ctx, (uint32)(-1));
                break;
            }
            SVCRT_SVC_RET(p_svc_ctx, svcrt_mq_send_internal((int32)p[1], (uint32 *)p[2], (int32)p[3], (int32)p[4]));
            break;
        case 3:
            if(svcrt_kernel_svc_args_ok(p, 24u) == 0u ||
               svcrt_kernel_mq_buf_ok((const void *)p[2], (int32)p[3]) == 0u)
            {
                SVCRT_SVC_RET(p_svc_ctx, (uint32)(-1));
                break;
            }
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
        /* 用户参数块必须先校验再解引用：内核 RAM / 内核 Flash 一律拒绝 */
        if(svcrt_kernel_svc_args_ok(p, 16u) == 0u)
        {
            SVCRT_SVC_RET(p_svc_ctx, (uint32)(-1));
            break;
        }
        switch(p[0])
        {
        case 1:
            if(svcrt_kernel_user_name_ok((const void *)p[1], 8u) == 0u)
            {
                SVCRT_SVC_RET(p_svc_ctx, (uint32)(-1));
                break;
            }
            SVCRT_SVC_RET(p_svc_ctx, svcrt_timer_create_internal((char *)p[1]));
            break;
        case 2:
            /* 回调会在内核特权态执行：回调必须落在调用者自己的固件区内，
             * 参数指针必须位于用户可见 RAM */
            if(svcrt_kernel_svc_args_ok(p, 24u) == 0u ||
               svcrt_kernel_cb_own_region_ok((const void *)p[4]) == 0u ||
               ((p[5] != 0u) &&
                (svcrt_kernel_ptr_ram_ok((const void *)p[5], 1u) == 0u)))
            {
                SVCRT_SVC_RET(p_svc_ctx, (uint32)(-1));
                break;
            }
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

    case SVCRT_SVC_THREAD_CTRL:
        p = (uint32 *)SVCRT_SVC_ARG(p_svc_ctx, 0);
        /* Sub-command plus its arguments must sit in user visible RAM. The
         * entry point and the stack window get a second, stricter check down
         * in svcrt_thread_create_internal(): they have to belong to the
         * caller, not merely to somebody's user window. */
        if(svcrt_kernel_svc_args_ok(p, 24u) == 0u)
        {
            SVCRT_SVC_RET(p_svc_ctx, (uint32)(-1));
            break;
        }
        switch(p[0])
        {
            case 1:
                /* p[1]=entry p[2]=stack_bottom p[3]=stack_size
                 * p[4]=priority p[5]=period_ms (0 = event driven) */
                SVCRT_SVC_RET(p_svc_ctx, (uint32)svcrt_thread_create_internal(
                                  (void (*)(void))p[1], (uint32 *)p[2],
                                  p[3], (uint8)p[4], p[5]));
                break;
            case 2:
                SVCRT_SVC_RET(p_svc_ctx, (uint32)svcrt_thread_self_internal());
                break;
            case 3:
                svcrt_thread_exit_internal();
                break;
            default:
                SVCRT_SVC_RET(p_svc_ctx, (uint32)(-1));
                break;
        }
        break;

    case SVCRT_SVC_LOG:
        /* User log: r0=level, r1=tag, r2=msg. Both strings must sit in the
         * caller's own RAM or inside the caller's own firmware window (read
         * only string literals are the normal case); kernel RAM and kernel
         * flash stay rejected. */
        {
            const char *tag = (const char *)SVCRT_SVC_ARG(p_svc_ctx, 1);
            const char *msg = (const char *)SVCRT_SVC_ARG(p_svc_ctx, 2);

            if((svcrt_kernel_user_ro_ok(tag, 1u) == 0u) ||
               (svcrt_kernel_user_ro_ok(msg, 1u) == 0u))
            {
                SVCRT_SVC_RET(p_svc_ctx, (uint32)(-1));
                break;
            }

            SVCRT_SVC_RET(p_svc_ctx,
                          (uint32)svcrt_log_svc(SVCRT_SVC_ARG(p_svc_ctx, 0), tag, msg));
        }
        break;

    case SVCRT_SVC_SHELL:
        /* User console service:
         *   p[0] = 1 register   (p[1] = svcrt_ushell_cmd_t * in caller RAM)
         *   p[0] = 2 unregister (p[1] = name string)
         *   p[0] = 3 print      (p[1] = text string)
         * The descriptor lives in the caller's RAM and the handler must stay
         * inside the caller's own firmware window - the same policy the timer
         * callbacks use, so nobody can borrow shell privilege to run someone
         * else's code. */
        {
            uint32 *q = (uint32 *)SVCRT_SVC_ARG(p_svc_ctx, 0);

            if(svcrt_kernel_svc_args_ok(q, 16u) == 0u)
            {
                SVCRT_SVC_RET(p_svc_ctx, (uint32)(-1));
                break;
            }

            switch(q[0])
            {
                case 1:
                    {
                        const svcrt_ushell_cmd_t *cmd = (const svcrt_ushell_cmd_t *)q[1];

                        if((svcrt_kernel_ptr_ram_ok(cmd, (uint32)sizeof(svcrt_ushell_cmd_t)) == 0u) ||
                           ((svcrt_kernel_ptr_ram_ok(cmd->name, 1u) == 0u) &&
                            (svcrt_kernel_cb_own_region_ok(cmd->name) == 0u)) ||
                           ((svcrt_kernel_ptr_ram_ok(cmd->help, 1u) == 0u) &&
                            (svcrt_kernel_cb_own_region_ok(cmd->help) == 0u)) ||
                           (svcrt_kernel_cb_own_region_ok((const void *)cmd->func) == 0u))
                        {
                            SVCRT_SVC_RET(p_svc_ctx, (uint32)(-1));
                            break;
                        }

                        SVCRT_SVC_RET(p_svc_ctx, (uint32)svcrt_shell_ext_register(cmd));
                    }
                    break;

                case 2:
                    {
                        const char *name = (const char *)q[1];

                        if(svcrt_kernel_user_name_ok((const void *)name, 16u) == 0u)
                        {
                            SVCRT_SVC_RET(p_svc_ctx, (uint32)(-1));
                            break;
                        }

                        SVCRT_SVC_RET(p_svc_ctx, (uint32)svcrt_shell_ext_unregister(name));
                    }
                    break;

                case 3:
                    {
                        const char *msg = (const char *)q[1];
                        uint32 len;

                        if(svcrt_kernel_user_ro_ok(msg, 1u) == 0u)
                        {
                            SVCRT_SVC_RET(p_svc_ctx, (uint32)(-1));
                            break;
                        }

                        /* Bound the length here: the shell side copies into its
                         * own static buffer, so nothing is allocated on the
                         * caller's (shallow) task stack. */
                        for(len = 0u; len < 128u; len++)
                        {
                            if(msg[len] == '\0')
                            {
                                break;
                            }
                        }
                        if(len >= 128u)
                        {
                            SVCRT_SVC_RET(p_svc_ctx, (uint32)(-1));
                            break;
                        }

                        SVCRT_SVC_RET(p_svc_ctx, (uint32)svcrt_shell_print_n(msg, len));
                    }
                    break;

                default:
                    SVCRT_SVC_RET(p_svc_ctx, (uint32)(-1));
                    break;
            }
        }
        break;


    case SVCRT_SVC_APP_MGR:
        p = (uint32 *)SVCRT_SVC_ARG(p_svc_ctx, 0);
        /* 用户参数块必须先校验再解引用：内核 RAM / 内核 Flash 一律拒绝 */
        if(svcrt_kernel_svc_args_ok(p, 16u) == 0u)
        {
            SVCRT_SVC_RET(p_svc_ctx, (uint32)(-1));
            break;
        }
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
    svcrt_trace_isr(SVCRT_TR_IRQ_SVC, SVCRT_TR_ISR_EXIT);
}

/* ============================================================
 * CPU 异常统一处理入口
 * @brief HardFault / MemManage / BusFault / UsageFault 共用
 * @details 任务上下文中发生异常时做任务级恢复（重建栈帧重启该任务），
 *          连续故障达到 APP_CRASH_RESTART_MAX 的应用由 svcrt_loader_on_fault()
 *          禁用，不再无限重启；内核/中断上下文异常则记录后停机等调试器接管。
 *          注意：CFSR/HFSR 保持不清，便于用调试器定位故障原因。
 * @return 可用于恢复的任务栈指针（非 0 时板级层调用 svcrt_port_resume_task 完成恢复）；
 *         0 表示不可恢复，板级层应停机等待调试器。
 * ============================================================ */
uint32 svcrt_cpu_fault_handler(uint32 fault_type)
{
    if(svcrt_current_task_id > 0)
    {
        int32 tid  = svcrt_current_task_id;
        int32 next;

        svcrt_fault_record(fault_type, tid);

        #if (SVCRT_USE_FAULT_RECOVER == 1)
        /* 未达连续故障上限：安排重启（重建栈帧）；
         * 达到上限：svcrt_loader_on_fault 内部已置 INVALID 并清零 recover_pending，
         * 该 App 被禁用、不再重启。 */
        if(svcrt_loader_on_fault(tid) == 1)
        {
            /* 已达连续故障上限、该 App 已被禁用：不重建栈帧，直接从调度中摘除。
             * 返回 -1 表示该任务不属于任何 App 槽位/驱动区（内核内置任务），
             * 仍按默认策略重启，不能当作“已禁用”处理。 */
        }
        else
        {
            svcrt_task_recover_mark(tid);
        }
        #else
        svcrt_ready_del(tid - 1);
        svcrt_delay_disarm(tid - 1);
        svcrt_task_table[tid - 1].status = SVCRT_TASK_INVALID;
        #endif

        /* 关键：故障路径不经 PendSV 切换。
         * HardFault 优先级(-1)高于 PendSV(0xFF)，故障处理程序活动期间 PendSV
         * 不会被服务；若只置 PendSV 就返回，PendSV 永远不会执行，故障任务会
         * 带着损坏现场继续运行（或落回向量末尾的 while(1)）——这正是此前
         * “连续崩溃达到上限即禁用”在真实硬件上从不生效的原因。
         * 这里直接选出下一个任务，把它的栈指针返回给板级层，
         * 由 svcrt_port_resume_task() 完成“恢复寄存器 + 异常返回”，
         * 等价于 PendSV 切换的后半段。
         * old_psp 传 0：故障任务现场不再保存（它即将重建或被禁用）。 */
        next = svcrt_sched_next() + 1;      /* recover_pending 在 sched_next 内部完成重建 */
        if(next < 0)
        {
            next = 0;                       /* 无就绪任务：退回空闲 */
        }

        return (uint32)svcrt_sched_activate(next, 0u);
    }

    /* 内核/中断上下文异常：没有可重启的任务，记录后停机等待调试器接管 */
    svcrt_fault_record(fault_type, 0);
    return 0u;
}

uint32 svcrt_hardfault_handler(void)
{
    return svcrt_cpu_fault_handler(SVCRT_FAULT_HARDFAULT);
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
        return -1;                          /* 还是它自己，不必拉 PendSV */
    }
    return new_idx;
}

int32 svcrt_sched_activate(int32 new_task, uint32 old_psp)
{
    int32 tid = svcrt_current_task_id - 1;

    SVCRT_SCHED_STAT_BEGIN();

    if(svcrt_current_task_id == 0)
    {
        svcrt_idle_stack_ptr = old_psp;
    }
    else if(old_psp != 0u)
    {
        /* old_psp == 0：故障恢复路径，调用方不保存故障任务现场
         * （该任务即将重建栈帧或被禁用），跳过 stack_ptr 回写与栈检查。 */
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
                svcrt_ready_del(tid);           /* 栈溢出：从就绪集中摘除 */
                svcrt_delay_disarm(tid);
                svcrt_task_table[tid].status = SVCRT_TASK_INVALID;
            }
            else
            #endif
            {
                svcrt_task_table[tid].status = SVCRT_TASK_READY;
            }
        }
    }

    svcrt_trace_switch(
        (uint8)((svcrt_current_task_id > 0) ? (svcrt_current_task_id - 1) : SVCRT_TR_ID_IDLE),
        (uint8)((new_task > 0) ? (new_task - 1) : SVCRT_TR_ID_IDLE));
    svcrt_current_task_id = new_task;
    if(svcrt_current_task_id > 0)
    {
        tid = svcrt_current_task_id - 1;
        svcrt_task_table[tid].touch_tick = svcrt_kernel_tick;
        svcrt_task_table[tid].status = SVCRT_TASK_RUNNING;
        svcrt_mpu_set_app(&svcrt_task_table[tid]);
        SVCRT_SCHED_STAT_END();         /* sched_activate: task path */
        return svcrt_task_table[tid].stack_ptr;
    }
    else
    {
        svcrt_current_task_id = 0;

        /* A task switch rewrites every MPU region register, so the idle task
         * needs its own context restored here (captured at startup by the
         * board through svcrt_port_set_idle_mpu()). */
        svcrt_mpu_set_idle();
        SVCRT_SCHED_STAT_END();         /* sched_activate: idle path */
        return svcrt_idle_stack_ptr;
    }
}

/* ============================================================
 * O(1) 就绪队列与延时链
 *
 * 旧实现每做一次调度决策就全表扫一遍（选最高优先级）：
 *   选择 O(N)、同优先级轮转 O(N)、每拍倒计时 O(N)。
 * 这里换成两个链表 + 一张两级位图，把「选任务」「轮转」「倒计时」
 * 全部降到 O(1)。延时链的插入是 O(N)，但插入只发生在任务阻塞时，
 * 不在调度决策路径上（RT-Thread 的 rt_timer_start 同样是 O(N) 插入）。
 * ============================================================ */

/* 就绪位图：bit p = 优先级 p 上有就绪任务。
 * 优先级是 0..254 的 uint8（255 是保留哨兵，注册时已拒绝），
 * 所以用 8 个 32 位字 + 一个 8 位组位图，两级定位固定两步。 */
static uint32 svcrt_ready_map[8];
static uint8  svcrt_ready_group = 0u;

/* 每个优先级的就绪任务双向循环链表头（存任务下标；SVCRT_TASK_NIL=空桶） */
static uint8 svcrt_ready_head[256];

/* 延时链：按 delay_tick 升序的双向链表，表头单独保存 */
static uint8 svcrt_delay_head = SVCRT_TASK_NIL;

static uint32 svcrt_ready_total = 0u;       /* 就绪链上的任务数（自检/调试用） */
static uint8  svcrt_recover_cnt  = 0u;      /* 待恢复任务数：为 0 时整段跳过扫描 */

/*
 * 取 32 位最低置位位号（0..31），入参必须非 0。
 * 不用 CMSIS 的 __CLZ/__RBIT：内核源文件不依赖 CMSIS 头，
 * 这里用固定 5 步二分，步数与输入无关，仍是 O(1)。
 */
static uint8 svcrt_lowbit32(uint32 v)
{
    uint8 n = 0u;

    if((v & 0x0000FFFFu) == 0u) { n = (uint8)(n + 16u); v >>= 16; }
    if((v & 0x000000FFu) == 0u) { n = (uint8)(n + 8u);  v >>= 8;  }
    if((v & 0x0000000Fu) == 0u) { n = (uint8)(n + 4u);  v >>= 4;  }
    if((v & 0x00000003u) == 0u) { n = (uint8)(n + 2u);  v >>= 2;  }
    if((v & 0x00000001u) == 0u) { n = (uint8)(n + 1u); }
    return n;
}

/* ------------------------------------------------------------
 * 就绪链表：入桶 / 出桶 / 取最优
 * 约定：调用方必须处在关中断的临界区内（内核里所有调用点都满足）。
 * ------------------------------------------------------------ */

static void svcrt_ready_link(int32 idx, uint8 prio)
{
    uint8 h = svcrt_ready_head[prio];
    uint8 i = (uint8)idx;

    if(h == SVCRT_TASK_NIL)
    {
        svcrt_ready_head[prio] = i;
        svcrt_task_table[idx].ready_next = i;
        svcrt_task_table[idx].ready_prev = i;
    }
    else
    {
        uint8 tail = svcrt_task_table[h].ready_prev;    /* 桶尾：新就绪者排在最后 */

        svcrt_task_table[tail].ready_next = i;
        svcrt_task_table[i].ready_prev = tail;
        svcrt_task_table[i].ready_next = h;
        svcrt_task_table[h].ready_prev = i;
    }

    svcrt_ready_map[prio >> 5] |= (1u << (prio & 31u));
    svcrt_ready_group |= (uint8)(1u << (prio >> 5));
    svcrt_ready_total++;
}

static void svcrt_ready_unlink(int32 idx, uint8 prio)
{
    uint8 i = (uint8)idx;
    uint8 n = svcrt_task_table[idx].ready_next;
    uint8 p = svcrt_task_table[idx].ready_prev;

    if(n == SVCRT_TASK_NIL)
    {
        return;                                 /* 本来就不在链上 */
    }

    if(n == i)
    {
        svcrt_ready_head[prio] = SVCRT_TASK_NIL;
        svcrt_ready_map[prio >> 5] &= ~(1u << (prio & 31u));
        if(svcrt_ready_map[prio >> 5] == 0u)
        {
            svcrt_ready_group &= (uint8)~(1u << (prio >> 5));
        }
    }
    else
    {
        svcrt_task_table[n].ready_prev = p;
        svcrt_task_table[p].ready_next = n;
        if(svcrt_ready_head[prio] == i)
        {
            svcrt_ready_head[prio] = n;
        }
    }

    svcrt_task_table[idx].ready_next = SVCRT_TASK_NIL;
    svcrt_task_table[idx].ready_prev = SVCRT_TASK_NIL;
    if(svcrt_ready_total > 0u)
    {
        svcrt_ready_total--;
    }
}

int32 svcrt_ready_top(void)
{
    uint32 g = (uint32)svcrt_ready_group;
    uint32 w;
    uint32 b;

    if(g == 0u)
    {
        return -1;
    }

    w = (uint32)svcrt_lowbit32(g);                       /* 组号 0..7 */
    b = (uint32)svcrt_lowbit32(svcrt_ready_map[w]);      /* 组内位号 0..31 */
    return (int32)svcrt_ready_head[(uint8)((w << 5) | b)];
}

void svcrt_ready_add(int32 idx)
{
    if((idx < 0) || (idx >= svcrt_task_count))
    {
        return;
    }
    if(svcrt_task_table[idx].ready_next != SVCRT_TASK_NIL)
    {
        return;                                 /* 已在就绪链上，幂等 */
    }
    svcrt_ready_link(idx, svcrt_task_table[idx].priority);
    svcrt_delay_disarm(idx);                    /* 已可运行就不再需要到期唤醒 */
}

void svcrt_ready_del(int32 idx)
{
    if((idx < 0) || (idx >= svcrt_task_count))
    {
        return;
    }
    svcrt_ready_unlink(idx, svcrt_task_table[idx].priority);
}

void svcrt_ready_reprio(int32 idx, uint8 old_prio)
{
    if((idx < 0) || (idx >= svcrt_task_count))
    {
        return;
    }
    if(svcrt_task_table[idx].ready_next == SVCRT_TASK_NIL)
    {
        return;                                 /* 不在就绪集：改 priority 即可 */
    }
    svcrt_ready_unlink(idx, old_prio);
    svcrt_ready_link(idx, svcrt_task_table[idx].priority);
}

void svcrt_ready_rotate(int32 idx)
{
    uint8 prio;

    if((idx < 0) || (idx >= svcrt_task_count))
    {
        return;
    }
    if(svcrt_task_table[idx].ready_next == SVCRT_TASK_NIL)
    {
        return;
    }

    prio = svcrt_task_table[idx].priority;
    if((svcrt_ready_head[prio] == (uint8)idx) &&
       (svcrt_task_table[idx].ready_next == (uint8)idx))
    {
        return;                                 /* 桶里只有它自己，无需搬动 */
    }
    svcrt_ready_unlink(idx, prio);
    svcrt_ready_link(idx, prio);                /* 追加到桶尾，下一拍让位 */
}

uint32 svcrt_sched_ready_count(void)
{
    return svcrt_ready_total;
}

/* 诊断用：该任务当前是否挂在就绪链上（与 sched_check 的判据一致） */
uint8 svcrt_ready_contains(int32 task_idx)
{
    if((task_idx < 0) || (task_idx >= svcrt_task_count))
    {
        return 0u;
    }
    return (svcrt_task_table[task_idx].ready_next != SVCRT_TASK_NIL) ? 1u : 0u;
}

/* ------------------------------------------------------------
 * 延时链：按绝对到期节拍升序
 * ------------------------------------------------------------ */

static void svcrt_delay_link(int32 idx)
{
    svcrt_task_t *p = &svcrt_task_table[idx];
    uint8 i  = (uint8)idx;
    uint8 cur = svcrt_delay_head;
    uint8 prev = SVCRT_TASK_NIL;
    uint32 t = p->delay_tick;

    while((cur != SVCRT_TASK_NIL) && (svcrt_task_table[cur].delay_tick <= t))
    {
        prev = cur;
        cur = svcrt_task_table[cur].delay_next;
    }

    if(prev == SVCRT_TASK_NIL)
    {
        p->delay_next = svcrt_delay_head;
        p->delay_prev = SVCRT_TASK_NIL;
        if(svcrt_delay_head != SVCRT_TASK_NIL)
        {
            svcrt_task_table[svcrt_delay_head].delay_prev = i;
        }
        svcrt_delay_head = i;
    }
    else
    {
        p->delay_prev = prev;
        p->delay_next = cur;
        svcrt_task_table[prev].delay_next = i;
        if(cur != SVCRT_TASK_NIL)
        {
            svcrt_task_table[cur].delay_prev = i;
        }
    }

    p->delay_queued = 1u;
}

static void svcrt_delay_unlink(int32 idx)
{
    svcrt_task_t *p;
    uint8 n;
    uint8 v;

    if((idx < 0) || (idx >= svcrt_task_count))
    {
        return;
    }

    p = &svcrt_task_table[idx];
    if(p->delay_queued == 0u)
    {
        return;
    }

    n = p->delay_next;
    v = p->delay_prev;

    if(v == SVCRT_TASK_NIL)
    {
        svcrt_delay_head = n;                   /* 它是表头 */
    }
    else
    {
        svcrt_task_table[v].delay_next = n;
    }
    if(n != SVCRT_TASK_NIL)
    {
        svcrt_task_table[n].delay_prev = v;
    }

    p->delay_next = SVCRT_TASK_NIL;
    p->delay_prev = SVCRT_TASK_NIL;
    p->delay_queued = 0u;
}

void svcrt_delay_arm(int32 idx, uint32 ticks)
{
    if((idx < 0) || (idx >= svcrt_task_count))
    {
        return;
    }
    svcrt_delay_unlink(idx);                    /* 幂等：先摘掉可能残留的旧节点 */
    svcrt_task_table[idx].delay_tick = svcrt_kernel_tick + ticks;
    svcrt_delay_link(idx);
}

void svcrt_delay_disarm(int32 idx)
{
    svcrt_delay_unlink(idx);
}

/* 每拍调用：只处理链头已经到期的节点，没到期就立刻返回。
 * 提前被 post/unlock 唤醒的任务其节点可能还挂在链上（唤醒路径不必逐个摘链），
 * 这类「僵尸节点」到期时发现状态已不是 WAIT 就直接丢弃，结果仍然正确。 */
void svcrt_delay_tick(void)
{
    while(svcrt_delay_head != SVCRT_TASK_NIL)
    {
        int32 idx = (int32)svcrt_delay_head;
        svcrt_task_t *p = &svcrt_task_table[idx];

        if((int32)(svcrt_kernel_tick - p->delay_tick) < 0)
        {
            break;                              /* 有符号比较，抗节拍回绕 */
        }

        svcrt_delay_unlink(idx);

        if(p->status != SVCRT_TASK_WAIT)
        {
            continue;                           /* 已被提前唤醒的僵尸节点 */
        }

        p->wake_reason = 1;                     /* 1 = 超时唤醒 */
        if(p->wait_time > 0)
        {
            p->wait_time = 0;
        }
        else
        {
            /* 周期等待到期：按原语义补回一个周期 */
            p->period_time += p->period;
            if(p->period_time <= 0)
            {
                p->period_time = p->period;
            }
        }
        p->status = SVCRT_TASK_READY;
        svcrt_ready_add(idx);
    }
}

void svcrt_ready_reset(void)
{
    int32 i;
    int32 j;

    for(i = 0; i < (int32)SVCRT_TASK_MAX_NUM; i++)
    {
        svcrt_task_table[i].ready_next   = SVCRT_TASK_NIL;
        svcrt_task_table[i].ready_prev   = SVCRT_TASK_NIL;
        svcrt_task_table[i].delay_next   = SVCRT_TASK_NIL;
        svcrt_task_table[i].delay_prev   = SVCRT_TASK_NIL;
        svcrt_task_table[i].delay_queued = 0u;
        svcrt_task_table[i].delay_tick   = 0u;
        svcrt_task_table[i].slice_tick   = (int32)SVCRT_TIME_SLICE_TICKS;
    }
    for(j = 0; j < 256; j++)
    {
        svcrt_ready_head[j] = SVCRT_TASK_NIL;
    }
    for(j = 0; j < 8; j++)
    {
        svcrt_ready_map[j] = 0u;
    }
    svcrt_ready_group = 0u;
    svcrt_ready_total = 0u;
    svcrt_recover_cnt = 0u;
    svcrt_delay_head  = SVCRT_TASK_NIL;
}

/* 从任务表的 status 重新派生整个就绪集（位图 + 每优先级链表）。
 *
 * 为什么需要它：status 是唯一真相，位图/链表只是它的索引。索引一旦与真相
 * 脱钩，调度器不会报错也不会崩，只会「静默地把某个 READY 任务永远排在门外」。
 * 手工填 TCB 的旧式注册（只写 status、不挂链）就是这种脱钩。
 * 首次调度前重建一次，把这类历史写法也纳入调度；O(N) 且只发生一次。
 * 重建前先清空所有桶与 TCB 链字段，因此不会产生重复节点。 */
void svcrt_ready_rebuild(void)
{
    int32 i;
    int32 j;

    for(j = 0; j < 256; j++)
    {
        svcrt_ready_head[j] = SVCRT_TASK_NIL;
    }
    for(j = 0; j < 8; j++)
    {
        svcrt_ready_map[j] = 0u;
    }
    svcrt_ready_group = 0u;
    svcrt_ready_total = 0u;

    for(i = 0; i < (int32)SVCRT_TASK_MAX_NUM; i++)
    {
        svcrt_task_table[i].ready_next = SVCRT_TASK_NIL;
        svcrt_task_table[i].ready_prev = SVCRT_TASK_NIL;
    }

    for(i = 0; i < svcrt_task_count; i++)
    {
        if((svcrt_task_table[i].status == SVCRT_TASK_READY) ||
           (svcrt_task_table[i].status == SVCRT_TASK_RUNNING))
        {
            svcrt_ready_link(i, svcrt_task_table[i].priority);
        }
    }
}

/* 一致性自检：把「位图/链表」与「任务表真实状态」对照一遍，
 * 再和一次全表扫描选出的最优任务比对。返回不一致的条目数，0 = 一致。
 * 供 shell 的 schedcheck 命令调用；正常运行时不需要它。 */
int32 svcrt_sched_check(void)
{
    int32 bad = 0;
    uint32 cnt = 0u;
    int32 idx;
    int32 scan;
    uint8 bp;
    uint32 bt;
    int32 top;

    for(idx = 0; idx < svcrt_task_count; idx++)
    {
        svcrt_task_t *p = &svcrt_task_table[idx];
        uint8 on = (p->ready_next != SVCRT_TASK_NIL) ? 1u : 0u;
        uint8 should = ((p->status == SVCRT_TASK_READY) ||
                        (p->status == SVCRT_TASK_RUNNING)) ? 1u : 0u;

        if(on != should)
        {
            bad++;
        }
        if(on)
        {
            cnt++;
        }
        if((p->delay_queued != 0u) && (p->delay_prev == SVCRT_TASK_NIL) &&
           (svcrt_delay_head != (uint8)idx))
        {
            bad++;                              /* 声称在延时链上却找不到它 */
        }
    }

    for(idx = 0; idx < 8; idx++)
    {
        uint32 m = svcrt_ready_map[idx];
        uint32 k;

        if((m != 0u) && ((svcrt_ready_group & (uint8)(1u << idx)) == 0u))
        {
            bad++;                              /* 字位图非空但组位图没标 */
        }
        if((m == 0u) && ((svcrt_ready_group & (uint8)(1u << idx)) != 0u))
        {
            bad++;                              /* 组位图说有，字位图却是空的 */
        }

        for(k = 0u; k < 32u; k++)
        {
            if((m & (1u << k)) != 0u)
            {
                uint8 pr = (uint8)((idx << 5) | (int32)k);

                if(svcrt_ready_head[pr] == SVCRT_TASK_NIL)
                {
                    bad++;                      /* 位图说有，桶却是空的 */
                }
                else if(svcrt_task_table[svcrt_ready_head[pr]].priority != pr)
                {
                    bad++;                      /* 桶里的任务优先级对不上 */
                }
            }
        }
    }

    if(cnt != svcrt_ready_total)
    {
        bad++;
    }

    scan = -1;
    bp = 255u;
    bt = 0xFFFFFFFFu;
    for(idx = 0; idx < svcrt_task_count; idx++)
    {
        svcrt_task_t *p = &svcrt_task_table[idx];

        if((p->status == SVCRT_TASK_READY) || (p->status == SVCRT_TASK_RUNNING))
        {
            if(p->priority < bp)
            {
                bp = p->priority;
                bt = p->touch_tick;
                scan = idx;
            }
            else if(p->priority == bp)
            {
                if(p->touch_tick < bt)
                {
                    bt = p->touch_tick;
                    scan = idx;
                }
            }
        }
    }

    top = svcrt_ready_top();
    if(((scan < 0) && (top >= 0)) || ((scan >= 0) && (top < 0)))
    {
        bad++;
    }
    else if((scan >= 0) && (svcrt_task_table[scan].priority != svcrt_task_table[top].priority))
    {
        bad++;                                  /* 选出的优先级不同：真不一致 */
    }

    return bad;
}

/* 首次调度前重建就绪集的一次性标记 */
static uint8 svcrt_ready_built = 0u;

int32 svcrt_sched_next(void)
{
    if(svcrt_ready_built == 0u)
    {
        /* 到这里为止所有任务都已注册完毕（首次调度必然发生在启动之后），
         * 把就绪集按 status 重新派生一次，兜住「手工填 TCB 不挂链」这类写法。 */
        svcrt_ready_rebuild();
        svcrt_ready_built = 1u;
    }

    if(svcrt_recover_cnt != 0u)
    {
        /* 只有确实存在待恢复任务时才需要扫表收尾 */
        svcrt_task_recover_pending();
    }

    /* O(1)：位图取最高优先级，再取该优先级就绪桶的队首 */
    return svcrt_ready_top();
}


/* 每个节拍只处理延时链上已经到期的任务，不再全表扫倒计时；
 * 选择本身由 svcrt_ready_top() 常数时间完成，也不再需要优先级缓存。 */
static void svcrt_sched_tick_sweep(void)
{
    svcrt_delay_tick();
}

svcrt_task_t *svcrt_task_get_current(void)
{
    /* 只读一个整数下标，读操作本身就是原子的，这里不需要也不能关中断：
     * 信号量/互斥锁/消息队列都是在各自的临界区内调用本函数，
     * 若在此处 SVCRT_ENABLE_IRQ()，会打开调用方的临界区，
     * 使“已挂进等待队列、但还没置 WAIT”的窗口暴露给 ISR 里的 post/unlock，
     * 唤醒被投给一个还没睡下的任务并消耗掉（丢唤醒）。 */
    if(svcrt_current_task_id > 0)
    {
        return &svcrt_task_table[svcrt_current_task_id - 1];
    }
    return 0;
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
        svcrt_ready_del(svcrt_current_task_id - 1);
        svcrt_delay_arm(svcrt_current_task_id - 1, (uint32)p_tsk->wait_time);
        p_tsk->status = SVCRT_TASK_WAIT;
        svcrt_trace_wait((uint8)(svcrt_current_task_id - 1));
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
        if(p_tsk->period_time <= 0)
        {
            p_tsk->period_time = p_tsk->period;
        }
        svcrt_ready_del(svcrt_current_task_id - 1);
        svcrt_delay_arm(svcrt_current_task_id - 1, (uint32)p_tsk->period_time);
        p_tsk->status = SVCRT_TASK_WAIT;
        svcrt_trace_wait((uint8)(svcrt_current_task_id - 1));
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
        svcrt_ready_del(svcrt_current_task_id - 1);
        svcrt_trace_wait((uint8)(svcrt_current_task_id - 1));
        p_tsk->status = SVCRT_TASK_WAIT;
        SVCRT_SWITCH_TASK();
    }
    SVCRT_ENABLE_IRQ();
}

/* 在调用方已持有的临界区内完成“置等待状态 + 切走”，返回时中断仍关。
 * 关键点：等待队列的登记（调用方在关中断下完成）与本函数的置 WAIT/切走之间
 * 不允许出现中断打开的瞬间，否则 ISR 里的 post/unlock/mq_put 会把唤醒投递给
 * 一个还没睡下的任务，唤醒随即被吞掉（任务会一直阻塞到超时）。
 * @return 0=被显式唤醒，1=超时，-1=未进入阻塞（内核上下文或调度器已锁定） */
int32 svcrt_task_block_in_critical(uint32 timeout_ms)
{
    svcrt_task_t *p_tsk;
    int32 reason;

    /* 调用约定：进入时中断已关 */
    if(svcrt_current_task_id <= 0)
    {
        return -1;                      /* 内核上下文不允许阻塞 */
    }

    #if (SVCRT_USE_SCHED_LOCK == 1)
    if(svcrt_sched_lock_blocks())
    {
        return -1;                      /* 调度器锁定期间禁止阻塞 */
    }
    #endif

    p_tsk = &svcrt_task_table[svcrt_current_task_id - 1];
    p_tsk->wake_reason = 0;
    /* 参数类型是 uint32，调用方用 (uint32)(-1) 表示无限等待；
     * 必须先转成有符号再判断，否则负值会被当成 0xFFFFFFFF 毫秒，
     * MS_TO_TICK 溢出成一个不可控的巨大节拍数。 */
    p_tsk->wait_time   = ((int32)timeout_ms > 0) ? (int32)SVCRT_MS_TO_TICK((uint32)timeout_ms) : -1;
    svcrt_ready_del(svcrt_current_task_id - 1);
    if(p_tsk->wait_time > 0)
    {
        svcrt_delay_arm(svcrt_current_task_id - 1, (uint32)p_tsk->wait_time);
    }
    p_tsk->status      = SVCRT_TASK_WAIT;
    svcrt_trace_wait((uint8)(svcrt_current_task_id - 1));

    SVCRT_SWITCH_TASK();                /* 只是置 PendSV pending，此刻中断还关着 */
    SVCRT_ENABLE_IRQ();                 /* 开中断：PendSV 切走与 ISR 唤醒才有机会发生 */
    SVCRT_DISABLE_IRQ();                /* 回到本函数与调用方共同的临界区 */

    reason = p_tsk->wake_reason;
    return reason;
}

void svcrt_task_delay_internal(uint32 us)
{
    svcrt_port_delay_us(us);
}

/* 任务下线收尸：清理该任务在同步对象/消息队列中的等待登记与锁持有关系。
 * 自带保存-恢复语义的临界区，因此可以在已有临界区内调用。 */
void svcrt_task_release_resources(int32 task_id)
{
    uint32 state;

    if(task_id <= 0 || task_id > svcrt_task_count)
    {
        return;
    }

    state = SVCRT_ENTER_CRITICAL();
    svcrt_sync_release_task(task_id);
    svcrt_mq_release_task(task_id);
    svcrt_event_release_task(task_id);
    SVCRT_EXIT_CRITICAL(state);
}

void svcrt_task_kill_internal(void)
{
    if(svcrt_current_task_id > 0)
    {
        /* 收尸：不清理的话，post/unlock 会把一个已经不在等待的任务置为
         * READY（相当于从旧栈“复活”它），而它持有的互斥锁会永久锁死。 */
        svcrt_task_release_resources(svcrt_current_task_id);

        SVCRT_DISABLE_IRQ();
        svcrt_ready_del(svcrt_current_task_id - 1);
        svcrt_delay_disarm(svcrt_current_task_id - 1);
        svcrt_task_table[svcrt_current_task_id - 1].status = SVCRT_TASK_INVALID;
        SVCRT_ENABLE_IRQ();
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
/* 标记任务待恢复（阶段1）：置 INVALID + recover_pending。
 * 真正的栈帧重建推迟到下一次调度扫描（svcrt_sched_next -> svcrt_task_recover_pending），
 * 那时故障任务的旧现场已经不会再被写回 stack_ptr，重建才安全。 */
static void svcrt_task_recover_mark(int32 task_id)
{
    svcrt_task_t *p_task;

    if(task_id <= 0 || task_id > svcrt_task_count)
    {
        return;
    }

    p_task = &svcrt_task_table[task_id - 1];

    /* 它持有的锁与等待登记必须一并清理：任务重启后会从入口重新开始，
     * 不会再去解锁/摘除，留下的锁会永久锁死。 */
    svcrt_task_release_resources(task_id);

    SVCRT_DISABLE_IRQ();
    /* 自身优先级按基准复位：故障时它可能正被继承提升，
     * 而收尸已把它从所有等待关系里摘除，若不复位，恢复后它会带着
     * 提升来的高优先级一直运行。 */
    /* 必须先按当前优先级把它从就绪集摘出：
     * svcrt_ready_del 用的是「当前 priority」定位桶，若先改 priority
     * 就会去错误的桶里摘，把链表拆坏。 */
    svcrt_ready_del(task_id - 1);
    svcrt_delay_disarm(task_id - 1);
    p_task->priority        = p_task->base_priority;
    if(p_task->recover_pending == 0u)
    {
        svcrt_recover_cnt++;            /* 重复 mark 同一任务只计一次 */
    }
    p_task->recover_pending = 1;
    p_task->status          = SVCRT_TASK_INVALID;
    SVCRT_ENABLE_IRQ();

    #if (SVCRT_USE_FAULT_RECOVER == 1)
    svcrt_fault_record(SVCRT_FAULT_RECOVER, task_id);
    #endif
}

int32 svcrt_task_recover(int32 task_id)
{
    if(task_id <= 0 || task_id > svcrt_task_count)
    {
        return -1;
    }

    svcrt_task_recover_mark(task_id);
    SVCRT_SWITCH_TASK();
    return 0;
}

/* 阶段2：处理待恢复任务（在调度扫描中调用，旧任务现场已保存完毕，重建栈帧安全） */
static void svcrt_task_recover_pending(void)
{
    int32 idx;
    svcrt_task_t *p_task;

    if(svcrt_recover_cnt == 0u)
    {
        return;                     /* 绝大多数调度决策走这里，不扫表 */
    }

    for(idx = 0; idx < svcrt_task_count; idx++)
    {
        p_task = &svcrt_task_table[idx];
        if(p_task->status == SVCRT_TASK_INVALID && p_task->recover_pending == 1)
        {
            p_task->recover_pending = 0;
            if(svcrt_recover_cnt > 0u)
            {
                svcrt_recover_cnt--;
            }
            svcrt_task_stack_init(p_task, p_task->entry, p_task->stack_bottom, p_task->stack_size);
            p_task->wait_time   = 0;
            p_task->wake_reason = 0;
            p_task->period_time = p_task->period;
            p_task->status      = SVCRT_TASK_READY;
            svcrt_ready_add(idx);
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

uint16 svcrt_kernel_get_cpu_busy_ticks(void)
{
    #if (SVCRT_USE_CPU_LOAD == 1)
    return svcrt_cpu_busy_ticks;
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

