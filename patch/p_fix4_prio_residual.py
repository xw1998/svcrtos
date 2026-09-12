# -*- coding: utf-8 -*-
"""
修复 13：优先级继承的两个残留

残留1（传递提升不回收）：
  T1(高) 等 T2 持有的 M1，T2 又在等 T3 持有的 M2 时，链式传播会把 T2、T3
  一起提升到 T1 的优先级。T1 超时（或参数错误/被杀）退出后，原来的处理只
  重算了“该锁的直接 owner”（T2），T3 无人重算 —— 永久滞留在被提升的优先级上。
  修法：等待者退出后的重算改为“全量重算所有 owner + 重新传播”，
  这样所有传递提升都会被拉回基准，再按当前等待关系重新建立合法继承。

残留2（恢复后不复位自身优先级）：
  任务在故障时可能正处于被继承提升的状态。recover_mark 收尸了它与同步对象的
  关系，但没有把自身的 priority 复位回 base_priority，恢复后它会永久带着
  提升来的高优先级运行。修法：recover_mark 里按基准复位自身优先级。
"""
import os
import shutil

ROOT = r'D:\工作\git_project\svcrtos_new'
BK = r'C:\Users\14905\OneDrive\Documents\lingxi-claw\20260911-13-47-19-445\backup_fix4'

FILES = ['kernelsrc/src/svcrt_sync.c', 'kernelsrc/src/svcrt_task.c']


class Doc(object):
    def __init__(self, rel):
        self.rel = rel
        self.p = os.path.join(ROOT, rel.replace('/', os.sep))
        self.raw = open(self.p, 'rb').read()

    def sub(self, old, new, label, count=1):
        for eol in ('\r\n', '\n'):
            o = old.replace('\n', eol).encode('gbk')
            if self.raw.count(o) == count:
                n = new.replace('\n', eol).encode('gbk')
                self.raw = self.raw.replace(o, n)
                print('  ok(%s): %s' % (repr(eol), label))
                return
        raise AssertionError('[%s] %s: 未找到匹配' % (self.rel, label))

    def save(self):
        open(self.p, 'wb').write(self.raw)


RECALC_OLD = """/* 兼容旧调用点：按锁 idx 的持有者重算优先级（内部为上面那个全局重算） */
static void svcrt_mtx_recalc_priority(int32 idx)
{
    svcrt_mtx_recalc_task_priority(svcrt_mtxs[idx].owner);
}"""

RECALC_NEW = """/* 等待者退出（超时/参数错误/被杀）后回收优先级继承。
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
}"""

RM_OLD = """    SVCRT_DISABLE_IRQ();
    p_task->recover_pending = 1;
    p_task->status          = SVCRT_TASK_INVALID;
    SVCRT_ENABLE_IRQ();"""

RM_NEW = """    SVCRT_DISABLE_IRQ();
    /* 自身优先级按基准复位：故障时它可能正被继承提升，
     * 而收尸已把它从所有等待关系里摘除，若不复位，恢复后它会带着
     * 提升来的高优先级一直运行。 */
    p_task->priority        = p_task->base_priority;
    p_task->recover_pending = 1;
    p_task->status          = SVCRT_TASK_INVALID;
    SVCRT_ENABLE_IRQ();"""


def main():
    os.makedirs(BK, exist_ok=True)
    for rel in FILES:
        shutil.copy2(os.path.join(ROOT, rel.replace('/', os.sep)),
                     os.path.join(BK, rel.replace('/', '__')))

    d = Doc('kernelsrc/src/svcrt_sync.c')
    d.sub(RECALC_OLD, RECALC_NEW, '等待者退出改为全量回收继承')
    d.save()

    d = Doc('kernelsrc/src/svcrt_task.c')
    d.sub(RM_OLD, RM_NEW, '故障恢复按基准复位自身优先级')
    d.save()
    print('OK')


if __name__ == '__main__':
    main()
