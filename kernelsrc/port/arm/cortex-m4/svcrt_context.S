
    AREA    |.text|, CODE, READONLY

;--------------------------------------------------------------
; PendSV_Handler - Cortex-M4 task context switch.
; When SVCRT_USE_FPU=1 a task may own an FPU extended frame, which also
; holds S16-S31; EXC_RETURN (LR) says which frame the interrupted code used.
;--------------------------------------------------------------
PendSV_Handler		PROC
    EXPORT  PendSV_Handler
    IMPORT  svcrt_sched_is_switching
    IMPORT  svcrt_sched_activate
    IMPORT  svcrt_trace_isr

    PUSH    {R0, LR}

; 插桩：上下文切换现场。PendSV 是真正换栈的地方，
; 任务级「谁换成谁」由 svcrt_sched_activate 另外记一笔。
; 调用 C 函数前必须自己保护 R0-R3/R12/LR（AAPCS 允许被调方改它们），
; 这里 R0 与 R12 在本函数后面还要用。
    PUSH    {R0-R3, R12, LR}
    MOVS    R0, #14
    MOVS    R1, #0
    LDR     R2, =svcrt_trace_isr
    BLX     R2
    POP     {R0-R3, R12, LR}

    LDR     R2, =svcrt_sched_is_switching
    BLX     R2
    CMN     R0, #0
    BMI     endpend

    MRS     R1, PSP

    ; The BLX above may clobber R0-R3/R12, so reload the interrupted
    ; context's EXC_RETURN from the entry PUSH {R0, LR} (LR is SP+4).
    LDR     R12, [SP, #4]

 IF {FPU} != "SoftVFP"
    ; extended frame in use (EXC_RETURN bit4=0): save S16-S31 as well
    TST     R12, #0x10
    IT      EQ
    VSTMDBEQ R1!, {S16-S31}
 ENDIF

    ; Save R4-R11 and this context's EXC_RETURN. R12 > R11, so the
    ; EXC_RETURN lands at the top of the block, i.e. directly below
    ; the hardware frame - the layout svcrt_port_stack_init() builds.
    STMDB   R1!, {R4-R11, R12}

    LDR     R2, =svcrt_sched_activate
    BLX     R2

    ; R0 = incoming task stack, R3 = incoming task's own EXC_RETURN.
    LDMIA   R0!, {R4-R11, R12}

 IF {FPU} != "SoftVFP"
    ; The incoming task's EXC_RETURN says whether it owns S16-S31.
    TST     R12, #0x10
    IT      EQ
    VLDMIAEQ R0!, {S16-S31}
 ENDIF

    MSR     PSP, R0

; 插桩：切换完成。R12 此时是目标任务自己的 EXC_RETURN，
; 马上就要拿它异常返回，所以要保护到 BLX 之后再恢复。
    PUSH    {R0-R3, R12, LR}
    MOVS    R0, #14
    MOVS    R1, #1
    LDR     R2, =svcrt_trace_isr
    BLX     R2
    POP     {R0-R3, R12, LR}

    ; Return through the incoming task's EXC_RETURN instead of the value
    ; pushed on entry: only that value makes the hardware restore Thread
    ; mode on PSP, which is what actually performs the switch.
    POP     {R0, LR}
    BX      R12

endpend
; 插桩：本次 PendSV 没有真正切换（调度器判定不需要），
; 进出仍然成对记，否则时间线上会出现「只进不出」的中断。
    PUSH    {R0-R3, R12, LR}
    MOVS    R0, #14
    MOVS    R1, #1
    LDR     R2, =svcrt_trace_isr
    BLX     R2
    POP     {R0-R3, R12, LR}

    POP     {R0, PC}
    ENDP


;--------------------------------------------------------------
; SVC_Handler - system service call entry.
;--------------------------------------------------------------

;--------------------------------------------------------------
; svcrt_port_resume_task - 在异常处理程序内部直接恢复目标任务并异常返回
; 入参: R0 = 目标任务栈指针（指向其保存的 R4-R11 + EXC_RETURN）
; 说明: 故障恢复路径专用。Cortex-M 上 HardFault 优先级(-1)高于 PendSV(0xFF)，
;       故障处理程序活动期间 PendSV 不会被服务，因此不能靠 PendSV 触发切换，
;       必须在本函数内直接完成“恢复寄存器 + 异常返回”。函数不返回。
;       栈布局与 PendSV_Handler / svcrt_port_stack_init 保持一致。
;--------------------------------------------------------------
svcrt_port_resume_task		PROC
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

SVC_Handler		PROC
    EXPORT  SVC_Handler
    IMPORT  SVC_Server

    TST     LR, #4
    ITE     EQ
    MRSEQ   R0, MSP
    MRSNE   R0, PSP
    B       SVC_Server
    ENDP

    END
