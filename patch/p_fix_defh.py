# -*- coding: utf-8 -*-
"""修复 svcrt_def.h 里 SVC 号插入时被吃掉的一个 '#'。"""
import os

DEFH = r"D:\工作\git_project\svcrtos_new\kernelsrc\include\svcrt_def.h"

with open(DEFH, "rb") as f:
    d = f.read()

before = d
d = d.replace(b"##define SVCRT_SVC_LOG", b"#define SVCRT_SVC_LOG", 1)
d = d.replace(b"\r\ndefine SVCRT_SVC_APP_MGR", b"\r\n#define SVCRT_SVC_APP_MGR", 1)

if d == before:
    print("[warn] nothing changed")
else:
    with open(DEFH, "wb") as f:
        f.write(d)
    print("[ok] def.h repaired")

i = d.find(b"SVCRT_SVC_TIMER_CTRL")
print(repr(d[i:i + 230]))
