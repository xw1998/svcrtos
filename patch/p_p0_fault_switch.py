# -*- coding: utf-8 -*-
"""
P0-1 修复：故障恢复链路在真实硬件上不可用

问题（已确认的设计级缺陷）：
  板级 HardFault/MemManage/BusFault/UsageFault 向量调用内核 svcrt_cpu_fault_handler()，
  后者在任务上下文故障时只做 SVCRT_SWITCH_TASK()（= 置 PendSV pending）就返回，
  板级向量紧接着 while(1)。
  Cortex-M 上 HardFault 优先级 -1 高于 PendSV 的 0xFF，高优先级异常处于 active 期间
  低优先级异常无法被服务；返回后控制流又落回 while(1)。
  => PendSV 永远不会执行，任务不会重启，"连续崩溃 N 次即禁用"在真机上从不生效。

修复：
  1) port 层新增 svcrt_port_resume_task(sp)：在异常处理程序内部直接
     恢复目标任务寄存器并触发异常返回（PendSV 后半段的等价实现）。
  2) 内核 svcrt_cpu_fault_handler 改为返回"可用于恢复的任务栈指针"，
     故障路径不再依赖 PendSV，直接 选出下一个任务 + activate。
  3) 板级四个向量按返回值决定：非 0 → resume（不返回）；0 → while(1) 等调试器。

本脚本为字节级补丁，不改动无关内容。
"""
import os
import re
import shutil

ROOT = r'D:\工作\git_project\svcrtos_new'
BK   = r'C:\Users\14905\OneDrive\Documents\lingxi-claw\20260911-13-47-19-445\backup_p0_fault'

FILES = [
    'kernelsrc/port/arm/cortex-m3/svcrt_context.S',
    'kernelsrc/port/arm/cortex-m4/svcrt_context.S',
    'kernelsrc/include/svcrt_hal.h',
    'kernelsrc/include/svcrt_task.h',
    'kernelsrc/src/svcrt_task.c',
    'board/stm32f427/svcrt_board.c',
]


def path_of(rel):
    return os.path.join(ROOT, rel.replace('/', os.sep))


def do_backup():
    os.makedirs(BK, exist_ok=True)
    for rel in FILES:
        dst = os.path.join(BK, rel.replace('/', '__'))
        shutil.copy2(path_of(rel), dst)


class Doc(object):
    def __init__(self, rel):
        self.rel = rel
        self.p = path_of(rel)
        self.raw = open(self.p, 'rb').read()
        self.nl = b'\r\n' if b'\r\n' in self.raw else b'\n'
        self.hits = []

    def t(self, s):
        """把 unicode 文本按本文件换行风格编码为字节"""
        s = s.replace('\n', '\r\n') if self.nl == b'\r\n' else s
        return s.encode('gbk')

    def sub(self, old, new, label, count=1):
        if isinstance(old, str):
            old = self.t(old)
        if isinstance(new, str):
            new = self.t(new)
        n = self.raw.count(old)
        if n != count:
            raise AssertionError('[%s] %s: 期望匹配 %d 次，实际 %d 次' % (self.rel, label, count, n))
        self.raw = self.raw.replace(old, new)
        self.hits.append(label)

    def resub(self, pat, new, label, count=1, flags=re.S):
        if isinstance(new, str):
            new = self.t(new)
        rx = re.compile(pat, flags)
        found = rx.findall(self.raw)
        if len(found) != count:
            raise AssertionError('[%s] %s: 期望匹配 %d 次，实际 %d 次' % (self.rel, label, count, len(found)))
        self.raw = rx.sub(lambda m: new, self.raw, count=count)
        self.hits.append(label)

    def save(self):
        open(self.p, 'wb').write(self.raw)


