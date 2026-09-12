# -*- coding: utf-8 -*-
"""
P1-1 修复：阻塞原语的“唤醒丢失窗口”

问题：
  sem_wait / mtx_lock / mq_send / mq_recv 的阻塞流程是
      DISABLE_IRQ → 入等待队列 → ENABLE_IRQ → 置 wake_reason → 阻塞（内部再关中断置 WAIT）
  在“ENABLE_IRQ 之后、真正置 WAIT 之前”这个窗口里，ISR 里的 post/unlock/mq_put
  会把唤醒投递给一个还没睡下的任务：post 从等待队列弹出它、置 wake_reason=0、
  status=READY，随后该任务自己又执行到“置 WAIT 并切走”——唤醒被吞掉，
  调用者会一直阻塞到超时（无超时则永久阻塞）。

修复：
  新增 svcrt_task_block_in_critical(timeout_ms)：调用约定为“关中断进入、关中断返回”，
  在同一个临界区内完成「置等待参数 + 置 WAIT + 触发切换 + 开中断让 PendSV 生效」，
  然后重新关中断返回，调用方继续在同一临界区内处理超时/摘除队列。
  这样就不存在“已入队但还没睡下”的时间窗口。

改动的调用点：svcrt_sem_wait_internal / svcrt_mtx_lock_internal /
            svcrt_mq_send_internal / svcrt_mq_recv_internal
（原有 svcrt_task_wait_internal / svcrt_task_block_internal 保留给没有“先入队”步骤的
  普通延时与周期等待使用。）

未处理（如实记录）：event 模块的等待仍沿用旧写法，其“永久等待用周期等待实现、
timeout=0 语义相反、无临界区、等待者数组满仍照睡”等问题不在本次改动范围内。
"""
import os
import shutil

ROOT = r'D:\工作\git_project\svcrtos_new'
BK = r'C:\Users\14905\OneDrive\Documents\lingxi-claw\20260911-13-47-19-445\backup_p0_fault'

FILES = ['kernelsrc/include/svcrt_task.h', 'kernelsrc/src/svcrt_task.c',
         'kernelsrc/src/svcrt_sync.c', 'kernelsrc/src/svcrt_mq.c']


def do_backup():
    os.makedirs(BK, exist_ok=True)
    for rel in FILES:
        shutil.copy2(os.path.join(ROOT, rel.replace('/', os.sep)),
                     os.path.join(BK, rel.replace('/', '__')))


class Doc(object):
    def __init__(self, rel, enc='gbk'):
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


# ---------------------------------------------------------------- task.h
TH_OLD = """void svcrt_task_block_internal(void);"""

TH_NEW = """void svcrt_task_block_internal(void);

/**
* @brief 在调用方已持有的临界区内阻塞当前任务（供信号量/互斥锁/消息队列使用）
* @param timeout_ms >0 定时等待（ms）；<=0 无限等待（只能被显式唤醒）
* @return 0=被显式唤醒（已获得资源），1=等待超时，-1=未能进入阻塞（调用方需自行摘除队列）
* @details 调用约定：进入时中断已关，返回时中断仍关（调用方在同一临界区内继续处理等待队列）。
*          与 svcrt_task_wait_internal 的区别：后者会自行开关中断，
*          对“先把自己挂进等待队列、再睡下”的原语来说，那中间存在唤醒丢失窗口
*          （ISR 的唤醒会投给一个还没睡下的任务）。本函数把置状态与切走放在同一临界区内。
*/
int32 svcrt_task_block_in_critical(uint32 timeout_ms);"""

# ---------------------------------------------------------------- task.c
TC_OLD = """void svcrt_task_block_internal(void)
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
}"""

TC_NEW = """void svcrt_task_block_internal(void)
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
    p_tsk->wait_time   = (timeout_ms > 0u) ? (int32)SVCRT_MS_TO_TICK(timeout_ms) : -1;
    p_tsk->status      = SVCRT_TASK_WAIT;

    SVCRT_SWITCH_TASK();                /* 只是置 PendSV pending，此刻中断还关着 */
    SVCRT_ENABLE_IRQ();                 /* 开中断：PendSV 切走与 ISR 唤醒才有机会发生 */
    SVCRT_DISABLE_IRQ();                /* 回到本函数与调用方共同的临界区 */

    reason = p_tsk->wake_reason;
    return reason;
}"""

