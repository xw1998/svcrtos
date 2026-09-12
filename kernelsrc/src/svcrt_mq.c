/**
* @brief SVCrtOS 消息队列模块实现
* @details 定长拷贝语义的消息队列：
*          - send：队列未满时拷贝消息并唤醒一个接收者；已满时阻塞（带超时）或立即失败
*          - recv：队列非空时取出一条并唤醒一个发送者；为空时阻塞（带超时）或立即失败
*          - send_from_isr：中断上下文安全，仅入队唤醒，不阻塞、不做上下文切换
*          返回值约定：0=成功，1=超时，-1=错误（句柄非法/队列满且不等待/等待者满等）
*/

#include "svcrt_mq.h"
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

/* 队列操作统一假设：调用者已持有关中断临界区 */
static void svcrt_mq_put(svcrt_mq_obj_t *p_mq, uint32 *p_src)
{
    uint32 *p_dst;
    int32 i;

    p_dst = &p_mq->buf[p_mq->tail * SVCRT_MQ_MSG_WORDS];
    for(i = 0; i < SVCRT_MQ_MSG_WORDS; i++)
        p_dst[i] = p_src[i];
    p_mq->tail = (p_mq->tail + 1) % SVCRT_MQ_DEPTH;
    p_mq->count++;
}

static void svcrt_mq_get(svcrt_mq_obj_t *p_mq, uint32 *p_dst)
{
    uint32 *p_src;
    int32 i;

    p_src = &p_mq->buf[p_mq->head * SVCRT_MQ_MSG_WORDS];
    for(i = 0; i < SVCRT_MQ_MSG_WORDS; i++)
        p_dst[i] = p_src[i];
    p_mq->head = (p_mq->head + 1) % SVCRT_MQ_DEPTH;
    p_mq->count--;
}

static void svcrt_mq_wake_one(svcrt_task_t **waiters)
{
    int32 j;
    for(j = 0; j < SVCRT_MAX_SYNC_WAITERS; j++)
    {
        if(waiters[j] != 0)
        {
            waiters[j]->wait_time = 0;
            waiters[j]->wake_reason = 0;
            waiters[j]->status = SVCRT_TASK_READY;
            waiters[j] = 0;
            return;
        }
    }
}

static int32 svcrt_mq_waiter_add(svcrt_task_t **waiters, svcrt_task_t *p_tsk)
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
            SVCRT_ENABLE_IRQ();
            return (i | SVCRT_MQ_HANDLE_FLAG);
        }
    }
    SVCRT_ENABLE_IRQ();
    return -1;
}

int32 svcrt_mq_send_internal(int32 handle, uint32 *buf, int32 len_words, int32 timeout_ms)
{
    int32 idx = handle & SVCRT_HANDLE_RELMASK;
    svcrt_mq_obj_t *p_mq;
    svcrt_task_t *p_tsk;
    int32 ret = 0;

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
        svcrt_mq_put(p_mq, buf);
        svcrt_mq_wake_one(p_mq->recv_waiters);
        SVCRT_ENABLE_IRQ();
        SVCRT_SWITCH_TASK();
        return 0;
    }

    /* 队列满：不等待则立即失败 */
    if(timeout_ms == 0)
    {
        SVCRT_ENABLE_IRQ();
        return -1;
    }

    p_tsk = svcrt_task_get_current();
    if(p_tsk == 0 || svcrt_mq_waiter_add(p_mq->send_waiters, p_tsk) < 0)
    {
        SVCRT_ENABLE_IRQ();
        return -1;
    }
    /* ע�⣺�˴����ſ��жϡ������������� block_in_critical ������ͬһ�ٽ����ڣ�
     * ���� ISR �Ļ��ѻ�Ͷ��һ����û˯�µ����񣨻��Ѷ�ʧ���� */

    /* 阻塞等待接收者腾出空间（wake_reason: 0=被唤醒 1=超时） */
    ret = svcrt_task_block_in_critical((uint32)timeout_ms);   /* ���жϷ��� */

    if(ret < 0)
    {
        /* δ�ܽ����������ں�������/��������������������� */
        svcrt_mq_waiter_remove(p_mq->send_waiters, p_tsk);
        SVCRT_ENABLE_IRQ();
        return -1;
    }

    if(ret == 1)
    {
        svcrt_mq_waiter_remove(p_mq->send_waiters, p_tsk);
        SVCRT_ENABLE_IRQ();
        return 1;
    }
    /* 被唤醒时空间可能已被他人占用（多个发送者被同时唤醒的场景不存在，
     * 但为稳妥起见仍检查一次；占用则按错误处理） */
    if(p_mq->count >= SVCRT_MQ_DEPTH)
    {
        SVCRT_ENABLE_IRQ();
        return -1;
    }
    svcrt_mq_put(p_mq, buf);
    svcrt_mq_wake_one(p_mq->recv_waiters);
    SVCRT_ENABLE_IRQ();
    SVCRT_SWITCH_TASK();
    return 0;
}

