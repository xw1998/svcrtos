
    AREA    |RESET|, CODE, READONLY
APPSTART    PROC
    EXPORT APPSTART
    IMPORT  main
            NOP
            NOP
            LDR R0, = main
            BLX R0
            B   .
            ENDP

            ALIGN
            END
