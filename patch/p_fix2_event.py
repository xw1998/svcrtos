# -*- coding: utf-8 -*-
"""
修复 7：event 模块整体重写（无临界区 / 永久等待实现错误 / 队列满照睡 / 丢失置位）

原实现问题：
  1) 全程没有临界区：waiting_tasks[] 的登记、set 时的遍历唤醒都是裸操作，
     与 ISR 里的 svcrt_event_set_from_isr 直接竞争；
  2) timeout<=0 时调用 svcrt_task_wait_period_internal()——那是“等到下一个周期”，
     不是“无限等待”；且该函数会把任务当周期性任务处理，与事件语义完全不同；
  3) 等待者数组满时，登记失败却仍然睡下去——set 时遍历不到它，永久阻塞；
  4) 无置位状态：先 set 后 wait 会丢掉这次置位（wait 直接睡到超时）；
  5) set 时只把任务置 READY，没有触发切换（要等下一次 tick 才被调度）。

修复：对齐 sem/mtx 的既有模式
  - 事件对象增加 used / flag 字段：flag 为自动复位式置位标记，wait 先消费已置位；
  - 等待登记 + 置 WAIT 放在同一临界区（复用 svcrt_task_block_in_critical，
    彻底消除丢唤醒窗口）；
  - 队列满 → 返回错误而不是照睡；超时 → 摘除自己并返回 SVCRT_SYNC_ERR_TIMEOUT；
  - 对象删除/任务下线时唤醒并摘除等待者（新增 svcrt_event_release_task）。
"""
import os
import shutil

ROOT = r'D:\工作\git_project\svcrtos_new'
BK = r'C:\Users\14905\OneDrive\Documents\lingxi-claw\20260911-13-47-19-445\backup_fix2'

FILES = ['kernelsrc/include/svcrt_event.h', 'kernelsrc/src/svcrt_event.c',
         'kernelsrc/src/svcrt_task.c']


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


H_STRUCT_OLD = """typedef struct {
    char name[16];
    svcrt_task_t *waiting_tasks[SVCRT_MAX_EVENT_WAITERS];
} svcrt_event_obj_t;"""

H_STRUCT_NEW = """typedef struct {
    char   name[16];
    uint8  used;                                        /* 该事件对象是否已被占用 */
    uint8  flag;                                        /* 自动复位式置位标记：1=已置位 */
    svcrt_task_t *waiting_tasks[SVCRT_MAX_EVENT_WAITERS];
} svcrt_event_obj_t;"""

H_DECL_OLD = """void svcrt_event_set_internal(int32 handle);"""
H_DECL_NEW = """void svcrt_event_set_internal(int32 handle);

/* 任务下线收尸：把任务从所有事件的等待队列中摘除。
 * 调用方需自行保证临界区；一般经 svcrt_task_release_resources() 调用。 */
void svcrt_event_release_task(int32 task_id);"""

C_INIT_OLD = """void svcrt_event_module_init(void)
{
    int32 i, j;
    for(i = 0; i < SVCRT_EVENT_NUM; i++)
    {
        for(j = 0; j < 8; j++)
            svcrt_events[i].name[j] = 0;
        for(j = 0; j < SVCRT_MAX_EVENT_WAITERS; j++)
            svcrt_events[i].waiting_tasks[j] = 0;
    }
}"""

C_INIT_NEW = """static int32 svcrt_event_waiter_add(svcrt_task_t **waiters, svcrt_task_t *p_tsk)
{
    int32 j;
    for(j = 0; j < SVCRT_MAX_EVENT_WAITERS; j++)
    {
        if(waiters[j] == 0)
        {
            waiters[j] = p_tsk;
            return 0;
        }
    }
    return -1;
}

static int32 svcrt_event_waiter_remove(svcrt_task_t **waiters, svcrt_task_t *p_tsk)
{
    int32 j;
    for(j = 0; j < SVCRT_MAX_EVENT_WAITERS; j++)
    {
        if(waiters[j] == p_tsk)
        {
            waiters[j] = 0;
            return 0;
        }
    }
    return -1;
}

void svcrt_event_module_init(void)
{
    int32 i, j;
    for(i = 0; i < SVCRT_EVENT_NUM; i++)
    {
        for(j = 0; j < 16; j++)
            svcrt_events[i].name[j] = 0;
        svcrt_events[i].used = 0;
        svcrt_events[i].flag = 0;
        for(j = 0; j < SVCRT_MAX_EVENT_WAITERS; j++)
            svcrt_events[i].waiting_tasks[j] = 0;
    }
}

/* 任务下线收尸：把任务从所有事件的等待队列摘除（调用方需已持临界区） */
void svcrt_event_release_task(int32 task_id)
{
    svcrt_task_t *p_tsk;
    int32 i;

    if(task_id <= 0 || task_id > svcrt_task_count)
    {
        return;
    }

    p_tsk = &svcrt_task_table[task_id - 1];

    for(i = 0; i < SVCRT_EVENT_NUM; i++)
    {
        if(svcrt_events[i].used == 0)
        {
            continue;
        }
        (void)svcrt_event_waiter_remove(svcrt_events[i].waiting_tasks, p_tsk);
    }
}"""