# ---------------------------------------------------------------- port 汇编
M4_RESUME = """
;--------------------------------------------------------------
; svcrt_port_resume_task - 在异常处理程序内部直接恢复目标任务并异常返回
; 入参: R0 = 目标任务栈指针（指向其保存的 R4-R11 + EXC_RETURN）
; 说明: 故障恢复路径专用。Cortex-M 上 HardFault 优先级(-1)高于 PendSV(0xFF)，
;       故障处理程序活动期间 PendSV 不会被服务，因此不能靠 PendSV 触发切换，
;       必须在本函数内直接完成“恢复寄存器 + 异常返回”。函数不返回。
;       栈布局与 PendSV_Handler / svcrt_port_stack_init 保持一致。
;--------------------------------------------------------------
svcrt_port_resume_task\t\tPROC
    EXPORT  svcrt_port_resume_task

    LDMIA   R0!, {R4-R11, LR}

 IF {FPU} != "SoftVFP"
    ; 目标任务保存的 EXC_RETURN bit4=0 表示其使用 FPU 扩展帧，还需恢复 S16-S31
    TST     LR, #0x10
    IT      EQ
    VLDMIAEQ R0!, {S16-S31}
 ENDIF

    MSR     PSP, R0
    BX      LR
    ENDP

"""

M3_RESUME = """
;--------------------------------------------------------------
; svcrt_port_resume_task - 在异常处理程序内部直接恢复目标任务并异常返回
; 入参: R0 = 目标任务栈指针（指向其保存的 R4-R11）
; 说明: 故障恢复路径专用。Cortex-M 上 HardFault 优先级(-1)高于 PendSV(0xFF)，
;       故障处理程序活动期间 PendSV 不会被服务，因此不能靠 PendSV 触发切换，
;       必须在本函数内直接完成“恢复寄存器 + 异常返回”。函数不返回。
;       M3 无 FPU，EXC_RETURN 固定 0xFFFFFFFD（返回 Thread 模式、使用 PSP）。
;--------------------------------------------------------------
svcrt_port_resume_task\t\tPROC
    EXPORT  svcrt_port_resume_task

    LDR     R1, =0xFFFFFFFD
    LDMIA   R0!, {R4-R11}
    MSR     PSP, R0
    BX      R1
    ENDP
    LTORG

"""


def patch_port(rel, blk, anchor=b'SVC_Handler\t\tPROC'):
    d = Doc(rel)
    d.sub(anchor, blk + anchor.decode('ascii'), 'insert svcrt_port_resume_task')
    d.save()
    return d.hits


# ---------------------------------------------------------------- hal.h 声明
HAL_DECL = """void svcrt_port_switch_task(void);

/**
* @brief 在异常处理程序内部直接恢复目标任务上下文并异常返回
* @param stack_ptr 目标任务栈指针（指向其保存的 R4-R11[/EXC_RETURN]）
* @details 故障恢复等“已经处于异常处理程序中、不能依赖 PendSV”的场景专用：
*          Cortex-M 上 HardFault 优先级高于 PendSV，故障处理程序活动期间
*          PendSV 不会被服务，必须由本函数直接完成恢复 + 异常返回。
*          本函数不返回；stack_ptr 为 0 时调用方应自行停机，不要调用。
*/
void svcrt_port_resume_task(uint32 stack_ptr);"""


def patch_hal():
    d = Doc('kernelsrc/include/svcrt_hal.h')
    d.sub('void svcrt_port_switch_task(void);', HAL_DECL, 'resume_task 声明')
    d.save()
    return d.hits


