# -*- coding: utf-8 -*-
"""
修复 16：配置宏一致性 + readme 配置表与代码对齐

问题1：readme 的“内核配置参数”表与实际代码不符
  - SVCRT_TASK_MAX_NUM 写 7，实际是 32；
  - SVCRT_USE_MPU / SVCRT_USE_PRIV 写“1 (M4/M7)”，实际默认值由架构宏派生，
    且当前板级配置明确把两者都置 0（MPU 未接线）；
  - 表里漏了 TASK_TABLE_RAM_MAX、MAX_EVENT_WAITERS、SEM_NUM、MTX_NUM、
    MAX_SYNC_WAITERS、STACK_END_FLAG 这些实际存在的宏。

问题2：svcrt_config.h 的“可被板级配置覆盖”只对一半宏成立
  该文件顶部声明“板级配置可覆盖这里的默认值”，但只有一部分宏用 #ifndef
  包住（SVCRT_USE_SPINLOCK、SVCRT_USE_FPU…）。其余（SVCRT_TASK_MAX_NUM、
  SVCRT_USE_MQ、SVCRT_TIMER_NUM 等）是裸 #define，板级配置想改就会撞上
  宏重定义告警，等于覆盖不了。统一给“可裁剪开关 / 可调参数”加 #ifndef；
  派生宏（SVCRT_MS_TO_TICK 之类）保持裸定义，它不是配置项。

实现说明：加 #ifndef 用“按宏名定位 + 跨行注释合并”的方式做，不硬编码
多行 define 的空白，避免锚点对不上。
"""
import os
import re
import shutil

ROOT = r'D:\工作\git_project\svcrtos_new'
BK = r'C:\Users\14905\OneDrive\Documents\lingxi-claw\20260911-13-47-19-445\backup_fix7'

FILES = ['kernelsrc/include/svcrt_config.h', 'readme.md']

# 需要加 #ifndef 的配置宏（不含派生宏 SVCRT_MS_TO_TICK）
GUARD_NAMES = [
    'SVCRT_TASK_MAX_NUM',
    'SVCRT_TASK_TABLE_RAM_MAX',
    'SVCRT_TICK_PERIOD_US',
    'SVCRT_EVENT_NUM',
    'SVCRT_MAX_EVENT_WAITERS',
    'SVCRT_SEM_NUM',
    'SVCRT_MTX_NUM',
    'SVCRT_MAX_SYNC_WAITERS',
    'SVCRT_DEV_MAX_NUM',
    'SVCRT_USE_MQ',
    'SVCRT_MQ_NUM',
    'SVCRT_MQ_DEPTH',
    'SVCRT_MQ_MSG_WORDS',
    'SVCRT_USE_TIMER',
    'SVCRT_TIMER_NUM',
    'SVCRT_TIMER_TASK_PRI',
    'SVCRT_TIMER_TASK_STACK_WORDS',
    'SVCRT_USE_FAULT_RECOVER',
    'SVCRT_FAULT_RECORD_NUM',
    'SVCRT_USE_CPU_LOAD',
    'SVCRT_USE_STACK_CHECK',
    'SVCRT_STACK_END_FLAG',
]


def detect_enc(raw):
    try:
        raw.decode('utf-8')
        return 'utf-8'
    except UnicodeDecodeError:
        return 'gbk'


class Doc(object):
    def __init__(self, rel):
        self.rel = rel
        self.p = os.path.join(ROOT, rel.replace('/', os.sep))
        self.raw = open(self.p, 'rb').read()
        self.enc = detect_enc(self.raw)

    def sub(self, old, new, label, count=1, all=False):
        for eol in ('\r\n', '\n'):
            o = old.replace('\n', eol).encode(self.enc)
            n = new.replace('\n', eol).encode(self.enc)
            hit = self.raw.count(o)
            if all:
                if hit > 0:
                    self.raw = self.raw.replace(o, n)
                    print('  ok(%s)x%d: %s' % (repr(eol), hit, label))
                    return
            elif hit == count:
                self.raw = self.raw.replace(o, n)
                print('  ok(%s): %s' % (repr(eol), label))
                return
        raise AssertionError('[%s/%s] %s: 未找到匹配' % (self.rel, self.enc, label))

    def save(self):
        open(self.p, 'wb').write(self.raw)


def define_block_end(lines, start):
    """返回一个 #define 逻辑行占用的最后一行下标（含跨行的块注释）。

    C 预处理里块注释内的换行不结束逻辑行，因此
    `#define X 1 /* 注释\\n  续行 */` 整体是一行。
    这里用 /* 与 */ 计数判断注释是否闭合，兼容行尾反斜杠续行。
    """
    i = start
    opens = 0
    while i < len(lines):
        text = lines[i].rstrip('\r')
        opens += text.count('/*') - text.count('*/')
        cont = text.rstrip().endswith('\\')
        if opens <= 0 and not cont:
            return i
        i += 1
    return len(lines) - 1


