# -*- coding: utf-8 -*-
"""把误插到 sem_wait 的链式传播调用，移到 mtx_lock 的入队之后"""
import os

p = os.path.join(r'D:\工作\git_project\svcrtos_new', 'kernelsrc', 'src', 'svcrt_sync.c')
raw = open(p, 'rb').read()

eol = b'\r\n' if raw.count(b'\r\n') > 0 else b'\n'
print('eol =', repr(eol), 'crlf count =', raw.count(b'\r\n'), 'lf count =', raw.count(b'\n'))


def T(s):
    return s.encode('gbk').replace(b'\n', eol)


REMOVE = T("    /* 链式传播：被提升的持有者若自己也在等别的锁，那把锁的持有者同样要提升 */\n"
           "    svcrt_mtx_propagate();\n\n")
print('remove anchor:', raw.count(REMOVE))
assert raw.count(REMOVE) == 1
raw = raw.replace(REMOVE, b'')

ANCHOR = T("    /* 同 sem_wait：入队与置 WAIT 放在同一临界区内，消除唤醒丢失窗口 */\n")
NEW = T("    /* 链式传播：被提升的持有者若自己也在等别的锁，那把锁的持有者同样要提升 */\n"
        "    svcrt_mtx_propagate();\n\n"
        "    /* 同 sem_wait：入队与置 WAIT 放在同一临界区内，消除唤醒丢失窗口 */\n")
print('add anchor:', raw.count(ANCHOR))
assert raw.count(ANCHOR) == 1
raw = raw.replace(ANCHOR, NEW)

open(p, 'wb').write(raw)
print('DONE')