# ---------------------------------------------------------------- sync.c
SW_OLD = """    if(svcrt_waiters_add(svcrt_sems[idx].waiters, p_tsk) < 0)
    {
        SVCRT_ENABLE_IRQ();
        return -1;
    }
    SVCRT_ENABLE_IRQ();

    p_tsk->wake_reason = 0;
    if(timeout_ms > 0)
        svcrt_task_wait_internal((uint32)timeout_ms);
    else
        svcrt_task_block_internal();

    /* wake_reason: 0=被 post 显式唤醒（拿到信号量），1=等待超时（未拿到）。
     * 超时必须返回负值并把自己从等待队列摘掉：
     *   - 不返回错误：调用者会以为拿到了信号量，但计数并没有减；
     *   - 不摘除：之后 post 会把一个早已苏醒、正在执行其它代码的任务
     *     当作等待者弹出并置 READY（虚假唤醒）。 */
    if(p_tsk->wake_reason == 1)
    {
        SVCRT_DISABLE_IRQ();
        (void)svcrt_waiters_remove(svcrt_sems[idx].waiters, p_tsk);
        SVCRT_ENABLE_IRQ();
        return SVCRT_SYNC_ERR_TIMEOUT;
    }

    return SVCRT_SYNC_OK;
}"""

SW_NEW = """    if(svcrt_waiters_add(svcrt_sems[idx].waiters, p_tsk) < 0)
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
    if(reason == 1)
    {
        (void)svcrt_waiters_remove(svcrt_sems[idx].waiters, p_tsk);
        SVCRT_ENABLE_IRQ();
        return SVCRT_SYNC_ERR_TIMEOUT;
    }

    SVCRT_ENABLE_IRQ();
    return SVCRT_SYNC_OK;
}"""

ML_OLD = """    if(svcrt_waiters_add(svcrt_mtxs[idx].waiters, p_tsk) < 0)
    {
        /* 等待队列已满、加锁失败：必须撤销刚才对持锁者的优先级继承提升，
         * 否则持锁者会被永久提权。 */
        svcrt_mtx_recalc_priority(idx);
        SVCRT_ENABLE_IRQ();
        return SVCRT_SYNC_ERR_PARAM;
    }
    SVCRT_ENABLE_IRQ();

    p_tsk->wake_reason = 0;
    if(timeout_ms > 0)
        svcrt_task_wait_internal((uint32)timeout_ms);
    else
        svcrt_task_block_internal();

    SVCRT_DISABLE_IRQ();

    if(p_tsk->wake_reason == 1)
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
}"""

ML_NEW = """    if(svcrt_waiters_add(svcrt_mtxs[idx].waiters, p_tsk) < 0)
    {
        /* 等待队列已满、加锁失败：必须撤销刚才对持锁者的优先级继承提升，
         * 否则持锁者会被永久提权。 */
        svcrt_mtx_recalc_priority(idx);
        SVCRT_ENABLE_IRQ();
        return SVCRT_SYNC_ERR_PARAM;
    }

    /* 同 sem_wait：入队与置 WAIT 放在同一临界区内，消除唤醒丢失窗口 */
    reason = svcrt_task_block_in_critical((uint32)timeout_ms);   /* 关中断返回 */

    if(reason < 0)
    {
        (void)svcrt_waiters_remove(svcrt_mtxs[idx].waiters, p_tsk);
        svcrt_mtx_recalc_priority(idx);
        SVCRT_ENABLE_IRQ();
        return SVCRT_SYNC_ERR_PARAM;
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
}"""

SEM_SIG_OLD = """int32 svcrt_sem_wait_internal(int32 handle, int32 timeout_ms)
{
    int32 idx = handle & SVCRT_HANDLE_RELMASK;
    svcrt_task_t *p_tsk;"""
SEM_SIG_NEW = """int32 svcrt_sem_wait_internal(int32 handle, int32 timeout_ms)
{
    int32 idx = handle & SVCRT_HANDLE_RELMASK;
    svcrt_task_t *p_tsk;
    int32 reason;"""

MTX_SIG_OLD = """int32 svcrt_mtx_lock_internal(int32 handle, int32 timeout_ms)
{
    int32 idx = handle & SVCRT_HANDLE_RELMASK;
    svcrt_task_t *p_tsk;"""
MTX_SIG_NEW = """int32 svcrt_mtx_lock_internal(int32 handle, int32 timeout_ms)
{
    int32 idx = handle & SVCRT_HANDLE_RELMASK;
    svcrt_task_t *p_tsk;
    int32 reason;"""

# ---------------------------------------------------------------- mq.c
MQ_SEND_A_OLD = """    if(p_tsk == 0 || svcrt_mq_waiter_add(p_mq->send_waiters, p_tsk) < 0)
    {
        SVCRT_ENABLE_IRQ();
        return -1;
    }
    SVCRT_ENABLE_IRQ();"""

MQ_SEND_A_NEW = """    if(p_tsk == 0 || svcrt_mq_waiter_add(p_mq->send_waiters, p_tsk) < 0)
    {
        SVCRT_ENABLE_IRQ();
        return -1;
    }
    /* 注意：此处不放开中断——入队与下面的 block_in_critical 必须在同一临界区内，
     * 否则 ISR 的唤醒会投给一个还没睡下的任务（唤醒丢失）。 */"""

