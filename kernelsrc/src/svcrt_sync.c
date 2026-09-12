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

    p_tsk->priority = pri;
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
                    p_owner->priority = p_w->priority;
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
            waiters[j]->wait_time   = 0;
            waiters[j]->wake_reason = reason;
            waiters[j]->status      = SVCRT_TASK_READY;
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

    if(task_id <= 0 || task_id > svcrt_task_count)
    {
        return;
    }

    p_tsk = &svcrt_task_table[task_id - 1];

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
                p_next->wake_reason = SVCRT_WAKE_NORMAL;
                p_next->status      = SVCRT_TASK_READY;
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
}

/* ============================================================
 * 信号量
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
    int32 reason;

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

    /* 入队与置 WAIT 必须在同一临界区内完成：中间一旦开中断，ISR 里的 post
     * 就会把唤醒投给一个还没睡下的任务，唤醒随之丢失（见 task.c 的说明）。 */
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

    SVCRT_DISABLE_IRQ();
    p_wake = svcrt_waiters_pop_highest(svcrt_sems[idx].waiters);
    if(p_wake != 0)
    {
        p_wake->wait_time = 0;
        p_wake->wake_reason = 0;
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
    svcrt_sems[idx].used    = 0;
    svcrt_sems[idx].name[0] = 0;
    svcrt_sems[idx].count   = 0;
    SVCRT_ENABLE_IRQ();
    SVCRT_SWITCH_TASK();
    return 0;
}

/* ============================================================
 * 互斥锁
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
        svcrt_mtxs[idx].owner         = p_tsk;
        svcrt_mtxs[idx].orig_priority = p_tsk->priority;
        SVCRT_ENABLE_IRQ();
        return 0;
    }

    if(svcrt_mtxs[idx].owner == p_tsk)
    {
        /* 同一任务重复加锁，不支持递归，返回错误 */
        SVCRT_ENABLE_IRQ();
        return -1;
    }

    /* 优先级继承：若持有者优先级低于当前等待者，临时提升持有者优先级以避免优先级反转 */
    if(svcrt_mtxs[idx].owner->priority > p_tsk->priority)
    {
        svcrt_mtxs[idx].owner->priority = p_tsk->priority;
    }

    if(svcrt_waiters_add(svcrt_mtxs[idx].waiters, p_tsk) < 0)
    {
        /* 等待队列已满、加锁失败：必须撤销刚才对持锁者的优先级继承提升，
         * 否则持锁者会被永久提权。 */
        svcrt_mtx_recalc_priority(idx);
        SVCRT_ENABLE_IRQ();
        return SVCRT_SYNC_ERR_PARAM;
    }

    /* 链式传播：被提升的持有者若自己也在等别的锁，那把锁的持有者同样要提升 */
    svcrt_mtx_propagate();

    /* 同 sem_wait：入队与置 WAIT 放在同一临界区内，消除唤醒丢失窗口 */
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
    p_next = svcrt_waiters_pop_highest(svcrt_mtxs[idx].waiters);
    if(p_next != 0)
    {
        svcrt_mtxs[idx].owner         = p_next;
        svcrt_mtxs[idx].orig_priority = p_next->priority;
        p_next->wait_time = 0;
        p_next->wake_reason = 0;
        p_next->status = SVCRT_TASK_READY;
    }
    else
    {
        svcrt_mtxs[idx].owner = 0;
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
    svcrt_mtxs[idx].used    = 0;
    svcrt_mtxs[idx].name[0] = 0;
    SVCRT_ENABLE_IRQ();
    SVCRT_SWITCH_TASK();
    return 0;
}