# ---------------------------------------------------------------- task.h
def patch_task_h():
    d = Doc('kernelsrc/include/svcrt_task.h')
    d.sub(
        'void svcrt_kernel_tick_handler(void);\nvoid svcrt_hardfault_handler(void);\n'
        '/* 各 CPU 异常共用入口：fault_type 取 svcrt_fault_type_t 中的 HARDFAULT/MEMFAULT/BUSFAULT/USGFAULT */\n'
        'void svcrt_cpu_fault_handler(uint32 fault_type);',
        'void svcrt_kernel_tick_handler(void);\n'
        '/* 各 CPU 异常共用入口：fault_type 取 svcrt_fault_type_t 中的 HARDFAULT/MEMFAULT/BUSFAULT/USGFAULT\n'
        ' * 返回值：可用于恢复的任务栈指针（非 0 时由板级层调用 svcrt_port_resume_task 完成恢复）；\n'
        ' *         0 表示不可恢复（内核/中断上下文故障），板级层应停机等待调试器。 */\n'
        'uint32 svcrt_cpu_fault_handler(uint32 fault_type);\n'
        'uint32 svcrt_hardfault_handler(void);',
        'fault 入口返回值声明')
    d.save()
    return d.hits


# ---------------------------------------------------------------- task.c
NEW_FAULT = """uint32 svcrt_cpu_fault_handler(uint32 fault_type)
{
    if(svcrt_current_task_id > 0)
    {
        int32 tid  = svcrt_current_task_id;
        int32 next;

        svcrt_fault_record(fault_type, tid);

        #if (SVCRT_USE_FAULT_RECOVER == 1)
        /* 未达连续故障上限：安排重启（重建栈帧）；
         * 达到上限：svcrt_loader_on_fault 内部已置 INVALID 并清零 recover_pending，
         * 该 App 被禁用、不再重启。 */
        if(svcrt_loader_on_fault(tid) != 0)
        {
            /* 已被禁用，无需恢复 */
        }
        else
        {
            svcrt_task_recover_mark(tid);
        }
        #else
        svcrt_task_table[tid - 1].status = SVCRT_TASK_INVALID;
        #endif

        /* 关键：故障路径不经 PendSV 切换。
         * HardFault 优先级(-1)高于 PendSV(0xFF)，故障处理程序活动期间 PendSV
         * 不会被服务；若只置 PendSV 就返回，PendSV 永远不会执行，故障任务会
         * 带着损坏现场继续运行（或落回向量末尾的 while(1)）——这正是此前
         * “连续崩溃达到上限即禁用”在真实硬件上从不生效的原因。
         * 这里直接选出下一个任务，把它的栈指针返回给板级层，
         * 由 svcrt_port_resume_task() 完成“恢复寄存器 + 异常返回”，
         * 等价于 PendSV 切换的后半段。
         * old_psp 传 0：故障任务现场不再保存（它即将重建或被禁用）。 */
        next = svcrt_sched_next() + 1;      /* recover_pending 在 sched_next 内部完成重建 */
        if(next < 0)
        {
            next = 0;                       /* 无就绪任务：退回空闲 */
        }

        return (uint32)svcrt_sched_activate(next, 0u);
    }

    /* 内核/中断上下文异常：没有可重启的任务，记录后停机等待调试器接管 */
    svcrt_fault_record(fault_type, 0);
    return 0u;
}

uint32 svcrt_hardfault_handler(void)
{
    return svcrt_cpu_fault_handler(SVCRT_FAULT_HARDFAULT);
}"""

NEW_RECOVER = """/* 标记任务待恢复（阶段1）：置 INVALID + recover_pending。
 * 真正的栈帧重建推迟到下一次调度扫描（svcrt_sched_next -> svcrt_task_recover_pending），
 * 那时故障任务的旧现场已经不会再被写回 stack_ptr，重建才安全。 */
static void svcrt_task_recover_mark(int32 task_id)
{
    svcrt_task_t *p_task;

    if(task_id <= 0 || task_id > svcrt_task_count)
    {
        return;
    }

    p_task = &svcrt_task_table[task_id - 1];

    SVCRT_DISABLE_IRQ();
    p_task->recover_pending = 1;
    p_task->status          = SVCRT_TASK_INVALID;
    SVCRT_ENABLE_IRQ();

    #if (SVCRT_USE_FAULT_RECOVER == 1)
    svcrt_fault_record(SVCRT_FAULT_RECOVER, task_id);
    #endif
}

int32 svcrt_task_recover(int32 task_id)
{
    if(task_id <= 0 || task_id > svcrt_task_count)
    {
        return -1;
    }

    svcrt_task_recover_mark(task_id);
    SVCRT_SWITCH_TASK();
    return 0;
}"""


