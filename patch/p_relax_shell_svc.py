# -*- coding: utf-8 -*-
"""SVC 0x1A 注册分支的指针校验放宽：
name / help 允许落在调用者自己的镜像区间（App 常量的字符串字面量就在那里），
不再强制要求用户 RAM；处理器仍必须落在调用者自己的固件区内。
svcrt_task.c 是 GBK + 混合行尾，全部按字节做正则替换，不整文件转码。
"""
import os
import re

ROOT = r"D:\工作\git_project\svcrtos_new"

TASK = os.path.join(ROOT, "kernelsrc", "src", "svcrt_task.c")

OLD = (r"                        if\(\(svcrt_kernel_ptr_ram_ok\(cmd, \(uint32\)sizeof\(svcrt_ushell_cmd_t\)\) == 0u\) \|\|\r?\n"
       r"                           \(svcrt_kernel_ptr_ram_ok\(cmd->name, 1u\) == 0u\) \|\|\r?\n"
       r"                           \(svcrt_kernel_ptr_ram_ok\(cmd->help, 1u\) == 0u\) \|\|\r?\n")

with open(TASK, "rb") as f:
    d = f.read()

m = re.search(OLD.encode("ascii"), d)
if m is None:
    print("[skip] pattern not found (already relaxed?)")
else:
    eol = b"\r\n" if d[m.start():m.start() + 60].count(b"\r\n") > 0 else b"\n"
    new = (b"                        if((svcrt_kernel_ptr_ram_ok(cmd, (uint32)sizeof(svcrt_ushell_cmd_t)) == 0u) ||" + eol +
           b"                           ((svcrt_kernel_ptr_ram_ok(cmd->name, 1u) == 0u) &&" + eol +
           b"                            (svcrt_kernel_cb_own_region_ok(cmd->name) == 0u)) ||" + eol +
           b"                           ((svcrt_kernel_ptr_ram_ok(cmd->help, 1u) == 0u) &&" + eol +
           b"                            (svcrt_kernel_cb_own_region_ok(cmd->help) == 0u)) ||" + eol)
    d = d[:m.start()] + new + d[m.end():]
    with open(TASK, "wb") as f:
        f.write(d)
    print("[ok] svcrt_task.c case-1 relaxed (%d bytes)" % len(d))

# 打印驱动示例源码，供下一步接入用
p = os.path.join(ROOT, "example", "stm32f427", "driver_sdk", "DRV_DEMO", "Src", "temp_drv.c")
with open(p, "rb") as f:
    raw = f.read()
try:
    print("=== temp_drv.c ===")
    print(raw.decode("gbk"))
except Exception:
    print(raw.decode("utf-8", "replace"))
