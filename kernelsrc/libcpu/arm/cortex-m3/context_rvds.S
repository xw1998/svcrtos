
    AREA    |.text|, CODE, READONLY

PendSV_Handler		PROC
	EXPORT	PendSV_Handler
    IMPORT  svcrt_sched_is_switching
	IMPORT	svcrt_sched_activate
    PUSH	{R0,LR}

    LDR     R2, = svcrt_sched_is_switching
    BLX		R2
    CMN     R0,#0
    BMI     endpend

    MRS     R1,PSP
    STMDB   R1!,{R4-R11}

    LDR     R2,=svcrt_sched_activate
    BLX		R0

    LDMIA	R0!,{R4-R11}
    MSR     PSP,R0

endpend
    POP     {R0,PC}
    ENDP

SVC_Handler			PROC
	EXPORT	SVC_Handler
    IMPORT	SVC_Server
		TST     LR, #4
        ITE     EQ
        MRSEQ   R0,MSP
        MRSNE   R0,PSP
        B       SVC_Server
		ENDP
	END