int32 svcrt_mq_recv_internal(int32 handle, uint32 *buf, int32 len_words, int32 timeout_ms)
{
    int32 idx = handle & SVCRT_HANDLE_RELMASK;
    svcrt_mq_obj_t *p_mq;
    svcrt_task_t *p_tsk;
    int32 reason;

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
        svcrt_mq_get(p_mq, buf);
        svcrt_mq_wake_one(p_mq->send_waiters);
        SVCRT_ENABLE_IRQ();
        SVCRT_SWITCH_TASK();
        return 0;
    }

    if(timeout_ms == 0)
    {
        SVCRT_ENABLE_IRQ();
        return -1;
    }

    p_tsk = svcrt_task_get_current();
    if(p_tsk == 0 || svcrt_mq_waiter_add(p_mq->recv_waiters, p_tsk) < 0)
    {
        SVCRT_ENABLE_IRQ();
        return -1;
    }
    /* ͬ mq_send�����ſ��жϣ������ block_in_critical ����ͬ��һ���ٽ��� */

    reason = svcrt_task_block_in_critical((uint32)timeout_ms);   /* ���жϷ��� */

    if(reason < 0)
    {
        svcrt_mq_waiter_remove(p_mq->recv_waiters, p_tsk);
        SVCRT_ENABLE_IRQ();
        return -1;
    }

    if(reason == 1)
    {
        svcrt_mq_waiter_remove(p_mq->recv_waiters, p_tsk);
        SVCRT_ENABLE_IRQ();
        return 1;
    }
    if(p_mq->count <= 0)
    {
        SVCRT_ENABLE_IRQ();
        return -1;
    }
    svcrt_mq_get(p_mq, buf);
    svcrt_mq_wake_one(p_mq->send_waiters);
    SVCRT_ENABLE_IRQ();
    SVCRT_SWITCH_TASK();
    return 0;
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
    svcrt_mq_put(p_mq, (uint32 *)buf);
    svcrt_mq_wake_one(p_mq->recv_waiters);
    SVCRT_ENABLE_IRQ();
    /* 不在 ISR 内做上下文切换：唤醒的任务置 READY 后由下一次调度接管 */
    return 0;
}

int32 svcrt_mq_delete_internal(int32 handle)
{
    int32 idx = handle & SVCRT_HANDLE_RELMASK;
    int32 j;

    if(SVCRT_MQ_HANDLE_FLAG != (handle & SVCRT_HANDLE_MASK))
        return -1;
    if(idx >= SVCRT_MQ_NUM)
        return -1;

    SVCRT_DISABLE_IRQ();
    svcrt_mqs[idx].used    = 0;
    svcrt_mqs[idx].name[0] = 0;
    svcrt_mqs[idx].head    = 0;
    svcrt_mqs[idx].tail    = 0;
    svcrt_mqs[idx].count   = 0;
    for(j = 0; j < SVCRT_MAX_SYNC_WAITERS; j++)
    {
        svcrt_mqs[idx].send_waiters[j] = 0;
        svcrt_mqs[idx].recv_waiters[j] = 0;
    }
    SVCRT_ENABLE_IRQ();
    return 0;
}

#endif /* SVCRT_USE_MQ */
