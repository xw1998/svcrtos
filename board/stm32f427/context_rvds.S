
    AREA    |.text|, CODE, READONLY

;--------------------------------------------------------------
; PendSV_Handler - 上下文切换
;
; 简化版:不再手动保存/恢复 FPU 上下文 (S16-S31)
; 原因:
;   1) FPCCR.ASPEN=1, LSPEN=1 已开启 FPU 自动状态保存和懒入栈,
;      硬件会在需要时把 S0-S15+FPSCR 入栈,EXC_RETURN.bit4 跟踪
;      是否有 FPU 帧,出栈时硬件自动恢复。
;   2) 任务里用到的 S16-S31 是非易失寄存器,正常 C 调用约定不会
;      跨函数破坏,任务切换时只要不动它们就行。
;   3) 这样避免了"PendSV 强行 VSTMDB 但 EXC_RETURN.bit4=1
;      不弹 FPU 导致栈错位"的致命问题。
;--------------------------------------------------------------
PendSV_Handler		PROC
    EXPORT  PendSV_Handler
    IMPORT  svcrt_sched_is_switching
    IMPORT  svcrt_sched_activate

    PUSH    {R0, LR}                ; 保存 EXC_RETURN

    LDR     R2, =svcrt_sched_is_switching
    BLX     R2                      ; R0 = 新任务 ID (-1 表示无切换)
    CMN     R0, #0
    BMI     endpend                 ; R0 < 0 → 不切换

    MRS     R1, PSP                 ; R1 = 当前 PSP
    STMDB   R1!, {R4-R11}           ; 保存 R4-R11 (8 word)

    LDR     R2, =svcrt_sched_activate
    BLX     R2                      ; R0 = 新任务 PSP (svcrt_sched_activate(new, old_psp))

    LDMIA   R0!, {R4-R11}           ; 恢复新任务的 R4-R11
    MSR     PSP, R0                 ; 更新 PSP 指向新任务的硬件帧

endpend
    POP     {R0, PC}                ; 异常返回 (LR 已被弹到 PC)
    ENDP


;--------------------------------------------------------------
; SVC_Handler - 系统调用入口
;--------------------------------------------------------------
SVC_Handler		PROC
    EXPORT  SVC_Handler
    IMPORT  SVC_Server

    TST     LR, #4                  ; 检查 EXC_RETURN.bit2
    ITE     EQ
    MRSEQ   R0, MSP                 ; bit2=0: 来自 MSP
    MRSNE   R0, PSP                 ; bit2=1: 来自 PSP
    B       SVC_Server              ; R0 指向异常帧,传给 C
    ENDP

    END
