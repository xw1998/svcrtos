/**
* @brief SVCrtOS 消息队列模块实现
* @details 变长拷贝语义的消息队列（消息 1..SVCRT_MQ_MSG_WORDS 字）：
*          - send：队列未满时拷贝消息并唤醒一个接收者；已满时阻塞（带超时）或立即失败
*          - recv：队列非空时取出一条并唤醒一个发送者；为空时阻塞（带超时）或立即失败
*          - send_from_isr：中断上下文安全，仅入队唤醒，不阻塞、不做上下文切换
*          返回值约定：
*            send：0=成功，负值=超时或错误（句柄非法/队列满且不等待/等待者满等）
*            recv：>0=实际取到的字数，负值=超时或错误
*          拷贝长度一律以调用者声明的 len_words 为准，绝不按槽宽整块拷——
*          调用者缓冲可能只有 len_words 个字，整块拷会读写越界。
*/

#include "svcrt_mq.h"
#include "svcrt_cfg.h"
#include "svcrt_hal.h"

#if (SVCRT_USE_MQ == 1)

static svcrt_mq_obj_t svcrt_mqs[SVCRT_MQ_NUM];

void svcrt_mq_module_init(void)
{
    int32 i, j;
    for(i = 0; i < SVCRT_MQ_NUM; i++)
    {
        svcrt_mqs[i].name[0] = 0;
        svcrt_mqs[i].used    = 0;
        svcrt_mqs[i].creator_id = 0;
        for(j = 0; j < SVCRT_MQ_DEPTH; j++)
        {
            svcrt_mqs[i].msglen[j] = 0;
        }
        svcrt_mqs[i].head    = 0;
        svcrt_mqs[i].tail    = 0;
        svcrt_mqs[i].count   = 0;
        for(j = 0; j < SVCRT_MAX_SYNC_WAITERS; j++)
        {
            svcrt_mqs[i].send_waiters[j] = 0;
            svcrt_mqs[i].recv_waiters[j] = 0;
        }
    }
}

static void svcrt_mq_copy_name(char *dst, char *src)
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

/* 队列操作统一假设：调用者已持有关中断临界区。
 * put：只拷调用者声明的 len 个字，槽内剩余位置补 0（避免把调用者缓冲之外的
 *      内容读进来），实际长度记进 msglen[槽]，供 recv 原样返回。 */
static void svcrt_mq_put(svcrt_mq_obj_t *p_mq, uint32 *p_src, int32 len)
{
    uint32 *p_dst;
    int32 i;

    p_dst = &p_mq->buf[p_mq->tail * SVCRT_MQ_MSG_WORDS];
    for(i = 0; i < SVCRT_MQ_MSG_WORDS; i++)
    {
        p_dst[i] = (i < len) ? p_src[i] : 0u;
    }
    p_mq->msglen[p_mq->tail] = (uint8)len;
    p_mq->tail = (p_mq->tail + 1) % SVCRT_MQ_DEPTH;
    p_mq->count++;
}

/* get：最多拷 max_len 个字到调用者缓冲，返回实际字数。
 * 发送方声明的长度超过 max_len 时按 max_len 截断（调用者缓冲是硬边界）。 */
static int32 svcrt_mq_get(svcrt_mq_obj_t *p_mq, uint32 *p_dst, int32 max_len)
{
    uint32 *p_src;
    int32 n;
    int32 i;

    p_src = &p_mq->buf[p_mq->head * SVCRT_MQ_MSG_WORDS];
    n = (int32)p_mq->msglen[p_mq->head];
    if(n > max_len)
    {
        n = max_len;
    }
    for(i = 0; i < n; i++)
    {
        p_dst[i] = p_src[i];
    }
    p_mq->head = (p_mq->head + 1) % SVCRT_MQ_DEPTH;
    p_mq->count--;
    return n;
}

static void svcrt_mq_wake_one(svcrt_task_t **waiters)
{
    int32 j;
    for(j = 0; j < SVCRT_MAX_SYNC_WAITERS; j++)
    {
        if(waiters[j] != 0)
        {
            waiters[j]->wait_time = 0;
            waiters[j]->wake_reason = SVCRT_WAKE_HANDOFF;
            waiters[j]->status = SVCRT_TASK_READY;
            svcrt_ready_add(SVCRT_TASK_IDX(waiters[j]));
            waiters[j] = 0;
            return;
        }
    }
}

