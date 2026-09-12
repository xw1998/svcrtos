# -*- coding: utf-8 -*-
"""
修复 1：get_current 破坏调用方临界区（丢唤醒窗口回归）
修复 2：svcrt_fault_record 非嵌套开关中断

问题1：
  svcrt_task_get_current() 无条件 SVCRT_DISABLE_IRQ()/SVCRT_ENABLE_IRQ()，
  而 sem_wait / mtx_lock / mq_send / mq_recv 都是在自己的临界区内调用它。
  那次 ENABLE_IRQ 会把调用方的临界区打开，于是
  「已把自己挂进等待队列、但还没置 WAIT」的窗口重新暴露给 ISR 里的
  post/unlock —— 唤醒被投给一个还没睡下的任务并消耗掉（丢唤醒），
  svcrt_task_block_in_critical() 的“同一临界区”前提被打破。
  读取 svcrt_current_task_id 是单个字的读，本身就是原子的，无需关中断。

问题2：
  svcrt_fault_record() 同样是无条件 DISABLE/ENABLE。它在故障处理路径、
  tick 上下文、以及调度器锁检查里被调用，若调用时本就处于某个临界区内，
  记录完成后会把中断打开，破坏外层临界区（故障路径更危险：之后还可能
  恢复运行别的任务，中间被 SysTick 插入改动任务表）。
  改为 SVCRT_ENTER_CRITICAL()/SVCRT_EXIT_CRITICAL() 保存-恢复语义。
"""
import os
import shutil

ROOT = r'D:\工作\git_project\svcrtos_new'
BK = r'C:\Users\14905\OneDrive\Documents\lingxi-claw\20260911-13-47-19-445\backup_fix2'

FILES = ['kernelsrc/src/svcrt_task.c', 'kernelsrc/src/svcrt_fault.c']


def do_backup():
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


GC_OLD = """svcrt_task_t *svcrt_task_get_current(void)
{
    svcrt_task_t *p_tsk = 0;
    SVCRT_DISABLE_IRQ();
    if(svcrt_current_task_id > 0)
    {
        p_tsk = &svcrt_task_table[svcrt_current_task_id - 1];
    }
    SVCRT_ENABLE_IRQ();
    return p_tsk;
}"""

GC_NEW = """svcrt_task_t *svcrt_task_get_current(void)
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
}"""

FR_OLD = """void svcrt_fault_record(uint32 type, int32 task_id)
{
    SVCRT_DISABLE_IRQ();
    svcrt_faults[svcrt_fault_wr].type    = type;
    svcrt_faults[svcrt_fault_wr].task_id = task_id;
    svcrt_faults[svcrt_fault_wr].tick    = svcrt_kernel_tick;
    svcrt_fault_wr = (svcrt_fault_wr + 1) % SVCRT_FAULT_RECORD_NUM;
    if(svcrt_fault_total < SVCRT_FAULT_RECORD_NUM)
    {
        svcrt_fault_total++;
    }
    SVCRT_ENABLE_IRQ();
}"""

FR_NEW = """void svcrt_fault_record(uint32 type, int32 task_id)
{
    /* 用保存-恢复语义，而不是无条件开关中断：
     * 本函数会从故障处理、tick、调度器锁检查等路径调用，调用时可能已经
     * 处在某个临界区内；无条件 ENABLE 会破坏外层临界区
     * （故障路径尤其危险：记录完还要恢复别的任务运行，中间被中断插入
     *   改动任务表就会出问题）。 */
    uint32 state = SVCRT_ENTER_CRITICAL();

    svcrt_faults[svcrt_fault_wr].type    = type;
    svcrt_faults[svcrt_fault_wr].task_id = task_id;
    svcrt_faults[svcrt_fault_wr].tick    = svcrt_kernel_tick;
    svcrt_fault_wr = (svcrt_fault_wr + 1) % SVCRT_FAULT_RECORD_NUM;
    if(svcrt_fault_total < SVCRT_FAULT_RECORD_NUM)
    {
        svcrt_fault_total++;
    }

    SVCRT_EXIT_CRITICAL(state);
}"""


if __name__ == '__main__':
    do_backup()

    print('--- svcrt_task.c (GBK) ---')
    d = Doc('kernelsrc/src/svcrt_task.c', 'gbk')
    d.sub(GC_OLD, GC_NEW, 'get_current 不再破坏调用方临界区')
    d.save()

    print('--- svcrt_fault.c (UTF-8) ---')
    d = Doc('kernelsrc/src/svcrt_fault.c', 'utf-8')
    d.sub(FR_OLD, FR_NEW, 'fault_record 改为保存-恢复临界区')
    d.save()
    print('OK')
