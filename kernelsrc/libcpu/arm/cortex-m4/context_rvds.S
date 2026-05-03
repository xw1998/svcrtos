
    AREA    |.text|, CODE, READONLY

PendSV_Handler		PROC
	EXPORT	PendSV_Handler
    IMPORT  Kernel_isSwitching
	IMPORT	Kernel_Active
    PUSH	{R0,LR}

    LDR     R2, = Kernel_isSwitching
    BLX		R2
    CMN     R0,#0
    BMI     endpend

    MRS     R1,PSP

    IF :DEF:SVCRT_USE_FPU
    VSTMDB	R1!,{S16-S31}
    ENDIF

    STMDB   R1!,{R4-R11}

    LDR     R2,=Kernel_Active
    BLX		R2

    LDMIA	R0!,{R4-R11}

    IF :DEF:SVCRT_USE_FPU
    VLDMIA	R0!,{S16-S31}
    ENDIF

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