def guard_defines(doc, names):
    s = doc.raw.decode(doc.enc)
    lines = s.split('\n')
    out = []
    added = []
    i = 0
    while i < len(lines):
        line = lines[i]
        code = line.rstrip('\r')
        m = re.match(r'#define\s+(\w+)', code)
        if m is not None and m.group(1) in names:
            name = m.group(1)
            end = define_block_end(lines, i)
            cr = '\r' if line.endswith('\r') else ''
            out.append('#ifndef ' + name + cr)
            for k in range(i, end + 1):
                out.append(lines[k])
            out.append('#endif' + cr)
            added.append(name)
            i = end + 1
            continue
        out.append(line)
        i += 1

    missing = [n for n in names if n not in added]
    if missing:
        raise AssertionError('未找到这些宏的定义: %s' % ', '.join(missing))
    doc.raw = '\n'.join(out).encode(doc.enc)
    print('  ok: 加了 %d 个 #ifndef 保护' % len(added))


CONV_OLD = """* @details 借鉴 RT-Thread 的 rtconfig.h 思路，把内核所有可裁剪、可调参数集中在此管理。
*          板级配置可通过 SVCRT_BOARD_CONFIG 宏指向的头文件覆盖这里的默认值。"""

CONV_NEW = """* @details 借鉴 RT-Thread 的 rtconfig.h 思路，把内核所有可裁剪、可调参数集中在此管理。
*          板级配置可通过 SVCRT_BOARD_CONFIG 宏指向的头文件覆盖这里的默认值。
*          覆盖规则：本文件里**所有可裁剪开关与可调参数**都必须用 #ifndef 包住，
*          否则板级配置重定义时会报宏重定义，等于覆盖不了；
*          由这些参数派生出来的宏（如 SVCRT_MS_TO_TICK）不包，它不是配置项。"""

TABLE_OLD = """| `SVCRT_CPU_ARCH` | `SVCRT_ARCH_CORTEX_M4` | CPU 架构选择 |
| `SVCRT_USE_FPU` | 1 (M4/M7) | 浮点单元使能 |
| `SVCRT_USE_MPU` | 1 (M4/M7) | MPU 内存保护使能 |
| `SVCRT_USE_PRIV` | 1 (依赖MPU) | 特权级分离使能 |
| `SVCRT_TASK_MAX_NUM` | 7 | 最大任务数量 |
| `SVCRT_TICK_PERIOD_US` | 500 | 滴答周期（微秒） |
| `SVCRT_EVENT_NUM` | 10 | 事件对象数量 |
| `SVCRT_DEV_MAX_NUM` | 8 | 最大设备数量 |"""

TABLE_NEW = """| `SVCRT_CPU_ARCH` | 由架构头派生（本板 `SVCRT_ARCH_CORTEX_M4`） | CPU 架构选择 |
| `SVCRT_USE_FPU` | 由架构派生，本板 1 | 浮点单元使能 |
| `SVCRT_USE_MPU` | 由架构派生，本板 0 | MPU 内存保护使能（**未接线**，见 `kernelsrc/src/svcrt_mpu.c` 的 @warning） |
| `SVCRT_USE_PRIV` | 依赖 `SVCRT_USE_MPU`，本板 0 | 特权级分离使能 |
| `SVCRT_TASK_MAX_NUM` | 32 | 最大任务数量（受下一行 RAM 预算约束） |
| `SVCRT_TASK_TABLE_RAM_MAX` | 8192 | TCB 数组的 RAM 预算上限（字节，超出则编译报错） |
| `SVCRT_TICK_PERIOD_US` | 500 | 滴答周期（微秒） |
| `SVCRT_EVENT_NUM` | 10 | 事件对象数量 |
| `SVCRT_MAX_EVENT_WAITERS` | 4 | 单个事件的等待者上限 |
| `SVCRT_SEM_NUM` / `SVCRT_MTX_NUM` | 8 / 8 | 信号量 / 互斥锁对象数量 |
| `SVCRT_MAX_SYNC_WAITERS` | 4 | 单个信号量/互斥锁的等待者上限 |
| `SVCRT_DEV_MAX_NUM` | 8 | 最大设备数量 |"""

TABLE2_OLD = """| `SVCRT_USE_FAULT_RECOVER` | 1 | 任务故障自动恢复开关 |
| `SVCRT_FAULT_RECORD_NUM` | 8 | 故障记录环形缓冲容量 |"""

TABLE2_NEW = """| `SVCRT_USE_FAULT_RECOVER` | 1 | 任务故障自动恢复开关 |
| `SVCRT_FAULT_RECORD_NUM` | 8 | 故障记录环形缓冲容量 |
| `SVCRT_USE_STACK_CHECK` / `SVCRT_STACK_END_FLAG` | 1 / 0xed01 | 栈溢出检测开关与栈底保护字 |"""


def main():
    os.makedirs(BK, exist_ok=True)
    for rel in FILES:
        src = os.path.join(ROOT, rel.replace('/', os.sep))
        shutil.copy2(src, os.path.join(BK, rel.replace('/', '__')))
    print('备份完成 -> %s' % BK)

    d = Doc('kernelsrc/include/svcrt_config.h')
    d.sub(CONV_OLD, CONV_NEW, '顶部写明覆盖规则')
    guard_defines(d, GUARD_NAMES)
    d.save()

    d = Doc('readme.md')
    d.sub(TABLE_OLD, TABLE_NEW, '配置表前段与代码对齐')
    d.sub(TABLE2_OLD, TABLE2_NEW, '配置表补栈检测行')
    d.save()

    print('OK')


if __name__ == '__main__':
    main()
