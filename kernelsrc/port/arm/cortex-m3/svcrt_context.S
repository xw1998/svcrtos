
    AREA    |.text|, CODE, READONLY

;--------------------------------------------------------------
; PendSV_Handler - Cortex-M3 任务上下文切换
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
    STMDB   R1!, {R4-R11}

    LDR     R2, =svcrt_sched_activate
    BLX     R2

    LDMIA   R0!, {R4-R11}
    MSR     PSP, R0

endpend
    POP     {R0, PC}
    ENDP


;--------------------------------------------------------------
; SVC_Handler - 系统服务调用入口
;--------------------------------------------------------------

;--------------------------------------------------------------
; svcrt_port_resume_task - 在异常处理程序内部直接恢复目标任务并异常返回
; 入参: R0 = 目标任务栈指针（指向其保存的 R4-R11）
; 说明: 故障恢复路径专用。Cortex-M 上 HardFault 优先级(-1)高于 PendSV(0xFF)，
;       故障处理程序活动期间 PendSV 不会被服务，因此不能靠 PendSV 触发切换，
;       必须在本函数内直接完成“恢复寄存器 + 异常返回”。函数不返回。
;       M3 无 FPU，EXC_RETURN 固定 0xFFFFFFFD（返回 Thread 模式、使用 PSP）。
;--------------------------------------------------------------
svcrt_port_resume_task		PROC
    EXPORT  svcrt_port_resume_task

    LDR     R1, =0xFFFFFFFD
    LDMIA   R0!, {R4-R11}
    MSR     PSP, R0
    BX      R1
    ENDP
    LTORG

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