C_CREATE_OLD = """int32 svcrt_event_create_internal(char *name)
{
    int32 i;
    for(i = 0; i < SVCRT_EVENT_NUM; i++)
    {
        if(svcrt_events[i].name[0] == 0)
        {
            int32 j;
            for(j = 0; j < 15; j++)
            {
                svcrt_events[i].name[j] = name[j];
                if(name[j] == 0)
                    break;
            }
            svcrt_events[i].name[15] = 0;
            return (i | SVCRT_EVENT_HANDLE_FLAG);
        }
    }
    return -1;
}"""

C_CREATE_NEW = """int32 svcrt_event_create_internal(char *name)
{
    int32 i;
    int32 idx = -1;

    if(name == 0)
    {
        return -1;
    }

    SVCRT_DISABLE_IRQ();
    for(i = 0; i < SVCRT_EVENT_NUM; i++)
    {
        if(svcrt_events[i].used == 0)
        {
            int32 j;
            for(j = 0; j < 15; j++)
            {
                svcrt_events[i].name[j] = name[j];
                if(name[j] == 0)
                    break;
            }
            svcrt_events[i].name[15]  = 0;
            svcrt_events[i].used      = 1;
            svcrt_events[i].flag      = 0;
            for(j = 0; j < SVCRT_MAX_EVENT_WAITERS; j++)
            {
                svcrt_events[i].waiting_tasks[j] = 0;
            }
            idx = i;
            break;
        }
    }
    SVCRT_ENABLE_IRQ();

    if(idx < 0)
    {
        return -1;
    }
    return (idx | SVCRT_EVENT_HANDLE_FLAG);
}"""

C_WAIT_OLD = """int32 svcrt_event_wait_internal(int32 event_handle, int32 timeout_ms)
{
    int32 idx = event_handle & SVCRT_HANDLE_RELMASK;
    int32 j;
    svcrt_task_t *p_tsk;

    if(idx >= SVCRT_EVENT_NUM)
        return -1;

    p_tsk = svcrt_task_get_current();
    if(p_tsk == 0)
        return -1;

    for(j = 0; j < SVCRT_MAX_EVENT_WAITERS; j++)
    {
        if(svcrt_events[idx].waiting_tasks[j] == 0)
        {
            svcrt_events[idx].waiting_tasks[j] = p_tsk;
            p_tsk->wake_reason = 0;
            break;
        }
    }

    if(timeout_ms > 0)
    {
        svcrt_task_wait_internal(timeout_ms);
    }
    else
    {
        svcrt_task_wait_period_internal();
    }
    return 0;
}"""

C_WAIT_NEW = """int32 svcrt_event_wait_internal(int32 event_handle, int32 timeout_ms)
{
    int32 idx = event_handle & SVCRT_HANDLE_RELMASK;
    svcrt_task_t *p_tsk;
    int32 reason;

    if(SVCRT_EVENT_HANDLE_FLAG != (event_handle & SVCRT_HANDLE_MASK))
        return -1;
    if(idx >= SVCRT_EVENT_NUM || svcrt_events[idx].used == 0)
        return -1;

    SVCRT_DISABLE_IRQ();

    /* 已经置位过：直接消费掉，不阻塞（否则“先 set 后 wait”会丢掉这次置位） */
    if(svcrt_events[idx].flag != 0u)
    {
        svcrt_events[idx].flag = 0u;
        SVCRT_ENABLE_IRQ();
        return 0;
    }

    p_tsk = svcrt_task_get_current();
    if(p_tsk == 0)
    {
        SVCRT_ENABLE_IRQ();
        return -1;
    }

    /* 等待队列满时必须报错返回：原先只跳过登记仍然睡下去，
     * set 时遍历不到它，该任务会永久阻塞。 */
    if(svcrt_event_waiter_add(svcrt_events[idx].waiting_tasks, p_tsk) < 0)
    {
        SVCRT_ENABLE_IRQ();
        return -1;
    }

    /* 登记与置 WAIT 同处一个临界区（关中断进入、关中断返回），消除丢唤醒窗口 */
    reason = svcrt_task_block_in_critical((uint32)timeout_ms);

    if(reason < 0)
    {
        (void)svcrt_event_waiter_remove(svcrt_events[idx].waiting_tasks, p_tsk);
        SVCRT_ENABLE_IRQ();
        return -1;
    }

    if(reason == SVCRT_WAKE_OBJ_DELETED)
    {
        SVCRT_ENABLE_IRQ();
        return -1;
    }

    if(reason == 1)
    {
        (void)svcrt_event_waiter_remove(svcrt_events[idx].waiting_tasks, p_tsk);
        SVCRT_ENABLE_IRQ();
        return SVCRT_SYNC_ERR_TIMEOUT;
    }

    SVCRT_ENABLE_IRQ();
    return 0;
}"""

