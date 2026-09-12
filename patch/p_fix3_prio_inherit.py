# -*- coding: utf-8 -*-
"""
修复 11：优先级继承的三处缺陷

原实现：
  1) 加锁时把“当时的 priority”记进 mtx.orig_priority。若该任务此前已被别的锁
     提升过，这个快照就是被提升后的值 —— 解锁时“恢复”成它，任务被永久提权；
  2) 解锁时用单把锁的快照恢复优先级。任务同时持有两把锁时，解开第一把就会把
     优先级降到快照值，即使它还在等/持第二把锁（多锁场景恢复错乱）；
  3) 没有链式传播：A(高) 等 B 持的锁、B 又在等 C 持的锁时，C 也应继承 A 的
     优先级，原实现只在直接等待关系上提升一层。

修复：
  - TCB 增加 base_priority（创建时设定、之后不变）作为恢复基准；
  - 新增 svcrt_mtx_recalc_task_priority()：按 base_priority 与“该任务持有的
    所有互斥锁的等待者”重算有效优先级；
  - 新增 svcrt_mtx_propagate()：多轮迭代做链式传播，直到不再变化；
  - 解锁路径改为“先摘除/转交等待者 → 再按持有情况重算 → 再传播”；
  - 删除路径不再用快照恢复，改为按基准重算。
"""
import os
import shutil

ROOT = r'D:\工作\git_project\svcrtos_new'
BK = r'C:\Users\14905\OneDrive\Documents\lingxi-claw\20260911-13-47-19-445\backup_fix3'

FILES = ['kernelsrc/include/svcrt_task.h', 'kernelsrc/src/svcrt_cfg.c',
         'kernelsrc/src/svcrt_sync.c']


class Doc(object):
    def __init__(self, rel, enc='gbk'):
        self.rel = rel
        self.enc = enc
        self.p = os.path.join(ROOT, rel.replace('/', os.sep))
        self.raw = open(self.p, 'rb').read()

    def sub(self, old, new, label, count=1):
        for eol in ('\r\n', '\n'):
            o = old.replace('\n', eol).encode(self.enc)
            if self.raw.count(o) == count:
                n = new.replace('\n', eol).encode(self.enc)
                self.raw = self.raw.replace(o, n)
                print('  ok(%s): %s' % (repr(eol), label))
                return
        raise AssertionError('[%s] %s: 未找到匹配' % (self.rel, label))

    def save(self):
        open(self.p, 'wb').write(self.raw)


HELPERS = """/* 按“基准优先级 + 该任务持有的所有互斥锁的等待者”重算有效优先级。
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

/* 兼容旧调用点：按锁 idx 的持有者重算优先级（内部为上面那个全局重算） */
static void svcrt_mtx_recalc_priority(int32 idx)
{
    svcrt_mtx_recalc_task_priority(svcrt_mtxs[idx].owner);
}"""

RECALC_OLD = """static void svcrt_mtx_recalc_priority(int32 idx)
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
}"""

UNLOCK_OLD = """    /* 恢复持有者的原始优先级（撤销之前可能的优先级继承提升） */
    p_tsk->priority = svcrt_mtxs[idx].orig_priority;

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
    SVCRT_ENABLE_IRQ();
    SVCRT_SWITCH_TASK();
    return 0;
}"""

UNLOCK_NEW = """    /* 先把锁交出去，再重算优先级：
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
}"""

DEL_OLD = """    /* 持有者若被继承提升过优先级，删除锁时把它恢复回原始优先级 */
    if(svcrt_mtxs[idx].owner != 0)
    {
        svcrt_mtxs[idx].owner->priority = (uint8)svcrt_mtxs[idx].orig_priority;
        svcrt_mtxs[idx].owner = 0;
    }"""

DEL_NEW = """    /* 删除锁相当于释放它：持有者按基准优先级（及仍持有的其它锁）重算，
     * 不再依赖加锁时的快照。 */
    if(svcrt_mtxs[idx].owner != 0)
    {
        svcrt_task_t *p_owner = svcrt_mtxs[idx].owner;
        svcrt_mtxs[idx].owner = 0;
        svcrt_mtx_recalc_task_priority(p_owner);
    }"""

LOCK_ADD_OLD = """    /* 入队与置 WAIT 必须在同一临界区内完成：中间一旦开中断，ISR 里的 post
     * 就会把唤醒投给一个还没睡下的任务，唤醒随之丢失（见 task.c 的说明）。 */"""
LOCK_ADD_NEW = """    /* 链式传播：被提升的持有者若自己也在等别的锁，那把锁的持有者同样要提升 */
    svcrt_mtx_propagate();

    /* 入队与置 WAIT 必须在同一临界区内完成：中间一旦开中断，ISR 里的 post
     * 就会把唤醒投给一个还没睡下的任务，唤醒随之丢失（见 task.c 的说明）。 */"""


def main():
    os.makedirs(BK, exist_ok=True)
    for rel in FILES:
        shutil.copy2(os.path.join(ROOT, rel.replace('/', os.sep)),
                     os.path.join(BK, rel.replace('/', '__')))

    d = Doc('kernelsrc/include/svcrt_task.h')
    d.sub('    uint8  priority;\n    uint8  shm_attri;',
          '    uint8  priority;                                /* 当前有效优先级（可被继承临时提升） */\n'
          '    uint8  base_priority;                           /* 基准优先级：创建后不变，撤销继承用 */\n'
          '    uint8  shm_attri;',
          'TCB 增加 base_priority')
    d.save()

    d = Doc('kernelsrc/src/svcrt_cfg.c')
    d.sub('    p_task->priority    = priority;',
          '    p_task->priority      = priority;\n'
          '    p_task->base_priority = priority;   /* 基准优先级：撤销优先级继承时的恢复依据 */',
          '创建任务时记录基准优先级')
    d.save()

    d = Doc('kernelsrc/src/svcrt_sync.c')
    d.sub(RECALC_OLD, HELPERS, '新增基准重算与链式传播')
    d.sub(LOCK_ADD_OLD, LOCK_ADD_NEW, '加锁时做链式传播')
    d.sub(UNLOCK_OLD, UNLOCK_NEW, '解锁改为按基准重算')
    d.sub(DEL_OLD, DEL_NEW, '删除锁按基准重算')
    d.save()
    print('OK')


if __name__ == '__main__':
    main()
