# -*- coding: utf-8 -*-
"""把新模块 svcrt_log.c 加进内核 MDK 工程（.uvprojx 为 LF 行尾）。"""
import io

P = r"D:\工作\git_project\svcrtos_new\example\stm32f427\kernel\SVCRTOS_TEST\MDK-ARM\SVCRTOS_TEST.uvprojx"

with open(P, "rb") as f:
    d = f.read()

if b"svcrt_log.c" in d:
    print("[skip] already present")
    raise SystemExit(0)

anchor = (b"              <FilePath>..\\..\\..\\..\\..\\kernelsrc\\src\\svcrt_installer.c</FilePath>\n"
          b"            </File>")

n = d.count(anchor)
print("anchor hits:", n)
if n != 1:
    raise SystemExit("anchor not unique")

block = (b"\n            <File>\n"
         b"              <FileName>svcrt_log.c</FileName>\n"
         b"              <FileType>1</FileType>\n"
         b"              <FilePath>..\\..\\..\\..\\..\\kernelsrc\\src\\svcrt_log.c</FilePath>\n"
         b"            </File>")

d = d.replace(anchor, anchor + block, 1)
with open(P, "wb") as f:
    f.write(d)
print("[ok] added svcrt_log.c to SVCRTOS_TEST.uvprojx")
