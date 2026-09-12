# -*- coding: utf-8 -*-
"""
修复 9：消息队列被唤醒后条件被抢走时丢弃消息/丢接收
修复 10：svcrt_task_block_in_critical 的负超时（无限等待）被当成超大毫秒数

问题9（mq_send/mq_recv）：
  任务被唤醒后，队列空间/消息可能已被其它任务抢走。原实现此时直接返回 -1：
  发送方消息被悄悄丢掉，接收方拿到“失败”但队列其实有数据。
  改为按剩余超时重新排队重试，直到成功或超时。

问题10（block_in_critical）：
  参数是 uint32，调用方传 (uint32)(-1) 表示无限等待；原实现用
  `timeout_ms > 0u` 判断，负值被当成 0xFFFFFFFF 毫秒，MS_TO_TICK 会溢出成
  一个巨大的节拍数——语义上“勉强算无限”，但数值不可控。
  改为先转 int32 再判断，负值/0 一律按无限等待处理。
"""
import os
import re
import shutil

ROOT = r'D:\工作\git_project\svcrtos_new'
BK = r'C:\Users\14905\OneDrive\Documents\lingxi-claw\20260911-13-47-19-445\backup_fix2'

FILES = ['kernelsrc/src/svcrt_mq.c', 'kernelsrc/src/svcrt_task.c']


class Doc(object):
    def __init__(self, rel, enc):
        self.rel = rel
        self.enc = enc
        self.p = os.path.join(ROOT, rel.replace('/', os.sep))
        self.raw = open(self.p, 'rb').read()
        self.nl = b'\r\n' if b'\r\n' in self.raw else b'\n'

    def t(self, s):
        s = s.replace('\n', '\r\n') if self.nl == b'\r\n' else s
        return s.encode(self.enc)

    def sub(self, old, new, label, count=1):
        ob, nb = self.t(old), self.t(new)
        n = self.raw.count(ob)
        assert n == count, '[%s] %s: 期望 %d 次，实际 %d 次' % (self.rel, label, count, n)
        self.raw = self.raw.replace(ob, nb)
        print('  ok:', label)

    def save(self):
        open(self.p, 'wb').write(self.raw)


REMAIN_FN = """/* 计算“被唤醒后重新排队”可用的剩余超时（ms）。
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

int32 svcrt_mq_send_internal("""

SEND_DECL_OLD = """    svcrt_task_t *p_tsk;
    int32 ret = 0;
"""
SEND_DECL_NEW = """    svcrt_task_t *p_tsk;
    int32 ret = 0;
    int32 remain = 0;
    uint32 start_tick = 0u;
    uint32 deadline = 0u;
"""

RECV_DECL_OLD = """    svcrt_mq_obj_t *p_mq;
    svcrt_task_t *p_tsk;
    int32 reason;
"""
RECV_DECL_NEW = """    svcrt_mq_obj_t *p_mq;
    svcrt_task_t *p_tsk;
    int32 reason;
    int32 remain = 0;
    uint32 start_tick = 0u;
    uint32 deadline = 0u;
"""

SEND_ADD_OLD = """    p_tsk = svcrt_task_get_current();
    if(p_tsk == 0 || svcrt_mq_waiter_add(p_mq->send_waiters, p_tsk) < 0)
    {
        SVCRT_ENABLE_IRQ();
        return -1;
    }"""
SEND_ADD_NEW = """    p_tsk = svcrt_task_get_current();
    if(p_tsk == 0)
    {
        SVCRT_ENABLE_IRQ();
        return -1;
    }

    /* 记录起始节拍与超时上限：被唤醒后若空间又被别人抢走，需要按剩余时间重试 */
    start_tick = svcrt_kernel_tick;
    deadline   = (timeout_ms > 0) ? SVCRT_MS_TO_TICK((uint32)timeout_ms) : 0u;

    if(svcrt_mq_waiter_add(p_mq->send_waiters, p_tsk) < 0)
    {
        SVCRT_ENABLE_IRQ();
        return -1;
    }"""

RECV_ADD_OLD = """    p_tsk = svcrt_task_get_current();
    if(p_tsk == 0 || svcrt_mq_waiter_add(p_mq->recv_waiters, p_tsk) < 0)
    {
        SVCRT_ENABLE_IRQ();
        return -1;
    }"""
RECV_ADD_NEW = """    p_tsk = svcrt_task_get_current();
    if(p_tsk == 0)
    {
        SVCRT_ENABLE_IRQ();
        return -1;
    }

    /* 记录起始节拍与超时上限：被唤醒后若消息又被别人取走，需要按剩余时间重试 */
    start_tick = svcrt_kernel_tick;
    deadline   = (timeout_ms > 0) ? SVCRT_MS_TO_TICK((uint32)timeout_ms) : 0u;

    if(svcrt_mq_waiter_add(p_mq->recv_waiters, p_tsk) < 0)
    {
        SVCRT_ENABLE_IRQ();
        return -1;
    }"""

