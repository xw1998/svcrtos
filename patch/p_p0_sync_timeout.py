# -*- coding: utf-8 -*-
"""
P0-2 修复：信号量/互斥锁超时语义错误

问题：
  svcrt_sem_wait_internal / svcrt_mtx_lock_internal 在阻塞返回后无条件 return 0：
    - 超时醒来时调用者以为“已经拿到信号量/锁”，但计数没减、owner 也不是它
      => 信号量计数错乱；互斥锁被“以为持有”后 unlock 必然失败；
    - 超时任务没有被从 waiters[] 摘除，之后 post/unlock 会把一个早已苏醒、
      正在跑别的代码的任务当成等待者弹出并置 READY（虚假唤醒 + 悬垂指针）。
  另外 svcrt_mtx_lock_internal 在“等待队列已满、加锁失败”直接返回 -1，
  却没有撤销此前对持锁者做的优先级继承提升，导致持锁者被永久提升。

修复：
  1) 新增 svcrt_waiters_remove()：把超时任务从等待队列摘除；
  2) 新增 svcrt_mtx_recalc_priority()：按当前等待者重算持锁者优先级
     （撤销优先级继承提升）；
  3) 等待返回后检查 p_tsk->wake_reason（1=超时，沿用 mq.c 的既有约定）：
       - 超时且未获得锁/信号量 => 摘除 + 返回 SVCRT_SYNC_ERR_TIMEOUT(-2)；
       - 互斥锁的“超时但 unlock 已把 owner 转交给自己”的竞态 => 按成功处理，
         否则这把锁会永远没有 owner；
  4) 等待队列满导致加锁失败时撤销优先级提升。

返回码约定与 svcrt.h 文档一致：0=成功，负值=失败/超时。
"""
import os

ROOT = r'D:\工作\git_project\svcrtos_new'
BK = r'C:\Users\14905\OneDrive\Documents\lingxi-claw\20260911-13-47-19-445\backup_p0_fault'

FILES = [
    'kernelsrc/include/svcrt_def.h',
    'kernelsrc/src/svcrt_sync.c',
    'kernelsrc/include/svcrt.h',
]


def do_backup():
    import shutil
    os.makedirs(BK, exist_ok=True)
    for rel in FILES:
        shutil.copy2(os.path.join(ROOT, rel.replace('/', os.sep)),
                     os.path.join(BK, rel.replace('/', '__')))


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


# ---------------------------------------------------------------- svcrt_def.h
DEF_NEW = """#define SVCRT_SEM_HANDLE_FLAG       (0x01300000)"""

DEF_OLD = """/* 同步原语（信号量/互斥锁）返回码：0=成功，负值=失败。
 * 超时必须返回负值——调用方据此判断“本次没有拿到资源”；
 * 若与 0（成功）混为一谈，会出现“以为拿到了信号量、计数却没减”的错乱。 */
#define SVCRT_SYNC_OK             (0)
#define SVCRT_SYNC_ERR_PARAM      (-1)
#define SVCRT_SYNC_ERR_TIMEOUT    (-2)

#define SVCRT_SEM_HANDLE_FLAG       (0x01300000)"""


# ---------------------------------------------------------------- svcrt_sync.c
HELPERS = """/* 从等待者列表移除指定任务（超时退出时使用），返回 0=已移除，-1=不在列表中 */
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
static void svcrt_mtx_recalc_priority(int32 idx)
{
    svcrt_task_t *p_owner = svcrt_mtxs[idx].owner;
    uint8 pri = (uint8)svcrt_mtxs[idx].orig_priority;
    int32 j;

    if(p_owner == 0)
    {
        return;
    }

    for(j = 0; j < SVCRT_MAX_SYNC_WAITERS; j++)
    {
        svcrt_task_t *p_w = svcrt_mtxs[idx].waiters[j];
        if(p_w != 0 && p_w->priority < pri)
        {
            pri = p_w->priority;
        }
    }

    p_owner->priority = pri;
}

/* ============================================================
 * 信号量
 * ============================================================ */"""

SEM_WAIT_OLD = """    p_tsk->wake_reason = 0;
    if(timeout_ms > 0)
        svcrt_task_wait_internal(timeout_ms);
    else
        svcrt_task_block_internal();

    return 0;
}

int32 svcrt_sem_post_internal(int32 handle)"""

SEM_WAIT_NEW = """    p_tsk->wake_reason = 0;
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
}

int32 svcrt_sem_post_internal(int32 handle)"""

MTX_LOCK_ADD_OLD = """    if(svcrt_waiters_add(svcrt_mtxs[idx].waiters, p_tsk) < 0)
    {
        SVCRT_ENABLE_IRQ();
        return -1;
    }
    SVCRT_ENABLE_IRQ();

    p_tsk->wake_reason = 0;
    if(timeout_ms > 0)
        svcrt_task_wait_internal(timeout_ms);
    else
        svcrt_task_block_internal();

    return 0;
}"""

MTX_LOCK_ADD_NEW = """    if(svcrt_waiters_add(svcrt_mtxs[idx].waiters, p_tsk) < 0)
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

SYNC_HDR_OLD = """*          阻塞等待复用 svcrt_task_wait_internal/svcrt_task_block_internal 实现任务挂起与切换。
"""
SYNC_HDR_NEW = """*          阻塞等待复用 svcrt_task_wait_internal/svcrt_task_block_internal 实现任务挂起与切换。
 *          阻塞返回后通过 task->wake_reason 区分唤醒原因（0=被 post/unlock 唤醒，
 *          1=等待超时），超时会把自己从等待队列摘除并返回负值错误码，
 *          绝不把“超时”当成“已获得资源”。
"""


def patch_def():
    d = Doc('kernelsrc/include/svcrt_def.h', 'gbk')
    d.sub(DEF_NEW, DEF_OLD, '新增同步原语返回码')
    d.save()


def patch_sync():
    d = Doc('kernelsrc/src/svcrt_sync.c', 'gbk')
    d.sub('/* ============================================================\n * 信号量\n * ============================================================ */',
          HELPERS, '新增等待者摘除与优先级重算')
    d.sub(SEM_WAIT_OLD, SEM_WAIT_NEW, 'sem_wait 超时语义')
    d.sub(MTX_LOCK_ADD_OLD, MTX_LOCK_ADD_NEW, 'mtx_lock 超时语义')
    d.sub(SYNC_HDR_OLD, SYNC_HDR_NEW, '文件头注释')
    d.save()


def patch_svcrt_h():
    d = Doc('kernelsrc/include/svcrt.h', 'gbk')
    d.sub('* @param timeout 超时时间（ms），负值表示永久等待\n'
          '* @return 0=成功，负值=超时或参数错误\n',
          '* @param timeout 超时时间（ms），非正值（<=0）表示永久等待\n'
          '* @return 0=成功；SVCRT_SYNC_ERR_TIMEOUT(-2)=等待超时（未获得资源）；其它负值=参数错误\n',
          'svcrt.h 等待类接口文档', count=2)
    d.save()


if __name__ == '__main__':
    do_backup()
    print('--- svcrt_def.h ---'); patch_def()
    print('--- svcrt_sync.c ---'); patch_sync()
    print('--- svcrt.h ---'); patch_svcrt_h()
    print('OK')
