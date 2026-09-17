# -*- coding: utf-8 -*-
"""main.c（GBK, CRLF）：接入 svcrt_log_init() 与启动横幅。

GBK 文件只做 ASCII 级别的字节替换，替换文本也全部用 ASCII，
避免整文件转码把 Keil 里的中文注释弄乱。
"""
import os

P = r"D:\工作\git_project\svcrtos_new\example\stm32f427\kernel\SVCRTOS_TEST\Core\Src\main.c"

with open(P, "rb") as f:
    d = f.read()

n0 = len(d)


def rep(old, new, what):
    global d
    if d.count(old) != 1:
        raise SystemExit("[%s] hits=%d" % (what, d.count(old)))
    d = d.replace(old, new, 1)


# 1) include
if b'svcrt_log.h' not in d:
    rep(b'#include "svcrt_shell.h"\r\n',
        b'#include "svcrt_shell.h"\r\n#include "svcrt_log.h"\r\n',
        "include")

# 2) 模块初始化之后立刻点亮日志，后面所有初始化都能留下痕迹
rep(b'    svcrt_kernel_module_init();\r\n',
    b'    svcrt_kernel_module_init();\r\n'
    b'\r\n'
    b'    /* Log first: everything after this point can leave a trace on the\n'
    b'     * console, which is the only cheap way to see where a boot stops. */\r\n'
    b'    svcrt_log_init();\r\n'
    b'    SVCRT_LOGI("BOOT", "SVCrtOS kernel starting, build %s %s",\r\n'
    b'               __DATE__, __TIME__);\r\n',
    "log init")

# 3) 映像认定/启动之后，把池与任务容量写进日志
rep(b'    svcrt_installer_init();\r\n'
    b'    #endif\r\n'
    b'}\r\n',
    b'    svcrt_installer_init();\r\n'
    b'    #endif\r\n'
    b'\r\n'
    b'    /* One line that answers the two questions asked most often on the\n'
    b'     * console: how much room is left for images, and how many tasks the\n'
    b'     * application set has already consumed. */\r\n'
    b'    {\r\n'
    b'        uint32 pool_largest = 0u;\r\n'
    b'        uint32 pool_free    = svcrt_loader_pool_free(&pool_largest);\r\n'
    b'\r\n'
    b'        SVCRT_LOGI("POOL", "free %u B (largest run %u B), tasks %d/%u",\r\n'
    b'                   pool_free, pool_largest,\r\n'
    b'                   (int)svcrt_task_count, (uint32)SVCRT_TASK_MAX_NUM);\r\n'
    b'    }\r\n'
    b'}\r\n',
    "boot banner")

with open(P, "wb") as f:
    f.write(d)

print("[ok] main.c %d -> %d bytes" % (n0, len(d)))