static int32 svcrt_mq_waiter_add(svcrt_task_t **waiters, svcrt_task_t *p_tsk)
{
    int32 j;

    /* Same rule as svcrt_waiters_add: one task, one slot.  A user-mode
     * waiter re-enters on every poll, so a duplicate must be recognised
     * instead of filling the queue. */
    for(j = 0; j < SVCRT_MAX_SYNC_WAITERS; j++)
    {
        if(waiters[j] == p_tsk)
        {
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

/* 从等待列表中摘除当前任务（超时唤醒时由接收路径清理） */
static void svcrt_mq_waiter_remove(svcrt_task_t **waiters, svcrt_task_t *p_tsk)
{
    int32 j;
    for(j = 0; j < SVCRT_MAX_SYNC_WAITERS; j++)
    {
        if(waiters[j] == p_tsk)
        {
            waiters[j] = 0;
        }
    }
}

/* 唤醒等待队列里的全部任务（对象被删除时用）：
 * 原因置 SVCRT_WAKE_OBJ_DELETED，让等待者返回错误而不是永远睡下去。 */
static void svcrt_mq_wake_all(svcrt_task_t **waiters, int32 reason)
{
    int32 j;

    for(j = 0; j < SVCRT_MAX_SYNC_WAITERS; j++)
    {
        if(waiters[j] != 0)
        {
            /* Same reason as the sync flusher: never re-queue a task that is
             * already being torn down. */
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

/* 任务下线收尸：把任务从所有消息队列的收/发等待队列中摘除。
 * 调用方需自行保证临界区（本函数不开关中断）。 */
void svcrt_mq_release_task(int32 task_id)
{
    svcrt_task_t *p_tsk;
    int32 i, j;

    if(task_id <= 0 || task_id > svcrt_task_count)
    {
        return;
    }

    p_tsk = &svcrt_task_table[task_id - 1];

    for(i = 0; i < SVCRT_MQ_NUM; i++)
    {
        if(svcrt_mqs[i].used == 0)
        {
            continue;
        }

        for(j = 0; j < SVCRT_MAX_SYNC_WAITERS; j++)
        {
            if(svcrt_mqs[i].send_waiters[j] == p_tsk)
            {
                svcrt_mqs[i].send_waiters[j] = 0;
            }
            if(svcrt_mqs[i].recv_waiters[j] == p_tsk)
            {
                svcrt_mqs[i].recv_waiters[j] = 0;
            }
        }
    }

    /* Queues the dead task created go away with it, for the same reason as
     * the mutexes and events: the table is small and start / stop cycles
     * would otherwise drain it. Kernel created queues carry id 0 and stay.
     * Waiters are only detached, never re-queued: this runs inside teardown. */
    for(i = 0; i < SVCRT_MQ_NUM; i++)
    {
        if((svcrt_mqs[i].used != 0) && (svcrt_mqs[i].creator_id == task_id))
        {
            for(j = 0; j < SVCRT_MAX_SYNC_WAITERS; j++)
            {
                svcrt_mqs[i].send_waiters[j] = 0;
                svcrt_mqs[i].recv_waiters[j] = 0;
            }
            svcrt_mqs[i].used       = 0;
            svcrt_mqs[i].name[0]    = 0;
            svcrt_mqs[i].head       = 0;
            svcrt_mqs[i].tail       = 0;
            svcrt_mqs[i].count      = 0;
            svcrt_mqs[i].creator_id = 0;
            for(j = 0; j < SVCRT_MQ_DEPTH; j++)
            {
                svcrt_mqs[i].msglen[j] = 0;
            }
        }
    }
}

int32 svcrt_mq_create_internal(char *name)
{
    int32 i;
    SVCRT_DISABLE_IRQ();
    for(i = 0; i < SVCRT_MQ_NUM; i++)
    {
        if(svcrt_mqs[i].used == 0)
        {
            svcrt_mq_copy_name(svcrt_mqs[i].name, name);
            svcrt_mqs[i].used  = 1;
            svcrt_mqs[i].head  = 0;
            svcrt_mqs[i].tail  = 0;
            svcrt_mqs[i].count = 0;
            svcrt_mqs[i].creator_id = (svcrt_current_task_id > 0)
                                      ? svcrt_current_task_id : 0;
            SVCRT_ENABLE_IRQ();
            return (i | SVCRT_MQ_HANDLE_FLAG);
        }
    }
    SVCRT_ENABLE_IRQ();
    return -1;
}

/* 计算“被唤醒后重新排队”可用的剩余超时（ms）。
 * deadline==0 表示无限等待，返回 -1；已用尽返回 0。 */
static int32 svcrt_mq_remain_ms(uint32 start_tick, uint32 deadline)
{
    uint32 elapsed;

    if(deadline == 0u)
    {
        return -1;
    }

    elapsed = svcrt_kernel_tick - start_tick;
    if(elapsed >= deadline)
    {
        return 0;
    }

    return (int32)((deadline - elapsed) * SVCRT_TICK_PERIOD_US / 1000u);
}

int32 svcrt_mq_send_internal(int32 handle, uint32 *buf, int32 len_words, int32 timeout_ms)
{
    int32 idx = handle & SVCRT_HANDLE_RELMASK;
    svcrt_mq_obj_t *p_mq;
    /* Fetched up front: the timeout==0 branch below already needs it. */
    svcrt_task_t *p_tsk = svcrt_task_get_current();
    int32 ret = 0;
    int32 remain = 0;
    uint32 start_tick = 0u;
    uint32 deadline = 0u;

    if(buf == 0 || len_words <= 0 || len_words > SVCRT_MQ_MSG_WORDS)
        return -1;
    if(SVCRT_MQ_HANDLE_FLAG != (handle & SVCRT_HANDLE_MASK))
        return -1;
    if(idx >= SVCRT_MQ_NUM || svcrt_mqs[idx].used == 0)
        return -1;
    p_mq = &svcrt_mqs[idx];

    SVCRT_DISABLE_IRQ();
    if(p_mq->count < SVCRT_MQ_DEPTH)
    {
        svcrt_mq_put(p_mq, buf, len_words);
        svcrt_mq_wake_one(p_mq->recv_waiters);
        SVCRT_ENABLE_IRQ();
        SVCRT_SWITCH_TASK();
        return 0;
    }

    /* 队列满：不等待则立即失败 */
    if(timeout_ms == 0)
    {
        /* Non-blocking try.  Drop a leftover registration of our own:
         * the user-mode poll loop closes a timed-out wait with timeout 0,
         * and a stale entry would keep a queue slot busy forever. */
        if(p_tsk != 0)
        {
            svcrt_mq_waiter_remove(p_mq->send_waiters, p_tsk);
        }
        SVCRT_ENABLE_IRQ();
        return -1;
    }

    p_tsk = svcrt_task_get_current();
    if(p_tsk == 0)
    {
        SVCRT_ENABLE_IRQ();
        return -1;
    }

    /* 记录起始节拍与超时上限：被唤醒后若空间又被别人抢走，需要按剩余时间重试 */
    start_tick = svcrt_kernel_tick;
    deadline   = (timeout_ms > 0) ? SVCRT_MS_TO_TICK((uint32)timeout_ms) : 0u;

    {
        int32 added = svcrt_mq_waiter_add(p_mq->send_waiters, p_tsk);

        if((added < 0) && (added != -2))
        {
            SVCRT_ENABLE_IRQ();
            return -1;              /* waiter queue is full */
        }
    }
    /* 注意：此处不放开中断——入队与下面的 block_in_critical 必须在同一临界区内，
     * 否则 ISR 的唤醒会投给一个还没睡下的任务（唤醒丢失）。 */

    /* 阻塞等待接收者腾出空间（wake_reason: 0=被唤醒 1=超时） */
    ret = svcrt_task_block_in_critical((uint32)timeout_ms);
    /* User-mode waiter: register only, yield in the user wrapper */
    if(svcrt_sync_in_handler() != 0u)
    {
        SVCRT_ENABLE_IRQ();
        return SVCRT_SYNC_ERR_WOULDBLOCK;
    }
   /* 关中断返回 */

    if(ret < 0)
    {
        /* 未能进入阻塞（内核上下文/调度器锁定）：撤销入队 */
        svcrt_mq_waiter_remove(p_mq->send_waiters, p_tsk);
        SVCRT_ENABLE_IRQ();
        return -1;
    }

    if(ret == SVCRT_WAKE_OBJ_DELETED)
    {
        /* 等待期间消息队列被删除：队列已被删除方清空，直接返回失败 */
        SVCRT_ENABLE_IRQ();
        return -1;
    }

    if(ret == SVCRT_WAKE_TIMEOUT)
    {
        svcrt_mq_waiter_remove(p_mq->send_waiters, p_tsk);
        SVCRT_ENABLE_IRQ();
        return -1;                          /* 超时未发送成功 */
    }
    /* 被唤醒时空间可能已被他人占用（多个发送者被同时唤醒的场景不存在，
     * 但为稳妥起见仍检查一次；占用则按错误处理） */
    /* 被唤醒不等于一定拿到空间：唤醒后空间可能已被其它发送者占走。
     * 原实现此时直接返回 -1，消息被悄悄丢掉；这里按剩余时间重新排队等待。 */
    while(p_mq->count >= SVCRT_MQ_DEPTH)
    {
        remain = svcrt_mq_remain_ms(start_tick, deadline);
        if(remain == 0)
        {
            SVCRT_ENABLE_IRQ();
            return -1;                      /* 超时：未发送成功 */
        }

        {
            int32 added = svcrt_mq_waiter_add(p_mq->send_waiters, p_tsk);

            if((added < 0) && (added != -2))
            {
                SVCRT_ENABLE_IRQ();
                return -1;          /* waiter queue is full */
            }
        }

        ret = svcrt_task_block_in_critical((uint32)remain);     /* 关中断返回 */

        if((ret & SVCRT_WAKE_HANDOFF) == 0)
        {
            svcrt_mq_waiter_remove(p_mq->send_waiters, p_tsk);
            SVCRT_ENABLE_IRQ();
            if(ret == SVCRT_WAKE_TIMEOUT)
            {
                return -1;
            }
            return -1;                      /* 对象被删 / 未能阻塞 */
        }
    }
    svcrt_mq_put(p_mq, buf, len_words);
    svcrt_mq_wake_one(p_mq->recv_waiters);
    SVCRT_ENABLE_IRQ();
    SVCRT_SWITCH_TASK();
    return 0;
}

int32 svcrt_mq_recv_internal(int32 handle, uint32 *buf, int32 len_words, int32 timeout_ms)
{
    int32 idx = handle & SVCRT_HANDLE_RELMASK;
    svcrt_mq_obj_t *p_mq;
    /* Fetched up front: the timeout==0 branch below already needs it. */
    svcrt_task_t *p_tsk = svcrt_task_get_current();
    int32 reason;
    int32 remain = 0;
    int32 got = 0;
    uint32 start_tick = 0u;
    uint32 deadline = 0u;

    if(buf == 0 || len_words <= 0 || len_words > SVCRT_MQ_MSG_WORDS)
        return -1;
    if(SVCRT_MQ_HANDLE_FLAG != (handle & SVCRT_HANDLE_MASK))
        return -1;
    if(idx >= SVCRT_MQ_NUM || svcrt_mqs[idx].used == 0)
        return -1;
    p_mq = &svcrt_mqs[idx];

    SVCRT_DISABLE_IRQ();
    if(p_mq->count > 0)
    {
        got = svcrt_mq_get(p_mq, buf, len_words);
        svcrt_mq_wake_one(p_mq->send_waiters);
        SVCRT_ENABLE_IRQ();
        SVCRT_SWITCH_TASK();
        return got;
    }

    if(timeout_ms == 0)
    {
        /* Non-blocking try.  Drop a leftover registration of our own:
         * the user-mode poll loop closes a timed-out wait with timeout 0,
         * and a stale entry would keep a queue slot busy forever. */
        if(p_tsk != 0)
        {
            svcrt_mq_waiter_remove(p_mq->recv_waiters, p_tsk);
        }
        SVCRT_ENABLE_IRQ();
        return -1;
    }

    p_tsk = svcrt_task_get_current();
    if(p_tsk == 0)
    {
        SVCRT_ENABLE_IRQ();
        return -1;
    }

    /* 记录起始节拍与超时上限：被唤醒后若消息又被别人取走，需要按剩余时间重试 */
    start_tick = svcrt_kernel_tick;
    deadline   = (timeout_ms > 0) ? SVCRT_MS_TO_TICK((uint32)timeout_ms) : 0u;

    {
        int32 added = svcrt_mq_waiter_add(p_mq->recv_waiters, p_tsk);

        if((added < 0) && (added != -2))
        {
            SVCRT_ENABLE_IRQ();
            return -1;              /* waiter queue is full */
        }
    }
    /* 同 mq_send：不放开中断，入队与 block_in_critical 必须同处一个临界区 */

    /* Same rule as mq_send: a user-mode waiter only registers here,
     * the yielding happens in the user-mode wrapper. */
    if(svcrt_sync_in_handler() != 0u)
    {
        SVCRT_ENABLE_IRQ();
        return SVCRT_SYNC_ERR_WOULDBLOCK;
    }

    reason = svcrt_task_block_in_critical((uint32)timeout_ms);   /* 关中断返回 */

    if(reason < 0)
    {
        svcrt_mq_waiter_remove(p_mq->recv_waiters, p_tsk);
        SVCRT_ENABLE_IRQ();
        return -1;
    }

    if(reason == SVCRT_WAKE_OBJ_DELETED)
    {
        /* 等待期间消息队列被删除：队列已被删除方清空，直接返回失败 */
        SVCRT_ENABLE_IRQ();
        return -1;
    }

    if(reason == SVCRT_WAKE_TIMEOUT)
    {
        svcrt_mq_waiter_remove(p_mq->recv_waiters, p_tsk);
        SVCRT_ENABLE_IRQ();
        return -1;                          /* 超时未收到消息 */
    }
    /* 被唤醒不等于一定拿到消息：唤醒后消息可能已被其它接收者取走。
     * 原实现此时直接返回 -1（队列里明明有数据却被判失败）；这里按剩余时间重试。 */
    while(p_mq->count <= 0)
    {
        remain = svcrt_mq_remain_ms(start_tick, deadline);
        if(remain == 0)
        {
            SVCRT_ENABLE_IRQ();
            return -1;                      /* 超时未收到消息 */
        }

        {
            int32 added = svcrt_mq_waiter_add(p_mq->recv_waiters, p_tsk);

            if((added < 0) && (added != -2))
            {
                SVCRT_ENABLE_IRQ();
                return -1;          /* waiter queue is full */
            }
        }

        reason = svcrt_task_block_in_critical((uint32)remain);  /* 关中断返回 */

        if((reason & SVCRT_WAKE_HANDOFF) == 0)
        {
            svcrt_mq_waiter_remove(p_mq->recv_waiters, p_tsk);
            SVCRT_ENABLE_IRQ();
            if(reason == SVCRT_WAKE_TIMEOUT)
            {
                return -1;
            }
            return -1;
        }
    }
    got = svcrt_mq_get(p_mq, buf, len_words);
    svcrt_mq_wake_one(p_mq->send_waiters);
    SVCRT_ENABLE_IRQ();
    SVCRT_SWITCH_TASK();
    return got;
}

int32 svcrt_mq_send_from_isr_internal(int32 handle, void *buf, int32 len_words)
{
    int32 idx = handle & SVCRT_HANDLE_RELMASK;
    svcrt_mq_obj_t *p_mq;

    if(buf == 0 || len_words <= 0 || len_words > SVCRT_MQ_MSG_WORDS)
        return -1;
    if(SVCRT_MQ_HANDLE_FLAG != (handle & SVCRT_HANDLE_MASK))
        return -1;
    if(idx >= SVCRT_MQ_NUM || svcrt_mqs[idx].used == 0)
        return -1;
    p_mq = &svcrt_mqs[idx];

    SVCRT_DISABLE_IRQ();
    if(p_mq->count >= SVCRT_MQ_DEPTH)
    {
        SVCRT_ENABLE_IRQ();
        return -1;
    }
    svcrt_mq_put(p_mq, (uint32 *)buf, len_words);
    svcrt_mq_wake_one(p_mq->recv_waiters);
    SVCRT_ENABLE_IRQ();
    /* 不在 ISR 内做上下文切换：唤醒的任务置 READY 后由下一次调度接管 */
    return 0;
}

int32 svcrt_mq_delete_internal(int32 handle)
{
    int32 idx = handle & SVCRT_HANDLE_RELMASK;
    int32 i;

    if(SVCRT_MQ_HANDLE_FLAG != (handle & SVCRT_HANDLE_MASK))
        return -1;
    if(idx >= SVCRT_MQ_NUM)
        return -1;

    SVCRT_DISABLE_IRQ();
    /* 先唤醒所有等待者（原因=对象已删除），否则队列清空后没有人再唤醒它们 */
    svcrt_mq_wake_all(svcrt_mqs[idx].send_waiters, SVCRT_WAKE_OBJ_DELETED);
    svcrt_mq_wake_all(svcrt_mqs[idx].recv_waiters, SVCRT_WAKE_OBJ_DELETED);
    svcrt_mqs[idx].used       = 0;
    svcrt_mqs[idx].name[0]    = 0;
    svcrt_mqs[idx].head       = 0;
    svcrt_mqs[idx].tail       = 0;
    svcrt_mqs[idx].count      = 0;
    svcrt_mqs[idx].creator_id = 0;
    for(i = 0; i < SVCRT_MQ_DEPTH; i++)
    {
        svcrt_mqs[idx].msglen[i] = 0;
    }
    SVCRT_ENABLE_IRQ();
    SVCRT_SWITCH_TASK();
    return 0;
}

#endif /* SVCRT_USE_MQ */
