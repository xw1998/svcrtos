    AREA    |RESET|, CODE, READONLY
MINISTART   PROC
    EXPORT MINISTART
    IMPORT  __main
            NOP
            NOP
            ; jump to C runtime entry __main: it copies .data and zeroes .bss
            ; (scatter loading) and then calls main(). Jumping straight to
            ; main() would run with uninitialised globals.
            LDR R0, = __main
            BX  R0
            B   .
            ENDP

            ALIGN
            END
