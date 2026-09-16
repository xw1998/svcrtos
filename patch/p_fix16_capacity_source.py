# -*- coding: utf-8 -*-
"""Byte-level patch for kernelsrc/include/svcrt_config.h (GBK, CRLF).

Goal: make config/svcrt_partition.h the single source of the task-capacity
policy.  Two local default definitions are removed and the partition header
is included instead.
"""
import sys, os

ROOT = r"D:\工作\git_project\svcrtos_new"
TARGET = os.path.join(ROOT, "kernelsrc", "include", "svcrt_config.h")

with open(TARGET, "rb") as f:
    data = f.read()

orig = data

# ---------------------------------------------------------------- patch 1
anchor1 = b"#ifdef SVCRT_BOARD_CONFIG\r\n#include SVCRT_BOARD_CONFIG\r\n#endif\r\n"
assert data.count(anchor1) == 1, "anchor1 count=%d" % data.count(anchor1)

insert1 = (
    b"\r\n"
    b"/* ============================================================\r\n"
    b" * Partition / capacity policy: single source of truth\r\n"
    b" * @brief Task table size and slot counts live in\r\n"
    b" *        config/svcrt_partition.h together with the Flash/RAM layout,\r\n"
    b" *        so a capacity change can never disagree with the layout.\r\n"
    b" *        Do not define SVCRT_TASK_MAX_NUM here anymore.\r\n"
    b" * ============================================================ */\r\n"
    b'#include "svcrt_partition.h"\r\n'
)
data = data.replace(anchor1, anchor1 + insert1, 1)

# ---------------------------------------------------------------- patch 2
anchor2_text = (
    "#ifndef SVCRT_TASK_MAX_NUM\r\n"
    "#define SVCRT_TASK_MAX_NUM        (32)   /* \u4efb\u52a1\uff08\u542b\u5185\u6838\u670d\u52a1\u4efb\u52a1\uff09\u4e0a\u9650\uff1b\r\n"
    "                                          * \u6bcf\u4e2a\u4efb\u52a1\u7ea6 80 \u5b57\u8282 TCB\uff0c\u8c03\u5927\u65f6\u53d7\r\n"
    "                                          * SVCRT_TASK_TABLE_RAM_MAX \u9884\u7b97\u7ea6\u675f */\r\n"
    "#endif\r\n"
    "#ifndef SVCRT_TASK_TABLE_RAM_MAX\r\n"
    "#define SVCRT_TASK_TABLE_RAM_MAX  (1024 * 8)   /* \u4efb\u52a1\u8868 TCB \u6570\u7ec4\u7684 RAM \u9884\u7b97\u4e0a\u9650\uff08\u5b57\u8282\uff09 */\r\n"
    "#endif\r\n"
)
anchor2 = anchor2_text.encode("gbk")
assert data.count(anchor2) == 1, "anchor2 count=%d" % data.count(anchor2)

replace2 = (
    b"/* Task capacity moved to config/svcrt_partition.h:\r\n"
    b" *   SVCRT_TASK_MAX_NUM        total task-table slots (static TCB array)\r\n"
    b" *   SVCRT_TASK_TABLE_RAM_MAX  RAM budget, derived from SVCRT_TASK_MAX_NUM\r\n"
    b" * Edit them there only; the partition header is included above. */\r\n"
)
data = data.replace(anchor2, replace2, 1)

# ---------------------------------------------------------------- verify
assert data != orig
assert data.count(b"\n") == data.count(b"\r\n"), "line endings changed"
assert b"#define SVCRT_TASK_MAX_NUM" not in data
assert b"#define SVCRT_TASK_TABLE_RAM_MAX" not in data
assert b'#include "svcrt_partition.h"' in data
data.decode("gbk")   # still valid GBK

with open(TARGET, "wb") as f:
    f.write(data)

print("OK bytes %d -> %d" % (len(orig), len(data)))
