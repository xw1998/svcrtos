# -*- coding: utf-8 -*-
"""P0-1 收尾：修正 on_fault 返回值判定 + 同步过期注释"""
import os

ROOT = r'D:\工作\git_project\svcrtos_new'


def patch(rel, pairs):
    p = os.path.join(ROOT, rel.replace('/', os.sep))
    raw = open(p, 'rb').read()
    nl = b'\r\n' if b'\r\n' in raw else b'\n'

    def t(s):
        return (s.replace('\n', '\r\n') if nl == b'\r\n' else s).encode('gbk')

    for old, new, label in pairs:
        ob, nb = t(old), t(new)
        n = raw.count(ob)
        assert n == 1, '[%s] %s: 匹配 %d 次' % (rel, label, n)
        raw = raw.replace(ob, nb)
        print('  ok:', label)
    open(p, 'wb').write(raw)


# 1) svcrt_task.c：只有明确返回 1（已达上限被禁用）才不恢复；
#    返回 -1 表示“不属于任何 App 槽位/驱动区”的内核任务，仍按默认重启处理。
patch('kernelsrc/src/svcrt_task.c', [
    ('        if(svcrt_loader_on_fault(tid) != 0)\n'
     '        {\n'
     '            /* 已被禁用，无需恢复 */\n'
     '        }\n',
     '        if(svcrt_loader_on_fault(tid) == 1)\n'
     '        {\n'
     '            /* 已达连续故障上限、该 App 已被禁用：不重建栈帧，直接从调度中摘除。\n'
     '             * 返回 -1 表示该任务不属于任何 App 槽位/驱动区（内核内置任务），\n'
     '             * 仍按默认策略重启，不能当作“已禁用”处理。 */\n'
     '        }\n',
     'on_fault 判定改为 == 1'),

    (' *          禁用，不再无限重启；内核/中断上下文异常则记录后停机等调试器接管。\n'
     ' *          注意：CFSR/HFSR 保持不清，便于用调试器定位故障原因。\n',
     ' *          禁用，不再无限重启；内核/中断上下文异常则记录后停机等调试器接管。\n'
     ' *          注意：CFSR/HFSR 保持不清，便于用调试器定位故障原因。\n'
     ' * @return 可用于恢复的任务栈指针（非 0 时板级层调用 svcrt_port_resume_task 完成恢复）；\n'
     ' *         0 表示不可恢复，板级层应停机等待调试器。\n',
     'fault handler 返回值注释'),
])

# 2) board.c：同步“经 PendSV 切走”这句已过期的注释
patch('board/stm32f427/svcrt_board.c', [
    (' *          这些向量的处理逻辑与本文件 HardFault_Handler 一致：\n'
     ' *          正常路径经 SVCRT_SWITCH_TASK 切走，末尾 while(1) 仅作兜底。\n',
     ' *          这些向量的处理逻辑与本文件 HardFault_Handler 一致：\n'
     ' *          内核返回可恢复的任务栈指针时，直接恢复该任务上下文并异常返回；\n'
     ' *          返回 0（内核/中断上下文故障）时落回 while(1)，停机等调试器接管。\n',
     '向量注释同步'),
])
print('OK')