def patch_task_c():
    d = Doc('kernelsrc/src/svcrt_task.c')

    d.sub('static void svcrt_task_recover_pending(void);',
          'static void svcrt_task_recover_mark(int32 task_id);\n'
          'static void svcrt_task_recover_pending(void);',
          'recover_mark 前置声明')

    d.resub(rb'void svcrt_cpu_fault_handler\(uint32 fault_type\)\r\n'
            rb'\{.*?\r\n\}\r\n\r\n'
            rb'void svcrt_hardfault_handler\(void\)\r\n'
            rb'\{\r\n'
            rb'    svcrt_cpu_fault_handler\(SVCRT_FAULT_HARDFAULT\);\r\n'
            rb'\}', NEW_FAULT, 'fault handler 重写')

    d.resub(rb'int32 svcrt_task_recover\(int32 task_id\)\r\n\{.*?\r\n\}\r\n',
            NEW_RECOVER + '\n', 'task_recover 拆分')

    d.sub('    else\n    {\n        svcrt_task_table[tid].stack_ptr = old_psp;',
          '    else if(old_psp != 0u)\n    {\n'
          '        /* old_psp == 0：故障恢复路径，调用方不保存故障任务现场\n'
          '         * （该任务即将重建栈帧或被禁用），跳过 stack_ptr 回写与栈检查。 */\n'
          '        svcrt_task_table[tid].stack_ptr = old_psp;',
          'activate 跳过故障现场回写')

    d.save()
    return d.hits


# ---------------------------------------------------------------- board.c
BOARD_FAULT_CALL = """    sp = svcrt_hardfault_handler();

    if(sp != 0u)
    {
        /* 内核已选出可运行任务并返回其栈指针：直接恢复并异常返回（本函数不返回） */
        svcrt_port_resume_task(sp);
    }

    while(1) { }"""

BOARD_SOFT = """    uint32 sp = svcrt_cpu_fault_handler(SVCRT_FAULT_%s);

    if(sp != 0u)
    {
        /* 同上：故障恢复路径不依赖 PendSV，直接恢复目标任务上下文 */
        svcrt_port_resume_task(sp);
    }

    while(1) { }"""


def patch_board():
    d = Doc('board/stm32f427/svcrt_board.c')

    d.sub('    (void)hfsr; (void)cfsr; (void)mmfar; (void)bfar;',
          '    uint32 sp;\n\n    (void)hfsr; (void)cfsr; (void)mmfar; (void)bfar;',
          'HardFault 增加 sp 变量')

    d.resub(rb'    svcrt_hardfault_handler\(\);.*?while\(1\) \{ \}',
            BOARD_FAULT_CALL, 'HardFault 向量接线')

    for name in ('MEMFAULT', 'BUSFAULT', 'USGFAULT'):
        d.resub(rb'    svcrt_cpu_fault_handler\(SVCRT_FAULT_' + name.encode() + rb'\);\r\n    while\(1\) \{ \}',
                BOARD_SOFT % name, name + ' 向量接线')

    d.save()
    return d.hits


def main():
    do_backup()
    print('备份目录:', BK)
    print('--- cortex-m4/context.S :', patch_port(FILES[1], M4_RESUME))
    print('--- cortex-m3/context.S :', patch_port(FILES[0], M3_RESUME))
    print('--- svcrt_hal.h         :', patch_hal())
    print('--- svcrt_task.h        :', patch_task_h())
    print('--- svcrt_task.c        :', patch_task_c())
    print('--- svcrt_board.c       :', patch_board())
    print('OK')


if __name__ == '__main__':
    main()
