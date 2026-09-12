
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

    PUSH    {R0, LR}

    LDR     R2, =svcrt_sched_is_switching
    BLX     R2
    CMN     R0, #0
    BMI     endpend

    MRS     R1, PSP

 IF {FPU} != "SoftVFP"
    ; extended frame in use (EXC_RETURN bit4=0): save S16-S31 as well
    TST     LR, #0x10
    IT      EQ
    VSTMDBEQ R1!, {S16-S31}
 ENDIF

    STMDB   R1!, {R4-R11, LR}

    LDR     R2, =svcrt_sched_activate
    BLX     R2

    LDMIA   R0!, {R4-R11, LR}

 IF {FPU} != "SoftVFP"
    TST     LR, #0x10
    IT      EQ
    VLDMIAEQ R0!, {S16-S31}
 ENDIF

    MSR     PSP, R0

endpend
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
