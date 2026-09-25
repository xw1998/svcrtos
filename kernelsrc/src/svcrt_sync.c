/**
* @brief SVCrtOS 同步原语实现（信号量与互斥锁）
* @details 实现计数信号量和带优先级继承的互斥锁：
*          - 信号量：count>0 时 wait 直接减 1 通过，否则任务挂起等待；post 唤醒等待者或增加计数。
*          - 互斥锁：记录 owner，加锁时若被占用则挂起；为避免优先级反转，当高优先级任务等待时，
*            临时把持有者优先级提升到与等待者一致（优先级继承），解锁后恢复。
*          阻塞等待复用 svcrt_task_wait_internal/svcrt_task_block_internal 实现任务挂起与切换。
 *          阻塞返回后通过 task->wake_reason 区分唤醒原因（0=被 post/unlock 唤醒，
 *          1=等待超时），超时会把自己从等待队列摘除并返回负值错误码，
 *          绝不把“超时”当成“已获得资源”。
* @author xw
* @date 2026.05.30
*/

#include "svcrt_sync.h"
#include "svcrt_hal.h"
#include "svcrt_cfg.h"
#include "svcrt_trace.h"     /* kernel auto hooks: object waits/wakes as sync records */

static svcrt_sem_obj_t svcrt_sems[SVCRT_SEM_NUM];
static svcrt_mtx_obj_t svcrt_mtxs[SVCRT_MTX_NUM];
static svcrt_cond_obj_t svcrt_conds[SVCRT_COND_NUM];

/* Token-conservation diagnostics.  Both counters are only incremented on
 * paths where the kernel refused to claim a success it could not prove, so a
 * non-zero value always means "a wake reached a task nobody asked for",
 * never "the caller did something wrong".  Readable via the shell log. */
volatile uint32 svcrt_diag_sync_ghost = 0u;   /* wake without SVCRT_WAKE_HANDOFF */

/* Per object event ledger (shell "syncinfo").  A post that found a waiter
 * hands the token over without touching the count, so count alone cannot
 * say whether the object ever saw traffic; these counters can. */
static volatile uint32 dbg_sem_posts[SVCRT_SEM_NUM];
static volatile uint32 dbg_sem_takes[SVCRT_SEM_NUM];
static volatile uint32 dbg_sem_queued[SVCRT_SEM_NUM];
static volatile uint32 dbg_sem_last_waiter[SVCRT_SEM_NUM];

void svcrt_sync_sem_trace(int32 idx, uint32 *posts, uint32 *takes,
                          uint32 *queued, uint32 *last_waiter)
{
    if((idx < 0) || (idx >= SVCRT_SEM_NUM))
    {
        return;
    }
    if(posts != 0)        { *posts = dbg_sem_posts[idx]; }
    if(takes != 0)        { *takes = dbg_sem_takes[idx]; }
    if(queued != 0)       { *queued = dbg_sem_queued[idx]; }
    if(last_waiter != 0)  { *last_waiter = dbg_sem_last_waiter[idx]; }
}

static volatile uint32 dbg_sem_qbefore[SVCRT_SEM_NUM];
static volatile uint32 dbg_sem_woke[SVCRT_SEM_NUM];
static volatile uint32 dbg_sem_reason[SVCRT_SEM_NUM];
static volatile uint32 dbg_sem_also[SVCRT_SEM_NUM];

void svcrt_sync_sem_handoff(int32 idx, uint32 *woke, uint32 *reason,
                            uint32 *pendsv_also)
{
    if((idx < 0) || (idx >= SVCRT_SEM_NUM))
    {
        return;
    }
    if(woke != 0)        { *woke = dbg_sem_woke[idx]; }
    if(reason != 0)      { *reason = dbg_sem_reason[idx]; }
    if(pendsv_also != 0) { *pendsv_also = dbg_sem_also[idx]; }
}

uint32 svcrt_sync_sem_qbefore(int32 idx)
{
    if((idx < 0) || (idx >= SVCRT_SEM_NUM))
    {
        return 0u;
    }
    return dbg_sem_qbefore[idx];
}
volatile uint32 svcrt_diag_sync_dup   = 0u;   /* duplicate waiter registration  */

/* Which object and which task produced the last ghost / timeout wake.
 * Purely descriptive: a counter can only under-report, never invent. */
volatile uint32 dbg_sem_ghost_cnt = 0u;
volatile uint32 dbg_sem_ghost_idx = 0xFFFFFFFFu;
volatile uint32 dbg_sem_ghost_tid = 0u;
volatile uint32 dbg_sem_to_cnt   = 0u;
volatile uint32 dbg_sem_appq_hit = 0u;

/* ------------------------------------------------------------
 * Read-only debug getters (shell "syncinfo").  Pure copies, so a snapshot
 * can never be worse than the caller's own race, and a hung task can be
 * traced to the exact object it sits in.
 * ------------------------------------------------------------ */
uint32 svcrt_sync_sem_num(void)  { return (uint32)SVCRT_SEM_NUM; }
uint32 svcrt_sync_mtx_num(void)  { return (uint32)SVCRT_MTX_NUM; }
uint32 svcrt_sync_cond_num(void) { return (uint32)SVCRT_COND_NUM; }

static int32 sync_dbg_row(uint8 used, int32 creator, int32 owner, int32 count,
                          svcrt_task_t **waiters, svcrt_sync_dbg_row_t *out)
{
    int32 j;
    int32 n = 0;

    out->used        = used;
    out->creator_id  = creator;
    out->owner_id    = owner;
    out->count       = count;
    out->nwait       = 0;
    for(j = 0; j < SVCRT_MAX_SYNC_WAITERS; j++)
    {
        out->waiter_id[j] = 0u;
        if(waiters[j] != 0)
        {
            out->waiter_id[j] = (uint32)(SVCRT_TASK_IDX(waiters[j]) + 1);
            n++;
        }
    }
    out->nwait = n;
    return 0;
}

int32 svcrt_sync_sem_debug(int32 idx, svcrt_sync_dbg_row_t *out)
{
    if((idx < 0) || (idx >= SVCRT_SEM_NUM) || (out == 0))
    {
        return -1;
    }
    return sync_dbg_row(svcrt_sems[idx].used, svcrt_sems[idx].creator_id, 0,
                        svcrt_sems[idx].count, svcrt_sems[idx].waiters, out);
}

int32 svcrt_sync_mtx_debug(int32 idx, svcrt_sync_dbg_row_t *out)
{
    if((idx < 0) || (idx >= SVCRT_MTX_NUM) || (out == 0))
    {
        return -1;
    }
    return sync_dbg_row(svcrt_mtxs[idx].used, svcrt_mtxs[idx].creator_id,
                        (int32)((svcrt_mtxs[idx].owner != 0)
                                ? (SVCRT_TASK_IDX(svcrt_mtxs[idx].owner) + 1) : 0),
                        0, svcrt_mtxs[idx].waiters, out);
}