C_SET_OLD = """void svcrt_event_set_internal(int32 event_handle)
{
    int32 idx = event_handle & SVCRT_HANDLE_RELMASK;
    int32 j;

    if(idx >= SVCRT_EVENT_NUM)
        return;

    for(j = 0; j < SVCRT_MAX_EVENT_WAITERS; j++)
    {
        if(svcrt_events[idx].waiting_tasks[j] != 0)
        {
            svcrt_events[idx].waiting_tasks[j]->wake_reason = 0;
            svcrt_events[idx].waiting_tasks[j]->status = SVCRT_TASK_READY;
            svcrt_events[idx].waiting_tasks[j] = 0;
        }
    }
}"""

C_SET_NEW = """void svcrt_event_set_internal(int32 event_handle)
{
    int32 idx = event_handle & SVCRT_HANDLE_RELMASK;
    int32 j;

    if(SVCRT_EVENT_HANDLE_FLAG != (event_handle & SVCRT_HANDLE_MASK))
        return;
    if(idx >= SVCRT_EVENT_NUM || svcrt_events[idx].used == 0)
        return;

    SVCRT_DISABLE_IRQ();

    /* 记录置位：没有等待者时留给下一次 wait 消费，避免“先 set 后 wait”丢事件 */
    svcrt_events[idx].flag = 1u;

    for(j = 0; j < SVCRT_MAX_EVENT_WAITERS; j++)
    {
        svcrt_task_t *p_w = svcrt_events[idx].waiting_tasks[j];

        if(p_w != 0)
        {
            p_w->wait_time   = 0;
            p_w->wake_reason = SVCRT_WAKE_NORMAL;
            p_w->status      = SVCRT_TASK_READY;
            svcrt_events[idx].waiting_tasks[j] = 0;
        }
    }

    SVCRT_ENABLE_IRQ();

    /* 原先只置 READY 不触发切换，被唤醒的任务要等到下一次 tick 才运行 */
    SVCRT_SWITCH_TASK();
}"""

REL_OLD = """    state = SVCRT_ENTER_CRITICAL();
    svcrt_sync_release_task(task_id);
    svcrt_mq_release_task(task_id);
    SVCRT_EXIT_CRITICAL(state);"""
REL_NEW = """    state = SVCRT_ENTER_CRITICAL();
    svcrt_sync_release_task(task_id);
    svcrt_mq_release_task(task_id);
    svcrt_event_release_task(task_id);
    SVCRT_EXIT_CRITICAL(state);"""


def main():
    do_backup()

    print('--- svcrt_event.h ---')
    d = Doc('kernelsrc/include/svcrt_event.h', 'gbk')
    d.sub(H_STRUCT_OLD, H_STRUCT_NEW, '事件对象增加 used/flag')
    d.sub(H_DECL_OLD, H_DECL_NEW, '声明 event_release_task')
    d.save()

    print('--- svcrt_event.c ---')
    d = Doc('kernelsrc/src/svcrt_event.c', 'gbk')
    d.sub('#include "svcrt_event.h"', '#include "svcrt_event.h"\n#include "svcrt_hal.h"\n#include "svcrt_cfg.h"',
          '补充必要头文件')
    d.sub(C_INIT_OLD, C_INIT_NEW, '模块初始化 + 等待者辅助 + 收尸')
    d.sub(C_CREATE_OLD, C_CREATE_NEW, 'create 加临界区与 used 标记')
    d.sub(C_WAIT_OLD, C_WAIT_NEW, 'wait 重写')
    d.sub(C_SET_OLD, C_SET_NEW, 'set 重写')
    d.save()

    print('--- svcrt_task.c ---')
    d = Doc('kernelsrc/src/svcrt_task.c', 'gbk')
    d.sub(REL_OLD, REL_NEW, '收尸纳入事件等待队列')
    d.save()
    print('OK')


if __name__ == '__main__':
    main()
