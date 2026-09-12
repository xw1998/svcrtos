# -*- coding: utf-8 -*-
"""修复 6 收尾：svcrt_loader.c 的任务下线点接入收尸（halt_task 有两处相同代码块）"""
import os

ROOT = r'D:\工作\git_project\svcrtos_new'
p = os.path.join(ROOT, 'kernelsrc', 'src', 'svcrt_loader.c')
raw = open(p, 'rb').read()

HALT_OLD = ("    SVCRT_DISABLE_IRQ();\n"
            "    svcrt_task_table[task_id - 1u].recover_pending = 0u;\n"
            "    svcrt_task_table[task_id - 1u].status          = SVCRT_TASK_INVALID;\n"
            "    SVCRT_ENABLE_IRQ();")

HALT_NEW = ("    /* 收尸：把该任务从同步对象/消息队列的等待队列中摘除，并释放它持有的锁。\n"
            "     * 否则它被踢出调度后，残留的锁与等待登记会牵连其它任务。 */\n"
            "    svcrt_task_release_resources((int32)task_id);\n"
            "\n"
            "    SVCRT_DISABLE_IRQ();\n"
            "    svcrt_task_table[task_id - 1u].recover_pending = 0u;\n"
            "    svcrt_task_table[task_id - 1u].status          = SVCRT_TASK_INVALID;\n"
            "    SVCRT_ENABLE_IRQ();")

HALT_OLD_B = HALT_OLD.encode('utf-8')
HALT_NEW_B = HALT_NEW.encode('utf-8')
n = raw.count(HALT_OLD_B)
assert n == 2, 'halt 锚点匹配 %d 次' % n
raw = raw.replace(HALT_OLD_B, HALT_NEW_B)
print('ok: halt 两处接入收尸')

S_OLD = "    svcrt_task_table[task_id - 1].status          = SVCRT_TASK_INVALID;"
S_NEW = ("    svcrt_task_release_resources(task_id);\n"
         "    svcrt_task_table[task_id - 1].status          = SVCRT_TASK_INVALID;")
S_OLD_B = S_OLD.encode('utf-8')
S_NEW_B = S_NEW.encode('utf-8')
n = raw.count(S_OLD_B)
assert n == 2, '禁用点锚点匹配 %d 次' % n
raw = raw.replace(S_OLD_B, S_NEW_B)
print('ok: 禁用 App 两处接入收尸')

open(p, 'wb').write(raw)
print('DONE')