int32 svcrt_sync_cond_debug(int32 idx, svcrt_sync_dbg_row_t *out)
{
    if((idx < 0) || (idx >= SVCRT_COND_NUM) || (out == 0))
    {
        return -1;
    }
    return sync_dbg_row(svcrt_conds[idx].used, svcrt_conds[idx].creator_id, 0, 0,
                        svcrt_conds[idx].waiters, out);
}

void svcrt_sync_module_init(void)
{
    int32 i, j;
    for(i = 0; i < SVCRT_SEM_NUM; i++)
    {
        svcrt_sems[i].name[0] = 0;
        svcrt_sems[i].count   = 0;
        svcrt_sems[i].used    = 0;
        svcrt_sems[i].creator_id = 0;
        for(j = 0; j < SVCRT_MAX_SYNC_WAITERS; j++)
            svcrt_sems[i].waiters[j] = 0;
    }
    for(i = 0; i < SVCRT_MTX_NUM; i++)
    {
        svcrt_mtxs[i].name[0]       = 0;
        svcrt_mtxs[i].owner         = 0;
        svcrt_mtxs[i].used          = 0;
        svcrt_mtxs[i].orig_priority = 0;
        svcrt_mtxs[i].creator_id    = 0;
        for(j = 0; j < SVCRT_MAX_SYNC_WAITERS; j++)
            svcrt_mtxs[i].waiters[j] = 0;
    }
    for(i = 0; i < SVCRT_COND_NUM; i++)
    {
        svcrt_conds[i].name[0]    = 0;
        svcrt_conds[i].used       = 0;
        svcrt_conds[i].creator_id = 0;
        for(j = 0; j < SVCRT_MAX_SYNC_WAITERS; j++)
            svcrt_conds[i].waiters[j] = 0;
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

/* ------------------------------------------------------------------
 * User-mode waiters (App code enters the kernel through SVC_Handler)
 *
 * App tasks are unprivileged, so every sync call arrives inside SVC_Handler.
 * PendSV can never preempt SVC, therefore "register and wait for the token"
 * cannot finish inside the handler: the real yield only happens in user mode.
 * Policy: a user-mode waiter only registers and returns
 * SVCRT_SYNC_ERR_WOULDBLOCK; the oslib wrapper polls and yields.  A poster
 * that sees the appq marker does not pop the queue entry, it bumps count
 * instead, and the next poll takes it from the fast path.  A token therefore
 * always has one owner: either it sits in count, or it was handed to a
 * concrete ready task.
 * ------------------------------------------------------------------ */
static volatile uint8 svcrt_sync_appq[SVCRT_TASK_MAX_NUM];

uint8 svcrt_sync_in_handler(void)
{
    /* Same rule as the port layer: IPSR != 0 means we are inside an
     * exception (IRQ or SVC).  Reuse svcrt_port_in_isr() so there is
     * only one place that reads IPSR. */
    return svcrt_port_in_isr();
}

static void svcrt_appq_set(svcrt_task_t *p, uint8 v)
{
    int32 i;

    if(p == 0)
    {
        return;
    }
    i = (int32)SVCRT_TASK_IDX(p);
    if((i < 0) || (i >= SVCRT_TASK_MAX_NUM))
    {
        return;
    }
    svcrt_sync_appq[i] = v;
}

static uint8 svcrt_appq_get(svcrt_task_t *p)
{
    int32 i;

    if(p == 0)
    {
        return 0u;
    }
    i = (int32)SVCRT_TASK_IDX(p);
    if((i < 0) || (i >= SVCRT_TASK_MAX_NUM))
    {
        return 0u;
    }
    return svcrt_sync_appq[i];
}

static int32 svcrt_waiters_add(svcrt_task_t **waiters, svcrt_task_t *p_tsk)
{
    int32 j;
    /* One task, one slot.  Registering the same task twice would leave a
     * stale pointer behind after the next remove() clears only the first
     * match, and a later post would then wake a task that is not waiting
     * any more - i.e. hand out a token that was never posted to it. */
    for(j = 0; j < SVCRT_MAX_SYNC_WAITERS; j++)
    {
        if(waiters[j] == p_tsk)
        {
            svcrt_diag_sync_dup++;
            return -2;
        }
    }
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

/* 从等待者列表中取出优先级最高（priority 数值最小）的任务并移除 */
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

/* Look at the queue head without popping it: a user-mode waiter cannot be
 * resumed in place, so a poster / unlocker must ask "is the head a user-mode
 * waiter?" first.  Popping one hands the token to a task nobody will serve. */
static svcrt_task_t *svcrt_waiters_peek(svcrt_task_t **waiters, uint8 only_kernel)
{
    int32 j;
    int32 best = -1;
    uint8 best_pri = 255;

    for(j = 0; j < SVCRT_MAX_SYNC_WAITERS; j++)
    {
        svcrt_task_t *p = waiters[j];

        if(p == 0)
        {
            continue;
        }
        if((only_kernel != 0u) && (svcrt_appq_get(p) != 0u))
        {
            continue;
        }
        if(p->priority <= best_pri)
        {
            best_pri = p->priority;
            best = j;
        }
    }
    return (best < 0) ? 0 : waiters[best];
}

/* 从等待者列表移除指定任务（超时退出时使用），返回 0=已移除，-1=不在列表中 */
static int32 svcrt_waiters_remove(svcrt_task_t **waiters, svcrt_task_t *p_tsk)
{
    int32 j;
    for(j = 0; j < SVCRT_MAX_SYNC_WAITERS; j++)
    {
        if(waiters[j] == p_tsk)
        {
            waiters[j] = 0;
            return 0;
        }
    }
    return -1;
}

/* 按该锁当前的等待者重算持锁者优先级，用于撤销优先级继承提升
 * （超时退出、等待队列满加锁失败等“等待者消失”的场景）。
 * 取持锁者原始优先级与所有等待者优先级中的最高者（数值最小）。
 * @note 多把锁嵌套时的优先级恢复仍不完善（见 docs 中的遗留问题清单）。 */
/* 按“基准优先级 + 该任务持有的所有互斥锁的等待者”重算有效优先级。
 * 基准优先级（base_priority）在任务创建后不再变化，因此不会出现
 * “把被提升后的值当成原始值”导致永久提权的问题；
 * 遍历该任务持有的所有锁，也修掉了多锁场景下按单把锁快照恢复的错乱。 */
static void svcrt_mtx_recalc_task_priority(svcrt_task_t *p_tsk)
{
    uint8 pri;
    int32 i, j;

    if(p_tsk == 0)
    {
        return;
    }

    pri = p_tsk->base_priority;

    for(i = 0; i < SVCRT_MTX_NUM; i++)
    {
        if(svcrt_mtxs[i].used == 0 || svcrt_mtxs[i].owner != p_tsk)
        {
            continue;
        }

        for(j = 0; j < SVCRT_MAX_SYNC_WAITERS; j++)
        {
            svcrt_task_t *p_w = svcrt_mtxs[i].waiters[j];

            if(p_w != 0 && p_w->priority < pri)
            {
                pri = p_w->priority;
            }
        }
    }

    if(p_tsk->priority != pri)
    {
        uint8 old_pri = p_tsk->priority;

        p_tsk->priority = pri;
        svcrt_ready_reprio(SVCRT_TASK_IDX(p_tsk), old_pri);
    }
}

/* 优先级继承的链式传播：
 * A(高) 等 B 持有的锁，而 B 又在等 C 持有的锁时，C 也必须继承 A 的优先级，
 * 否则“优先级反转”只被消掉一层。多轮迭代直到不再变化即收敛
 * （优先级只会向更高处单调收敛，等待关系有限，必然终止）。 */
static void svcrt_mtx_propagate(void)
{
    int32 round, i, j, changed;

    for(round = 0; round < (SVCRT_TASK_MAX_NUM + 1); round++)
    {
        changed = 0;

        for(i = 0; i < SVCRT_MTX_NUM; i++)
        {
            svcrt_task_t *p_owner;

            if(svcrt_mtxs[i].used == 0)
            {
                continue;
            }

            p_owner = svcrt_mtxs[i].owner;
            if(p_owner == 0)
            {
                continue;
            }

            for(j = 0; j < SVCRT_MAX_SYNC_WAITERS; j++)
            {
                svcrt_task_t *p_w = svcrt_mtxs[i].waiters[j];

                if(p_w != 0 && p_owner->priority > p_w->priority)
                {
                    uint8 old_pri = p_owner->priority;

                    p_owner->priority = p_w->priority;
                    svcrt_ready_reprio(SVCRT_TASK_IDX(p_owner), old_pri);
                    changed = 1;
                }
            }
        }

        if(changed == 0)
        {
            break;
        }
    }
}

/* 等待者退出（超时/参数错误/被杀）后回收优先级继承。
 * 这里必须“全量重算所有 owner + 重新传播”，不能只重算该锁的直接 owner：
 * 链式提升会把优先级传递给链上的中间持有者（T1 等 T2 的 M1、T2 又在等
 * T3 的 M2 时 T3 也会被提升），只重算 T2 会让 T3 永久滞留在被提升的优先级。
 * 全量重算先把所有人拉回基准，再按当前等待关系重新建立合法的继承。 */
static void svcrt_mtx_recalc_priority(int32 idx)
{
    int32 i;

    (void)idx;                          /* 全量回收，不依赖具体是哪把锁 */

    for(i = 0; i < SVCRT_MTX_NUM; i++)
    {
        if(svcrt_mtxs[i].used != 0)
        {
            svcrt_mtx_recalc_task_priority(svcrt_mtxs[i].owner);
        }
    }

    svcrt_mtx_propagate();
}

/* 把等待队列里的任务全部唤醒（用于对象被删除等“等待目标已消失”的场景）。
 * reason 置为 SVCRT_WAKE_OBJ_DELETED，等待者据此返回错误码，
 * 而不是永远睡下去（对象删除后队列被清空，谁也唤不醒它们）。 */
static void svcrt_waiters_wake_all(svcrt_task_t **waiters, int32 reason)
{
    int32 j;

    for(j = 0; j < SVCRT_MAX_SYNC_WAITERS; j++)
    {
        if(waiters[j] != 0)
        {
            /* A task already in teardown must not go back on the ready list: the
             * teardown path has just removed it from every queue, so adding it
             * again would corrupt the ready ring. */
            if(waiters[j]->status == SVCRT_TASK_INVALID)
            {
                waiters[j] = 0;
                continue;
            }
            waiters[j]->wait_time   = 0;
            waiters[j]->wake_reason = reason;
            waiters[j]->status      = SVCRT_TASK_READY;
            svcrt_ready_add(SVCRT_TASK_IDX(waiters[j]));
            waiters[j]              = 0;
        }
    }
}

/* 任务下线收尸：把 task_id 从所有同步对象的等待队列摘除；
 * 若它正持有某把互斥锁，则把锁移交给等待者中优先级最高者并唤醒它，
 * 没有等待者则直接释放。
 * 调用方需自行保证临界区（本函数不开关中断）。 */
void svcrt_sync_release_task(int32 task_id)
{
    svcrt_task_t *p_tsk;
    int32 i, j;

    /* A dead task can never come back to poll: the marker has to go with it,
     * or posters keep skipping a slot nobody will ever serve again. */
    svcrt_appq_set(&svcrt_task_table[task_id - 1], 0u);

    for(i = 0; i < SVCRT_COND_NUM; i++)
    {
        if(svcrt_conds[i].used == 0)
        {
            continue;
        }
        for(j = 0; j < SVCRT_MAX_SYNC_WAITERS; j++)
        {
            if(svcrt_conds[i].waiters[j] == p_tsk)
            {
                svcrt_conds[i].waiters[j] = 0;
            }
        }
    }

    for(i = 0; i < SVCRT_SEM_NUM; i++)
    {
        if(svcrt_sems[i].used == 0)
        {
            continue;
        }
        for(j = 0; j < SVCRT_MAX_SYNC_WAITERS; j++)
        {
            if(svcrt_sems[i].waiters[j] == p_tsk)
            {
                svcrt_sems[i].waiters[j] = 0;
            }
        }
    }

    for(i = 0; i < SVCRT_MTX_NUM; i++)
    {
        if(svcrt_mtxs[i].used == 0)
        {
            continue;
        }

        for(j = 0; j < SVCRT_MAX_SYNC_WAITERS; j++)
        {
            if(svcrt_mtxs[i].waiters[j] == p_tsk)
            {
                svcrt_mtxs[i].waiters[j] = 0;
            }
        }

        if(svcrt_mtxs[i].owner == p_tsk)
        {
            svcrt_task_t *p_next = svcrt_waiters_pop_highest(svcrt_mtxs[i].waiters);

            if(p_next != 0)
            {
                svcrt_mtxs[i].owner         = p_next;
                svcrt_mtxs[i].orig_priority = p_next->priority;
                p_next->wait_time   = 0;
                p_next->wake_reason = SVCRT_WAKE_HANDOFF;
                p_next->status      = SVCRT_TASK_READY;
                svcrt_ready_add(SVCRT_TASK_IDX(p_next));
            }
            else
            {
                svcrt_mtxs[i].owner = 0;
            }
        }
        else
        {
            svcrt_mtx_recalc_priority(i);
        }
    }

    /* Objects the dead task created go away with it. Without this an App
     * that creates a mutex (or semaphore) per run leaves it used forever,
     * and a handful of start / stop cycles exhausts the table so the next
     * start cannot create anything. Objects created by the kernel carry
     * id 0 and are left alone. Waiters are only detached, never re-queued:
     * this runs while tasks are being torn down, and putting a task back
     * on the ready ring from here would fight the teardown itself. The
     * tasks that could wait on an App object are that App's own. */
    for(i = 0; i < SVCRT_SEM_NUM; i++)
    {
        if((svcrt_sems[i].used != 0) && (svcrt_sems[i].creator_id == task_id))
        {
            for(j = 0; j < SVCRT_MAX_SYNC_WAITERS; j++)
            {
                svcrt_sems[i].waiters[j] = 0;
            }
            svcrt_sems[i].used       = 0;
            svcrt_sems[i].name[0]    = 0;
            svcrt_sems[i].count      = 0;
            svcrt_sems[i].creator_id = 0;
        }
    }

    for(i = 0; i < SVCRT_MTX_NUM; i++)
    {
        if((svcrt_mtxs[i].used != 0) && (svcrt_mtxs[i].creator_id == task_id))
        {
            /* The object is going away, so the lock is simply dropped.
             * The priority-inheritance rollback is deliberately NOT run:
             * the owner is usually the very task being torn down, and it has
             * already been taken off the ready list - touching the ring for
             * it again corrupts the scheduler. */
            svcrt_mtxs[i].owner         = 0;
            svcrt_mtxs[i].orig_priority = 0;
            for(j = 0; j < SVCRT_MAX_SYNC_WAITERS; j++)
            {
                svcrt_mtxs[i].waiters[j] = 0;
            }
            svcrt_mtxs[i].used       = 0;
            svcrt_mtxs[i].name[0]    = 0;
            svcrt_mtxs[i].creator_id = 0;
        }
    }
}

/* ============================================================
 * 信号量
 * ============================================================ */
int32 svcrt_sem_create_internal(char *name, int32 init_count)
{
    int32 i, j;
    SVCRT_DISABLE_IRQ();
    for(i = 0; i < SVCRT_SEM_NUM; i++)
    {
        if(svcrt_sems[i].used == 0)
        {
            svcrt_sync_copy_name(svcrt_sems[i].name, name);
            svcrt_sems[i].count = init_count;
            svcrt_sems[i].used  = 1;
            /* A reused slot inherits nothing: an empty waiter queue is part
             * of the object state, not an optimisation.  Skipping this lets a
             * post on the new object pop a pointer the old object left behind
             * and hand a token to a task that never waited on it. */
            for(j = 0; j < SVCRT_MAX_SYNC_WAITERS; j++)
            {
                svcrt_sems[i].waiters[j] = 0;
            }
            svcrt_sems[i].creator_id = (svcrt_current_task_id > 0)
                                       ? svcrt_current_task_id : 0;
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
    int32 reason;

    if(SVCRT_SEM_HANDLE_FLAG != (handle & SVCRT_HANDLE_MASK))
        return -1;
    if(idx >= SVCRT_SEM_NUM || svcrt_sems[idx].used == 0)
        return -1;

    p_tsk = svcrt_task_get_current();

    SVCRT_DISABLE_IRQ();
    if(svcrt_sems[idx].count > 0)
    {
        /* Somebody is queued: the token belongs to the queue head.  When the
         * head is the caller itself (a user-mode waiter coming back for it),
         * take it and drop the own registration. */
        svcrt_task_t *p_top = svcrt_waiters_peek(svcrt_sems[idx].waiters, 0u);

        if((p_top == 0) || (p_top == p_tsk))
        {
            svcrt_sems[idx].count--;
            if(p_tsk != 0)
            {
                (void)svcrt_waiters_remove(svcrt_sems[idx].waiters, p_tsk);
                svcrt_appq_set(p_tsk, 0u);
            }
            dbg_sem_takes[idx]++;
            SVCRT_ENABLE_IRQ();
            return 0;
        }
    }

    /* timeout==0 表示“只试一次、不等待”：计数为 0 立即报超时，
     * 不登记等待者、不阻塞（0 不再被解释成永久等待）。 */
    if(timeout_ms == 0)
    {
        /* Try once, and drop a leftover registration of our own: otherwise a
         * poster parks the token in a count nobody will ever take. */
        if(p_tsk != 0)
        {
            (void)svcrt_waiters_remove(svcrt_sems[idx].waiters, p_tsk);
            svcrt_appq_set(p_tsk, 0u);
        }
        SVCRT_ENABLE_IRQ();
        return SVCRT_SYNC_ERR_TIMEOUT;
    }

    if(p_tsk == 0)
    {
        SVCRT_ENABLE_IRQ();
        return -1;
    }

    {
        int32 added = svcrt_waiters_add(svcrt_sems[idx].waiters, p_tsk);

        if((added < 0) && (added != -2))
        {
            SVCRT_ENABLE_IRQ();
            return -1;                      /* waiter queue is full */
        }
        if(added == 0)
        {
            dbg_sem_queued[idx]++;
            dbg_sem_last_waiter[idx] = (uint32)svcrt_current_task_id;
        }
    }

    /* User-mode waiter: register, then return.  The yielding happens in the
     * user-mode wrapper (see the block comment above). */
    if(svcrt_sync_in_handler() != 0u)
    {
        svcrt_appq_set(p_tsk, 1u);
        dbg_sem_appq_hit++;
        SVCRT_ENABLE_IRQ();
        return SVCRT_SYNC_ERR_WOULDBLOCK;
    }

    /* 入队与置 WAIT 必须在同一临界区内完成：中间一旦开中断，ISR 里的 post
     * 就会把唤醒投给一个还没睡下的任务，唤醒随之丢失（见 task.c 的说明）。 */
    svcrt_trace_wait_obj(MDK_TRACE_SVCRT_OBJ(MDK_TRACE_SVCRT_CLASS_SEM, idx));
    reason = svcrt_task_block_in_critical((uint32)timeout_ms);   /* 关中断返回 */

    if(reason < 0)
    {
        /* 未能进入阻塞（内核上下文 / 调度器锁定）：撤销入队并报错 */
        (void)svcrt_waiters_remove(svcrt_sems[idx].waiters, p_tsk);
        SVCRT_ENABLE_IRQ();
        return SVCRT_SYNC_ERR_PARAM;
    }

    /* reason: 0=被 post 显式唤醒（拿到信号量），1=等待超时（未拿到）。
     * 超时必须返回负值并把自己从等待队列摘掉：
     *   - 不返回错误：调用者会以为拿到了信号量，但计数并没有减；
     *   - 不摘除：之后 post 会把一个早已苏醒、正在执行其它代码的任务
     *     当作等待者弹出并置 READY（虚假唤醒）。 */
    if(reason == SVCRT_WAKE_OBJ_DELETED)
    {
        /* 等待期间信号量被删除：队列已被删除方清空，不要再动队列 */
        SVCRT_ENABLE_IRQ();
        return SVCRT_SYNC_ERR_DELETED;
    }

    if(reason == 1)
    {
        (void)svcrt_waiters_remove(svcrt_sems[idx].waiters, p_tsk);
        SVCRT_ENABLE_IRQ();
        return SVCRT_SYNC_ERR_TIMEOUT;
    }

    /* reason==0 only means "something put me back on the ready list". It is
     * not proof that a poster handed me a token: the delay/period path also
     * readies a task and leaves wake_reason at the 0 written on entry. So
     * verify it instead of trusting it: a real post pops me out of the
     * waiter queue, a spurious wake leaves me sitting in it. Reporting
     * success while still queued makes the caller believe it holds a token
     * nobody ever posted (and the count is never decremented). */
    if((reason & SVCRT_WAKE_HANDOFF) == 0)
    {
        /* No poster claimed this task: only the delay/period path and a
         * leftover queue entry can produce such a wake, and claiming success
         * here would invent a post that never happened. */
        svcrt_diag_sync_ghost++;
        dbg_sem_ghost_cnt++;
        dbg_sem_ghost_idx = (uint32)idx;
        dbg_sem_ghost_tid = (uint32)svcrt_current_task_id;
        (void)svcrt_waiters_remove(svcrt_sems[idx].waiters, p_tsk);
        SVCRT_ENABLE_IRQ();
        return SVCRT_SYNC_ERR_TIMEOUT;
    }
    (void)svcrt_waiters_remove(svcrt_sems[idx].waiters, p_tsk);

    SVCRT_ENABLE_IRQ();
    return SVCRT_SYNC_OK;
}

int32 svcrt_sem_post_internal(int32 handle)
{
    int32 idx = handle & SVCRT_HANDLE_RELMASK;
    svcrt_task_t *p_wake;

    if(SVCRT_SEM_HANDLE_FLAG != (handle & SVCRT_HANDLE_MASK))
        return -1;
    if(idx >= SVCRT_SEM_NUM || svcrt_sems[idx].used == 0)
        return -1;

    dbg_sem_posts[idx]++;
    SVCRT_DISABLE_IRQ();
    dbg_sem_qbefore[idx] = 0u;
    {
        int32 jj;
        for(jj = 0; jj < SVCRT_MAX_SYNC_WAITERS; jj++)
        {
            if(svcrt_sems[idx].waiters[jj] != 0)
            {
                dbg_sem_qbefore[idx]++;
            }
        }
    }
    p_wake = svcrt_waiters_peek(svcrt_sems[idx].waiters, 1u);
    if(p_wake != 0)
    {
        (void)svcrt_waiters_remove(svcrt_sems[idx].waiters, p_wake);
    }
    if(p_wake != 0)
    {
        p_wake->wait_time = 0;
        p_wake->wake_reason = SVCRT_WAKE_HANDOFF;
        dbg_sem_woke[idx]   = (uint32)(SVCRT_TASK_IDX(p_wake) + 1);
        dbg_sem_reason[idx] = SVCRT_WAKE_HANDOFF;
        p_wake->status = SVCRT_TASK_READY;
        svcrt_ready_add(SVCRT_TASK_IDX(p_wake));
        mdk_trace_svcrt_obj_signal((uint16)idx, (uint8)MDK_TRACE_SVCRT_CLASS_SEM);
    }
    else
    {
        svcrt_sems[idx].count++;
    }
    SVCRT_ENABLE_IRQ();
    SVCRT_SWITCH_TASK();
    return 0;
}

int32 svcrt_sem_post_from_isr(int32 handle)
{
    /* 中断上下文安全：post 内部不阻塞，仅关中断修改状态并触发切换 */
    return svcrt_sem_post_internal(handle);
}

int32 svcrt_sem_delete_internal(int32 handle)
{
    int32 idx = handle & SVCRT_HANDLE_RELMASK;

    if(SVCRT_SEM_HANDLE_FLAG != (handle & SVCRT_HANDLE_MASK))
        return -1;
    if(idx >= SVCRT_SEM_NUM)
        return -1;

    SVCRT_DISABLE_IRQ();
    /* 先唤醒全部等待者（原因=对象已删除），否则它们会永远阻塞：
     * 对象删除后等待队列被清空，再也没有 post 来唤醒它们。 */
    svcrt_waiters_wake_all(svcrt_sems[idx].waiters, SVCRT_WAKE_OBJ_DELETED);
    svcrt_sems[idx].used       = 0;
    svcrt_sems[idx].name[0]    = 0;
    svcrt_sems[idx].count      = 0;
    svcrt_sems[idx].creator_id = 0;
    SVCRT_ENABLE_IRQ();
    SVCRT_SWITCH_TASK();
    return 0;
}

/* ============================================================
 * 互斥锁
 * ============================================================ */
int32 svcrt_mtx_create_internal(char *name)
{
    int32 i, j;
    SVCRT_DISABLE_IRQ();
    for(i = 0; i < SVCRT_MTX_NUM; i++)
    {
        if(svcrt_mtxs[i].used == 0)
        {
            svcrt_sync_copy_name(svcrt_mtxs[i].name, name);
            svcrt_mtxs[i].owner = 0;
            svcrt_mtxs[i].used  = 1;
            /* See svcrt_sem_create_internal: the queue starts empty. */
            for(j = 0; j < SVCRT_MAX_SYNC_WAITERS; j++)
            {
                svcrt_mtxs[i].waiters[j] = 0;
            }
            svcrt_mtxs[i].creator_id = (svcrt_current_task_id > 0)
                                       ? svcrt_current_task_id : 0;
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
    int32 reason;

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
        /* Same rule as the semaphore: with waiters queued the lock goes to
         * the queue head; only when the head is the caller itself does it
         * drop its registration and take the lock. */
        svcrt_task_t *p_top = svcrt_waiters_peek(svcrt_mtxs[idx].waiters, 0u);

        if((p_top == 0) || (p_top == p_tsk))
        {
            (void)svcrt_waiters_remove(svcrt_mtxs[idx].waiters, p_tsk);
            svcrt_appq_set(p_tsk, 0u);
            svcrt_mtxs[idx].owner         = p_tsk;
            svcrt_mtxs[idx].orig_priority = p_tsk->priority;
            SVCRT_ENABLE_IRQ();
            return 0;
        }
    }

    if(svcrt_mtxs[idx].owner == p_tsk)
    {
        /* 同一任务重复加锁，不支持递归，返回错误 */
        SVCRT_ENABLE_IRQ();
        return -1;
    }

    /* timeout==0 表示“只试一次、不等待”：锁已被他人持有立即报超时。
     * 这里不登记等待者、也不做优先级继承——没建立等待关系就不该提权。 */
    if(timeout_ms == 0)
    {
        /* Non-blocking try.  Drop a leftover registration of our own:
         * the user-mode poll loop closes a timed-out wait with timeout 0,
         * and a stale entry would keep a queue slot busy forever. */
        if(p_tsk != 0)
        {
            (void)svcrt_waiters_remove(svcrt_mtxs[idx].waiters, p_tsk);
            svcrt_appq_set(p_tsk, 0u);
        }
        SVCRT_ENABLE_IRQ();
        return SVCRT_SYNC_ERR_TIMEOUT;
    }

    /* 优先级继承：若持有者优先级低于当前等待者，临时提升持有者优先级以避免优先级反转 */
    if(svcrt_mtxs[idx].owner->priority > p_tsk->priority)
    {
        uint8 old_pri = svcrt_mtxs[idx].owner->priority;

        svcrt_mtxs[idx].owner->priority = p_tsk->priority;
        svcrt_ready_reprio(SVCRT_TASK_IDX(svcrt_mtxs[idx].owner), old_pri);
    }

    if(svcrt_waiters_add(svcrt_mtxs[idx].waiters, p_tsk) == -1)
    {
        /* Queue full: roll the priority inheritance back, otherwise the
         * waiter believes it is queued while nothing will ever wake it. */
        svcrt_mtx_recalc_priority(idx);
        SVCRT_ENABLE_IRQ();
        return SVCRT_SYNC_ERR_PARAM;
    }

    /* User-mode waiter: register only (same rule as sem_wait). */
    if(svcrt_sync_in_handler() != 0u)
    {
        svcrt_appq_set(p_tsk, 1u);
        SVCRT_ENABLE_IRQ();
        return SVCRT_SYNC_ERR_WOULDBLOCK;
    }

    /* 链式传播：被提升的持有者若自己也在等别的锁，那把锁的持有者同样要提升 */
    svcrt_mtx_propagate();

    /* 同 sem_wait：入队与置 WAIT 放在同一临界区内，消除唤醒丢失窗口 */
    svcrt_trace_wait_obj(MDK_TRACE_SVCRT_OBJ(MDK_TRACE_SVCRT_CLASS_MUTEX, idx));
    reason = svcrt_task_block_in_critical((uint32)timeout_ms);   /* 关中断返回 */

    if(reason < 0)
    {
        (void)svcrt_waiters_remove(svcrt_mtxs[idx].waiters, p_tsk);
        svcrt_mtx_recalc_priority(idx);
        SVCRT_ENABLE_IRQ();
        return SVCRT_SYNC_ERR_PARAM;
    }

    if(reason == SVCRT_WAKE_OBJ_DELETED)
    {
        /* 等待期间互斥锁被删除：队列已被删除方清空，只需撤销优先级继承 */
        svcrt_mtx_recalc_priority(idx);
        SVCRT_ENABLE_IRQ();
        return SVCRT_SYNC_ERR_DELETED;
    }

    if(reason == 1)
    {
        /* 竞态：等待超时与 unlock 的“转交”几乎同时发生，unlock 已经把
         * owner 改成本任务。此时锁确实归本任务所有，按成功处理；
         * 若仍按超时返回，这把锁将永远没有 owner（锁泄漏）。 */
        if(svcrt_mtxs[idx].owner == p_tsk)
        {
            SVCRT_ENABLE_IRQ();
            return SVCRT_SYNC_OK;
        }

        /* 未获得锁：摘除等待者并撤销优先级继承提升 */
        (void)svcrt_waiters_remove(svcrt_mtxs[idx].waiters, p_tsk);
        svcrt_mtx_recalc_priority(idx);
        SVCRT_ENABLE_IRQ();
        return SVCRT_SYNC_ERR_TIMEOUT;
    }

    /* Same self-check as svcrt_sem_wait_internal: reason==0 is not proof.
     * A real unlock hands the ownership to me before readying me, so if the
     * lock is not mine the wake was spurious and claiming success would
     * hand out a mutex nobody released. */
    if((svcrt_mtxs[idx].owner != p_tsk) || ((reason & SVCRT_WAKE_HANDOFF) == 0))
    {
        /* Ownership changed behind our back, or nothing ever unlocked on our
         * behalf: either way this is not a lock handover. */
        if(svcrt_mtxs[idx].owner == p_tsk)
        {
            svcrt_diag_sync_ghost++;
        }
        (void)svcrt_waiters_remove(svcrt_mtxs[idx].waiters, p_tsk);
        svcrt_mtx_recalc_priority(idx);
        SVCRT_ENABLE_IRQ();
        return SVCRT_SYNC_ERR_TIMEOUT;
    }

    SVCRT_ENABLE_IRQ();
    return SVCRT_SYNC_OK;
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
        /* 非持有者不能解锁 */
        SVCRT_ENABLE_IRQ();
        return -1;
    }

    /* 先把锁交出去，再重算优先级：
     * 顺序反了会把“即将转交的等待者”也算进自己的继承来源。 */
    p_next = svcrt_waiters_peek(svcrt_mtxs[idx].waiters, 1u);
    if(p_next != 0)
    {
        /* peek skipped the user-mode waiters; hand the lock over properly */
        (void)svcrt_waiters_remove(svcrt_mtxs[idx].waiters, p_next);
        svcrt_mtxs[idx].owner         = p_next;
        svcrt_mtxs[idx].orig_priority = p_next->priority;
        p_next->wait_time = 0;
        p_next->wake_reason = SVCRT_WAKE_HANDOFF;
        p_next->status = SVCRT_TASK_READY;
        svcrt_ready_add(SVCRT_TASK_IDX(p_next));
        /* The lock is not going idle, it is being handed over: from the
         * waiter's point of view this is the acquire that ends its wait. */
        mdk_trace_svcrt_mutex_acquire((uint16)idx);
    }
    else
    {
        svcrt_mtxs[idx].owner = 0;
        mdk_trace_svcrt_mutex_release((uint16)idx);
    }

    /* 撤销优先级继承：按基准优先级 + 仍在持有的其它锁重算
     * （不再用加锁时的快照恢复，多锁场景也不会错乱） */
    svcrt_mtx_recalc_task_priority(p_tsk);
    svcrt_mtx_propagate();

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
    /* 删除锁相当于释放它：持有者按基准优先级（及仍持有的其它锁）重算，
     * 不再依赖加锁时的快照。 */
    if(svcrt_mtxs[idx].owner != 0)
    {
        svcrt_task_t *p_owner = svcrt_mtxs[idx].owner;
        svcrt_mtxs[idx].owner = 0;
        svcrt_mtx_recalc_task_priority(p_owner);
    }
    svcrt_waiters_wake_all(svcrt_mtxs[idx].waiters, SVCRT_WAKE_OBJ_DELETED);
    svcrt_mtxs[idx].used       = 0;
    svcrt_mtxs[idx].name[0]    = 0;
    svcrt_mtxs[idx].creator_id = 0;
    SVCRT_ENABLE_IRQ();
    SVCRT_SWITCH_TASK();
    return 0;
}


/* ============================================================
 * 条件变量
 * ============================================================ */

/* 在「中断已关」的临界区里把互斥量交出去。
 *
 * 条件变量等待必须先释放锁再入队，而且这两步不能被打断：signal 只要落进
 * 中间那个窗口，等待者就会睡在一个已经发生过的事件后面（永久阻塞）。
 * svcrt_mtx_unlock_internal() 在这里不能用 —— 它自己会开中断、还会切任务，
 * 而此刻调用方正拿着临界区。所以这里只要「交出锁 + 重算优先级」这一段，
 * 临界区与任务切换都留给调用方。
 */
static void svcrt_mtx_release_locked(int32 idx, svcrt_task_t *p_owner)
{
    svcrt_task_t *p_next;

    p_next = svcrt_waiters_pop_highest(svcrt_mtxs[idx].waiters);

    if(p_next != 0)
    {
        svcrt_mtxs[idx].owner         = p_next;
        svcrt_mtxs[idx].orig_priority = p_next->priority;
        p_next->wait_time   = 0;
        p_next->wake_reason = SVCRT_WAKE_HANDOFF;
        p_next->status      = SVCRT_TASK_READY;
        svcrt_ready_add(SVCRT_TASK_IDX(p_next));
        /* Same handover as mtx_unlock: the waiter's wait closes here. */
        mdk_trace_svcrt_mutex_acquire((uint16)idx);
    }
    else
    {
        svcrt_mtxs[idx].owner = 0;
        mdk_trace_svcrt_mutex_release((uint16)idx);
    }

    svcrt_mtx_recalc_task_priority(p_owner);
    svcrt_mtx_propagate();
}

int32 svcrt_cond_create_internal(char *name)
{
    int32 i, j;

    SVCRT_DISABLE_IRQ();
    for(i = 0; i < SVCRT_COND_NUM; i++)
    {
        if(svcrt_conds[i].used == 0)
        {
            svcrt_sync_copy_name(svcrt_conds[i].name, name);
            svcrt_conds[i].used       = 1;
            svcrt_conds[i].creator_id = svcrt_current_task_id;
            for(j = 0; j < SVCRT_MAX_SYNC_WAITERS; j++)
            {
                svcrt_conds[i].waiters[j] = 0;
            }
            SVCRT_ENABLE_IRQ();
            return (int32)(SVCRT_COND_HANDLE_FLAG | (uint32)i);
        }
    }
    SVCRT_ENABLE_IRQ();
    return -1;                  /* 对象表满：明确失败，不静默复用 */
}

/* 等待：原子地「释放 mtx + 入队 + 挂起」，醒来后重新获取 mtx。
 * 返回值 0 = 被 signal/broadcast 唤醒，SVCRT_SYNC_ERR_TIMEOUT = 超时。
 * 两种情况返回时都一定持有 mtx（这是 POSIX 的契约，调用方据此写临界区）。 */
int32 svcrt_cond_wait_internal(int32 cond_handle, int32 mtx_handle, int32 timeout_ms)
{
    int32 ci = cond_handle & SVCRT_HANDLE_RELMASK;
    int32 mi = mtx_handle & SVCRT_HANDLE_RELMASK;
    svcrt_task_t *p_tsk;
    int32 reason;

    if(SVCRT_COND_HANDLE_FLAG != (cond_handle & SVCRT_HANDLE_MASK))
        return SVCRT_SYNC_ERR_PARAM;
    if(SVCRT_MTX_HANDLE_FLAG != (mtx_handle & SVCRT_HANDLE_MASK))
        return SVCRT_SYNC_ERR_PARAM;
    if(ci >= SVCRT_COND_NUM || svcrt_conds[ci].used == 0)
        return SVCRT_SYNC_ERR_PARAM;
    if(mi >= SVCRT_MTX_NUM || svcrt_mtxs[mi].used == 0)
        return SVCRT_SYNC_ERR_PARAM;
    /* timeout 0 不能当“不等待”用：block_in_critical 会把 0 当作永久等待，
     * 而条件变量也没法真的“不等待”（释放锁与入队必须一步做完）。
     * 这里垃圾输入就报错，不猜调用方的意图。 */
    if(timeout_ms == 0)
        return SVCRT_SYNC_ERR_PARAM;

    p_tsk = svcrt_task_get_current();
    if(p_tsk == 0)
        return SVCRT_SYNC_ERR_PARAM;

    /* A user-mode condition wait must use the enqueue/poll/abort trio: here
     * the handler can neither yield nor defer "take the mutex back" to user
     * mode, so fail loudly instead of returning a plausible-looking 0. */
    if(svcrt_sync_in_handler() != 0u)
    {
        return SVCRT_SYNC_ERR_PARAM;
    }

    SVCRT_DISABLE_IRQ();

    /* 调用者必须持有该互斥量：POSIX 要求如此，而且「由等待方交出去」
     * 正是条件变量存在的意义。不持有就直接报错，不猜调用者的意图。 */
    if(svcrt_mtxs[mi].owner != p_tsk)
    {
        SVCRT_ENABLE_IRQ();
        return SVCRT_SYNC_ERR_PARAM;
    }

    {
        int32 added = svcrt_waiters_add(svcrt_conds[ci].waiters, p_tsk);

        /* Same rule as everywhere else: -2 = already registered. */
        if((added < 0) && (added != -2))
        {
            SVCRT_ENABLE_IRQ();
            return SVCRT_SYNC_ERR_PARAM;    /* 等待者表满 */
        }
    }

    /* 关键的一步：交出锁 + 睡下，全程中断关闭 */
    svcrt_trace_wait_obj(MDK_TRACE_SVCRT_OBJ(MDK_TRACE_SVCRT_CLASS_COND, ci));
    svcrt_mtx_release_locked(mi, p_tsk);
    reason = svcrt_task_block_in_critical((uint32)timeout_ms);

    if(reason < 0)
    {
        /* 内核上下文 / 调度器锁定：撤销入队，并把锁重新拿回来（契约为准） */
        (void)svcrt_waiters_remove(svcrt_conds[ci].waiters, p_tsk);
        SVCRT_ENABLE_IRQ();
        (void)svcrt_mtx_lock_internal(mtx_handle, (int32)(uint32)-1);
        return SVCRT_SYNC_ERR_PARAM;
    }

    if((reason & SVCRT_WAKE_HANDOFF) == 0)
    {
        /* Nothing signalled on our behalf (timeout, or a wake nobody asked
         * for): the task must not stay in the queue either way. */
        if(reason != SVCRT_WAKE_TIMEOUT)
        {
            svcrt_diag_sync_ghost++;
        }
        (void)svcrt_waiters_remove(svcrt_conds[ci].waiters, p_tsk);
    }

    SVCRT_ENABLE_IRQ();

    /* 无论被唤醒、超时还是对象被删，返回前都要重新持有互斥量（契约）。
     * The re-lock result must not be swallowed: if the mutex cannot be taken
     * back, the contract is broken and the waiter would continue believing it
     * owns the lock, which corrupts ownership for everyone after it. */
    if(svcrt_mtx_lock_internal(mtx_handle, (int32)(uint32)-1) != SVCRT_SYNC_OK)
    {
        return SVCRT_SYNC_ERR_PARAM;
    }

    if(reason == SVCRT_WAKE_OBJ_DELETED)
    {
        return SVCRT_SYNC_ERR_DELETED;
    }

    if((reason != SVCRT_WAKE_TIMEOUT) && ((reason & SVCRT_WAKE_HANDOFF) == 0))
    {
        return SVCRT_SYNC_ERR_TIMEOUT;   /* woken, but not by signal/broadcast */
    }

    return (reason == SVCRT_WAKE_TIMEOUT) ? SVCRT_SYNC_ERR_TIMEOUT : SVCRT_SYNC_OK;
}

/* User-mode condition variable, three steps.  signal/broadcast pops the
 * user-mode waiter and readies it (it is polling anyway, and a ready bit is
 * not a token), poll only answers "am I still queued", so no token is
 * involved anywhere in this path. */
int32 svcrt_cond_enqueue_internal(int32 cond_handle)
{
    int32 idx = cond_handle & SVCRT_HANDLE_RELMASK;
    svcrt_task_t *p_tsk;
    int32 added;

    if(SVCRT_COND_HANDLE_FLAG != (cond_handle & SVCRT_HANDLE_MASK))
        return SVCRT_SYNC_ERR_PARAM;
    if(idx >= SVCRT_COND_NUM || svcrt_conds[idx].used == 0)
        return SVCRT_SYNC_ERR_PARAM;

    p_tsk = svcrt_task_get_current();
    if(p_tsk == 0)
        return SVCRT_SYNC_ERR_PARAM;

    SVCRT_DISABLE_IRQ();
    added = svcrt_waiters_add(svcrt_conds[idx].waiters, p_tsk);
    if((added < 0) && (added != -2))
    {
        SVCRT_ENABLE_IRQ();
        return SVCRT_SYNC_ERR_PARAM;
    }
    svcrt_appq_set(p_tsk, 1u);
    SVCRT_ENABLE_IRQ();
    return SVCRT_SYNC_OK;
}

int32 svcrt_cond_poll_internal(int32 cond_handle)
{
    int32 idx = cond_handle & SVCRT_HANDLE_RELMASK;
    svcrt_task_t *p_tsk;
    int32 j;
    int32 found = 0;

    if(SVCRT_COND_HANDLE_FLAG != (cond_handle & SVCRT_HANDLE_MASK))
        return SVCRT_SYNC_ERR_PARAM;
    if(idx >= SVCRT_COND_NUM || svcrt_conds[idx].used == 0)
        return SVCRT_SYNC_ERR_PARAM;

    p_tsk = svcrt_task_get_current();
    if(p_tsk == 0)
        return SVCRT_SYNC_ERR_PARAM;

    SVCRT_DISABLE_IRQ();
    for(j = 0; j < SVCRT_MAX_SYNC_WAITERS; j++)
    {
        if(svcrt_conds[idx].waiters[j] == p_tsk)
        {
            found = 1;
            break;
        }
    }
    if(found == 0)
    {
        /* Popped by signal/broadcast (or never queued): drop the marker */
        svcrt_appq_set(p_tsk, 0u);
    }
    SVCRT_ENABLE_IRQ();
    return (found != 0) ? 0 : 1;
}

int32 svcrt_cond_abort_internal(int32 cond_handle)
{
    int32 idx = cond_handle & SVCRT_HANDLE_RELMASK;
    svcrt_task_t *p_tsk;

    if(SVCRT_COND_HANDLE_FLAG != (cond_handle & SVCRT_HANDLE_MASK))
        return SVCRT_SYNC_ERR_PARAM;
    if(idx >= SVCRT_COND_NUM || svcrt_conds[idx].used == 0)
        return SVCRT_SYNC_ERR_PARAM;

    p_tsk = svcrt_task_get_current();
    if(p_tsk == 0)
        return SVCRT_SYNC_ERR_PARAM;

    SVCRT_DISABLE_IRQ();
    (void)svcrt_waiters_remove(svcrt_conds[idx].waiters, p_tsk);
    svcrt_appq_set(p_tsk, 0u);
    SVCRT_ENABLE_IRQ();
    return SVCRT_SYNC_OK;
}

int32 svcrt_cond_signal_internal(int32 handle)
{
    int32 idx = handle & SVCRT_HANDLE_RELMASK;
    svcrt_task_t *p_tsk;

    if(SVCRT_COND_HANDLE_FLAG != (handle & SVCRT_HANDLE_MASK))
        return -1;
    if(idx >= SVCRT_COND_NUM || svcrt_conds[idx].used == 0)
        return -1;

    SVCRT_DISABLE_IRQ();
    p_tsk = svcrt_waiters_pop_highest(svcrt_conds[idx].waiters);
    if(p_tsk != 0)
    {
        p_tsk->wait_time   = 0;
        p_tsk->wake_reason = SVCRT_WAKE_HANDOFF;
        p_tsk->status      = SVCRT_TASK_READY;
        svcrt_ready_add(SVCRT_TASK_IDX(p_tsk));
        mdk_trace_svcrt_obj_signal((uint16)idx, (uint8)MDK_TRACE_SVCRT_CLASS_COND);
    }
    SVCRT_ENABLE_IRQ();

    /* 没有人等着不是错误（POSIX 语义）：signal 只是「如果有人在等就叫醒
     * 一个」，不计分、不保留。 */
    if(p_tsk != 0)
    {
        SVCRT_SWITCH_TASK();
    }
    return SVCRT_SYNC_OK;
}

int32 svcrt_cond_broadcast_internal(int32 handle)
{
    int32 idx = handle & SVCRT_HANDLE_RELMASK;
    int32 j;
    int32 woke = 0;

    if(SVCRT_COND_HANDLE_FLAG != (handle & SVCRT_HANDLE_MASK))
        return -1;
    if(idx >= SVCRT_COND_NUM || svcrt_conds[idx].used == 0)
        return -1;

    SVCRT_DISABLE_IRQ();
    for(j = 0; j < SVCRT_MAX_SYNC_WAITERS; j++)
    {
        if(svcrt_conds[idx].waiters[j] != 0)
        {
            svcrt_task_t *p_tsk = svcrt_conds[idx].waiters[j];

            svcrt_conds[idx].waiters[j] = 0;
            p_tsk->wait_time   = 0;
            p_tsk->wake_reason = SVCRT_WAKE_HANDOFF;
            p_tsk->status      = SVCRT_TASK_READY;
            svcrt_ready_add(SVCRT_TASK_IDX(p_tsk));
            mdk_trace_svcrt_obj_signal((uint16)idx, (uint8)MDK_TRACE_SVCRT_CLASS_COND);
            woke++;
        }
    }
    SVCRT_ENABLE_IRQ();

    if(woke > 0)
    {
        SVCRT_SWITCH_TASK();
    }
    return SVCRT_SYNC_OK;
}

int32 svcrt_cond_delete_internal(int32 handle)
{
    int32 idx = handle & SVCRT_HANDLE_RELMASK;

    if(SVCRT_COND_HANDLE_FLAG != (handle & SVCRT_HANDLE_MASK))
        return -1;
    if(idx >= SVCRT_COND_NUM || svcrt_conds[idx].used == 0)
        return -1;

    SVCRT_DISABLE_IRQ();
    /* 唤醒全部等待者并告诉它们「对象没了」：不能让它们留在队列里等一个
     * 再也不会有人 signal 的条件变量。 */
    svcrt_waiters_wake_all(svcrt_conds[idx].waiters, SVCRT_WAKE_OBJ_DELETED);
    svcrt_conds[idx].used       = 0;
    svcrt_conds[idx].name[0]    = 0;
    svcrt_conds[idx].creator_id = 0;
    SVCRT_ENABLE_IRQ();
    return SVCRT_SYNC_OK;
}