MQ_SEND_B_OLD = """    p_tsk->wake_reason = 0;
    if(timeout_ms > 0)
        svcrt_task_wait_internal(timeout_ms);
    else
        svcrt_task_block_internal();

    SVCRT_DISABLE_IRQ();
    if(p_tsk->wake_reason == 1)
    {
        svcrt_mq_waiter_remove(p_mq->send_waiters, p_tsk);
        SVCRT_ENABLE_IRQ();
        return 1;
    }"""

MQ_SEND_B_NEW = """    ret = svcrt_task_block_in_critical((uint32)timeout_ms);   /* 关中断返回 */

    if(ret < 0)
    {
        /* 未能进入阻塞（内核上下文/调度器锁定）：撤销入队 */
        svcrt_mq_waiter_remove(p_mq->send_waiters, p_tsk);
        SVCRT_ENABLE_IRQ();
        return -1;
    }

    if(ret == 1)
    {
        svcrt_mq_waiter_remove(p_mq->send_waiters, p_tsk);
        SVCRT_ENABLE_IRQ();
        return 1;
    }"""

MQ_RECV_A_OLD = """    if(p_tsk == 0 || svcrt_mq_waiter_add(p_mq->recv_waiters, p_tsk) < 0)
    {
        SVCRT_ENABLE_IRQ();
        return -1;
    }
    SVCRT_ENABLE_IRQ();"""

MQ_RECV_A_NEW = """    if(p_tsk == 0 || svcrt_mq_waiter_add(p_mq->recv_waiters, p_tsk) < 0)
    {
        SVCRT_ENABLE_IRQ();
        return -1;
    }
    /* 同 mq_send：不放开中断，入队与 block_in_critical 必须同处一个临界区 */"""

MQ_RECV_B_OLD = """    p_tsk->wake_reason = 0;
    if(timeout_ms > 0)
        svcrt_task_wait_internal(timeout_ms);
    else
        svcrt_task_block_internal();

    SVCRT_DISABLE_IRQ();
    if(p_tsk->wake_reason == 1)
    {
        svcrt_mq_waiter_remove(p_mq->recv_waiters, p_tsk);
        SVCRT_ENABLE_IRQ();
        return 1;
    }"""

MQ_RECV_B_NEW = """    reason = svcrt_task_block_in_critical((uint32)timeout_ms);   /* 关中断返回 */

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
    }"""

MQ_RECV_SIG_OLD = """int32 svcrt_mq_recv_internal(int32 handle, uint32 *buf, int32 len_words, int32 timeout_ms)
{
    int32 idx = handle & SVCRT_HANDLE_RELMASK;
    svcrt_mq_obj_t *p_mq;
    svcrt_task_t *p_tsk;"""
MQ_RECV_SIG_NEW = """int32 svcrt_mq_recv_internal(int32 handle, uint32 *buf, int32 len_words, int32 timeout_ms)
{
    int32 idx = handle & SVCRT_HANDLE_RELMASK;
    svcrt_mq_obj_t *p_mq;
    svcrt_task_t *p_tsk;
    int32 reason;"""


if __name__ == '__main__':
    do_backup()

    print('--- svcrt_task.h ---')
    d = Doc('kernelsrc/include/svcrt_task.h')
    d.sub(TH_OLD, TH_NEW, '新增 block_in_critical 声明')
    d.save()

    print('--- svcrt_task.c ---')
    d = Doc('kernelsrc/src/svcrt_task.c')
    d.sub(TC_OLD, TC_NEW, '新增 block_in_critical 实现')
    d.save()

    print('--- svcrt_sync.c ---')
    d = Doc('kernelsrc/src/svcrt_sync.c')
    d.sub(SEM_SIG_OLD, SEM_SIG_NEW, 'sem_wait 增加 reason')
    d.sub(SW_OLD, SW_NEW, 'sem_wait 消除窗口')
    d.sub(MTX_SIG_OLD, MTX_SIG_NEW, 'mtx_lock 增加 reason')
    d.sub(ML_OLD, ML_NEW, 'mtx_lock 消除窗口')
    d.save()

    print('--- svcrt_mq.c ---')
    d = Doc('kernelsrc/src/svcrt_mq.c')
    d.sub(MQ_RECV_SIG_OLD, MQ_RECV_SIG_NEW, 'mq_recv 增加 reason')
    d.sub(MQ_SEND_A_OLD, MQ_SEND_A_NEW, 'mq_send 入队后不放开中断')
    d.sub(MQ_SEND_B_OLD, MQ_SEND_B_NEW, 'mq_send 消除窗口')
    d.sub(MQ_RECV_A_OLD, MQ_RECV_A_NEW, 'mq_recv 入队后不放开中断')
    d.sub(MQ_RECV_B_OLD, MQ_RECV_B_NEW, 'mq_recv 消除窗口')
    d.sub('    SVCRT_SWITCH_TASK();\n    return ret;\n}', '    SVCRT_SWITCH_TASK();\n    return 0;\n}', 'mq_send 返回值明确化')
    d.save()
    print('OK')
