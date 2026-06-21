    AREA    |RESET|, CODE, READONLY
DRVSTART    PROC
    EXPORT DRVSTART
    IMPORT  __main
            NOP
            NOP
            ; 跳转到 C 库入口 __main：完成 .data 拷贝 + .bss 清零（scatter loading）
            ; 后自动调用 main()。直接跳 main 会跳过运行时初始化导致全局变量未初始化。
            LDR R0, = __main
            BX  R0
            B   .
            ENDP

            ALIGN
            END
