
    AREA    |.text|, CODE, READONLY

;--------------------------------------------------------------
; PendSV_Handler - Cortex-M4 ???????????§Ý?
; ?? SVCRT_USE_FPU=1 ?????????/??? FPU ???????????? S16-S31??
; ?????? EXC_RETURN(LR) ???????????? FPU ???????
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
    ; ?????????????? FPU??EXC_RETURN bit4=0???????? S16-S31
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
; SVC_Handler - ????????????
;--------------------------------------------------------------
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