SEND_TAIL_OLD = """    if(p_mq->count >= SVCRT_MQ_DEPTH)
    {
        SVCRT_ENABLE_IRQ();
        return -1;
    }
    svcrt_mq_put(p_mq, buf);"""
SEND_TAIL_NEW = """    /* 被唤醒不等于一定拿到空间：唤醒后空间可能已被其它发送者占走。
     * 原实现此时直接返回 -1，消息被悄悄丢掉；这里按剩余时间重新排队等待。 */
    while(p_mq->count >= SVCRT_MQ_DEPTH)
    {
        remain = svcrt_mq_remain_ms(start_tick, deadline);
        if(remain == 0)
        {
            SVCRT_ENABLE_IRQ();
            return 1;                       /* 超时：未发送成功 */
        }

        if(svcrt_mq_waiter_add(p_mq->send_waiters, p_tsk) < 0)
        {
            SVCRT_ENABLE_IRQ();
            return -1;
        }

        ret = svcrt_task_block_in_critical((uint32)remain);     /* 关中断返回 */

        if(ret != SVCRT_WAKE_NORMAL)
        {
            svcrt_mq_waiter_remove(p_mq->send_waiters, p_tsk);
            SVCRT_ENABLE_IRQ();
            if(ret == SVCRT_WAKE_TIMEOUT)
            {
                return 1;
            }
            return -1;                      /* 对象被删 / 未能阻塞 */
        }
    }
    svcrt_mq_put(p_mq, buf);"""

RECV_TAIL_OLD = """    if(p_mq->count <= 0)
    {
        SVCRT_ENABLE_IRQ();
        return -1;
    }
    svcrt_mq_get(p_mq, buf);"""
RECV_TAIL_NEW = """    /* 被唤醒不等于一定拿到消息：唤醒后消息可能已被其它接收者取走。
     * 原实现此时直接返回 -1（队列里明明有数据却被判失败）；这里按剩余时间重试。 */
    while(p_mq->count <= 0)
    {
        remain = svcrt_mq_remain_ms(start_tick, deadline);
        if(remain == 0)
        {
            SVCRT_ENABLE_IRQ();
            return 1;
        }

        if(svcrt_mq_waiter_add(p_mq->recv_waiters, p_tsk) < 0)
        {
            SVCRT_ENABLE_IRQ();
            return -1;
        }

        reason = svcrt_task_block_in_critical((uint32)remain);  /* 关中断返回 */

        if(reason != SVCRT_WAKE_NORMAL)
        {
            svcrt_mq_waiter_remove(p_mq->recv_waiters, p_tsk);
            SVCRT_ENABLE_IRQ();
            if(reason == SVCRT_WAKE_TIMEOUT)
            {
                return 1;
            }
            return -1;
        }
    }
    svcrt_mq_get(p_mq, buf);"""

BC_OLD = """    p_tsk = &svcrt_task_table[svcrt_current_task_id - 1];
    p_tsk->wake_reason = 0;
    p_tsk->wait_time   = (timeout_ms > 0u) ? (int32)SVCRT_MS_TO_TICK(timeout_ms) : -1;"""
BC_NEW = """    p_tsk = &svcrt_task_table[svcrt_current_task_id - 1];
    p_tsk->wake_reason = 0;
    /* 参数类型是 uint32，调用方用 (uint32)(-1) 表示无限等待；
     * 必须先转成有符号再判断，否则负值会被当成 0xFFFFFFFF 毫秒，
     * MS_TO_TICK 溢出成一个不可控的巨大节拍数。 */
    p_tsk->wait_time   = ((int32)timeout_ms > 0) ? (int32)SVCRT_MS_TO_TICK((uint32)timeout_ms) : -1;"""


def main():
    os.makedirs(BK, exist_ok=True)
    for rel in FILES:
        shutil.copy2(os.path.join(ROOT, rel.replace('/', os.sep)),
                     os.path.join(BK, rel.replace('/', '__')))

    d = Doc('kernelsrc/src/svcrt_mq.c', 'gbk')
    d.sub('int32 svcrt_mq_send_internal(', REMAIN_FN, '新增剩余超时计算')
    d.sub(SEND_DECL_OLD, SEND_DECL_NEW, 'mq_send 增加重试变量')
    d.sub(RECV_DECL_OLD, RECV_DECL_NEW, 'mq_recv 增加重试变量')
    d.sub(SEND_ADD_OLD, SEND_ADD_NEW, 'mq_send 记录起始节拍')
    d.sub(RECV_ADD_OLD, RECV_ADD_NEW, 'mq_recv 记录起始节拍')
    d.sub(SEND_TAIL_OLD, SEND_TAIL_NEW, 'mq_send 重试循环')
    d.sub(RECV_TAIL_OLD, RECV_TAIL_NEW, 'mq_recv 重试循环')
    d.save()

    d = Doc('kernelsrc/src/svcrt_task.c', 'gbk')
    d.sub(BC_OLD, BC_NEW, 'block_in_critical 正确处理负超时')
    d.save()
    print('OK')


if __name__ == '__main__':
    main()
