    AREA    |RESET|, CODE, READONLY
DRVSTART    PROC
    EXPORT DRVSTART
    IMPORT  main
            NOP
            NOP
            LDR R0, = main
            BLX R0
            B   .
            ENDP

            ALIGN
            END
