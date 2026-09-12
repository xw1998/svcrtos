#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
SVCrtOS 分散加载文件（.sct）生成器
==================================

单一配置源头：config/svcrt_partition.h
本脚本读取其中的宏（含自动推导的地址），为各工程生成对应的 .sct，
保证「地址只定义一次，其余全部派生」，且生成结果一定不自相重叠。

用法：
    python tools/gen_scatter.py --target kernel --output build/kernel.sct
    python tools/gen_scatter.py --target driver --output build/bled_drv.sct
    python tools/gen_scatter.py --target app    --output build/app_demo.sct
    python tools/gen_scatter.py --target boot   --output build/boot.sct
    python tools/gen_scatter.py --target all    --output build          # 一次生成全部

    # 校验当前配置（展开所有关键宏并做重叠检查）
    python tools/gen_scatter.py --check

可选参数：
    --header <path>   指定分区头文件（默认 config/svcrt_partition.h），便于做配置变更演练
    --ccm             为内核附加 CCM/TCM RAM 段（默认自动：CHIP_CCM_SIZE > 0 时附加）

设计约束：
    1. Loader / App 工程的 .sct 只覆盖自己的分区，绝不越界；
    2. 各映像的 RW 段使用各自独立的 RAM 区间（不能共用，否则互相覆盖）；
    3. 生成的 .sct 属于构建产物，建议放在 build/ 且不纳入版本库。
"""

from __future__ import print_function

import argparse
import os
import re
import sys

# 仓库根目录（基于脚本自身位置定位，不依赖调用方的工作目录，
# 这样 MDK 在任意工程目录下执行本脚本都能正确定位配置头）
REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_HEADER = os.path.join(REPO_ROOT, "config", "svcrt_partition.h")

# ---------------------------------------------------------------- 宏解析
DEFINE_RE = re.compile(r"^\s*#\s*define\s+([A-Za-z_]\w*)\s+(.+?)\s*(?:/\*.*)?$")
UNDEF_RE = re.compile(r"^\s*#\s*undef\s+([A-Za-z_]\w*)")


def parse_defines(path):
    """解析头文件中的 #define（支持跨行续行），返回 {名称: 表达式字符串}"""
    defines = {}
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        raw = f.read()
    # 合并续行
    raw = re.sub(r"\\\s*\n", " ", raw)
    for line in raw.split("\n"):
        m = UNDEF_RE.match(line)
        if m:
            defines.pop(m.group(1), None)
            continue
        m = DEFINE_RE.match(line)
        if not m:
            continue
        name, expr = m.group(1), m.group(2).strip()
        if name in ("SVCRT_PARTITION_H",):
            continue
        # 跳过函数式宏
        if "(" in name:
            continue
        defines[name] = expr
    return defines


class MacroEval(object):
    """惰性求值的宏展开器：支持宏之间相互引用（如 APP_USER_SIZE）"""

    def __init__(self, defines):
        self.defines = defines
        self.cache = {}

    def value(self, name, stack=None):
        if name in self.cache:
            return self.cache[name]
        stack = stack or []
        if name in stack:
            raise RuntimeError("宏递归引用: %s" % " -> ".join(stack + [name]))
        if name not in self.defines:
            raise KeyError("未定义的宏: %s" % name)

        expr = self.defines[name]
        # 把表达式中的其它宏替换为已求值结果
        def repl(m):
            token = m.group(0)
            if token in ("u", "U", "L", "UL", "0x", "x"):
                return token
            if token in self.defines:
                return "(%d)" % self.value(token, stack + [name])
            return token

        expr = re.sub(r"[A-Za-z_]\w*", repl, expr)
        expr = expr.replace("u", "").replace("U", "")   # 去掉无符号后缀
        try:
            val = int(eval(expr, {"__builtins__": {}}, {}))  # noqa: S307
        except Exception as exc:                          # noqa: BLE001
            raise RuntimeError("宏 %s 求值失败: %s = %r (%s)" % (name, name, self.defines[name], exc))
        self.cache[name] = val
        return val


# ---------------------------------------------------------------- 布局模型
class Layout(object):
    def __init__(self, ev):
        self.ev = ev

    def v(self, name):
        return self.ev.value(name)

    def regions(self):
        """返回 [(名称, 基址, 大小)] 两组表：Flash 与 RAM"""
        flash = [
            ("BOOT", "BOOT_BASE", "BOOT_SIZE"),
            ("KERNEL", "KERNEL_BASE", "KERNEL_SIZE"),
            ("DRIVER_POOL", "DRIVER_POOL_BASE", "DRIVER_POOL_SIZE"),
            ("APP_SLOT0", "APP_SLOT0_BASE", "APP_SLOT0_SIZE"),
        ]
        ram = [
            ("SHARE_RAM", "SHARE_RAM_BASE", "SHARE_RAM_SIZE"),
            ("KERNEL_RAM", "KERNEL_RAM_BASE", "KERNEL_RAM_SIZE"),
            ("DRIVER_RAM", "DRIVER_RAM_BASE", "DRIVER_RAM_SIZE"),
            ("APP_RAM", "APP_RAM_BASE", "APP_RAM_SIZE"),
        ]
        return flash, ram

    def check(self):
        """检查各分区是否越界与重叠，返回问题列表"""
        problems = []
        for kind, table in (("Flash", self.regions()[0]), ("RAM", self.regions()[1])):
            used = []
            for name, base_m, size_m in table:
                base = self.v(base_m)
                size = self.v(size_m)
                if size > 0:
                    used.append((name, base, size))
            used.sort(key=lambda x: x[1])
            for i in range(len(used) - 1):
                n1, b1, s1 = used[i]
                n2, b2, _ = used[i + 1]
                if b1 + s1 > b2:
                    problems.append("%s 分区重叠: %s(0x%08X+0x%X) 与 %s(0x%08X)"
                                    % (kind, n1, b1, s1, n2, b2))
            top = self.v("CHIP_FLASH_BASE") + self.v("CHIP_FLASH_SIZE") \
                if kind == "Flash" else self.v("CHIP_RAM_BASE") + self.v("CHIP_RAM_SIZE")
            bottom = self.v("CHIP_FLASH_BASE") if kind == "Flash" else self.v("CHIP_RAM_BASE")
            for n, b, s in used:
                if b < bottom or b + s > top:
                    problems.append("%s 分区越界: %s(0x%08X+0x%X) 超出芯片范围" % (kind, n, b, s))
        return problems

    def dump(self):
        lines = ["分区布局（源自 %s）" % self.header, ""]
        lines.append("Flash:")
        for name, base_m, size_m in self.regions()[0]:
            base, size = self.v(base_m), self.v(size_m)
            if size == 0:
                lines.append("  %-12s (未使用)" % name)
            else:
                lines.append("  %-12s 0x%08X ~ 0x%08X  (%d KB)"
                             % (name, base, base + size - 1, size // 1024))
        lines.append("RAM:")
        for name, base_m, size_m in self.regions()[1]:
            base, size = self.v(base_m), self.v(size_m)
            lines.append("  %-12s 0x%08X ~ 0x%08X  (%d KB)"
                         % (name, base, base + size - 1, size // 1024))
        lines.append("App 区: 槽位 %d 个，每槽 %d KB"
                     % (self.v("APP_MAX_COUNT"), self.v("APP_SLOT_SIZE") // 1024))
        lines.append("硬件兼容签名: 0x%08X" % self.v("SVCRT_HW_COMPAT_ID"))
        return "\n".join(lines)


# ---------------------------------------------------------------- 生成
BANNER = """\
; *************************************************************
; ***  本文件由 tools/gen_scatter.py 自动生成，请勿手工编辑  ***
; ***  配置源头: config/svcrt_partition.h                    ***
; ***  修改布局请改配置头，然后重新编译（编译前会自动重新生成） ***
; *************************************************************
"""


def emit(region_name, base, size, ram_base, ram_size, out, ccm=None, stack_size=0):
    """输出一个映像的分散加载描述

    栈的约定（必须与内核 svcrt_loader_start 严格一致）：
        栈顶 = ram_base + ram_size
        栈底 = 栈顶 - stack_size
    RW/ZI 区被限制在 [ram_base, 栈底)，所以编译期就撞不到栈；
    ARM_LIB_STACK 指向的正是内核会设置的栈顶，__main 设置 SP 后
    与内核推导值完全一致（不会出现“双份栈”）。
    """
    if stack_size > ram_size:
        raise SystemExit("RAM 区过小：栈 %d 字节超出区域 %d 字节" % (stack_size, ram_size))
    rw_size = ram_size - stack_size
    out.write(BANNER)
    out.write("\n")
    out.write("LR_%s 0x%08X 0x%08X  {\n" % (region_name, base, size))
    out.write("  ER_%s 0x%08X 0x%08X  {\n" % (region_name, base, size))
    out.write("   *.o (RESET, +First)          ; 启动入口向量表必须位于分区首地址\n")
    out.write("   *(InRoot$$Sections)\n")
    out.write("   .ANY (+RO)\n")
    out.write("   .ANY (+XO)\n")
    out.write("  }\n")
    out.write("  RW_%s 0x%08X 0x%08X  {   ; 上限已扣除栈区，编译期不会侵占栈\n"
              % (region_name, ram_base, rw_size))
    out.write("   .ANY (+RW +ZI)\n")
    out.write("  }\n")
    # 栈：RAM 区最高地址，向下生长（与内核 svcrt_loader_start 推导的栈顶一致）
    if stack_size:
        out.write("  ARM_LIB_STACK 0x%08X EMPTY -0x%X {   ; 栈 %d 字节（与内核推导的栈顶一致）\n  }\n"
                  % (ram_base + ram_size, stack_size, stack_size))
    # 不再生成 ARM_LIB_HEAP：堆会与受栈保护的 RW 区重叠。
    # 应用不应依赖 malloc；确需堆时请改由内核内存服务提供。
    if ccm and ccm[1] > 0:
        out.write("  RW_CCM 0x%08X 0x%08X  {   ; CCM/TCM 快速 RAM\n" % (ccm[0], ccm[1]))
        out.write("   .ANY (+RW +ZI)\n")
        out.write("  }\n")
    out.write("}\n")


def gen_target(layout, target, out_path):
    v = layout.v
    d = os.path.dirname(os.path.abspath(out_path))
    if d and not os.path.isdir(d):
        os.makedirs(d)

    ccm = (v("CHIP_CCM_BASE"), v("CHIP_CCM_SIZE"))
    with open(out_path, "w", encoding="utf-8") as out:
        if target == "kernel":
            emit("KERNEL", v("KERNEL_BASE"), v("KERNEL_SIZE"),
                 v("KERNEL_RAM_BASE"), v("KERNEL_RAM_SIZE"), out, ccm=ccm)
        elif target == "driver":
            emit("DRIVER", v("DRIVER_POOL_BASE"), v("DRIVER_POOL_SIZE"),
                 v("DRIVER_RAM_BASE"), v("DRIVER_RAM_SIZE"), out,
                 stack_size=v("DRIVER_TASK_STACK_SIZE"))
        elif target == "app":
            emit("APP", v("APP_SLOT0_BASE"), v("APP_SLOT0_SIZE"),
                 v("APP_RAM_BASE"), v("APP_RAM_SIZE"), out,
                 stack_size=v("APP_TASK_STACK_SIZE"))
        elif target == "boot":
            if v("BOOT_SIZE") <= 0:
                raise SystemExit("BOOT_SIZE 为 0，未划分 Bootloader 区（如需 Boot，请先在配置头中设置 BOOT_SIZE）")
            emit("BOOT", v("BOOT_BASE"), v("BOOT_SIZE"),
                 v("BOOT_RAM_BASE"), v("BOOT_RAM_SIZE"), out)
        else:
            raise SystemExit("未知 target: %s" % target)
    print("[gen_scatter] %-7s -> %s" % (target, out_path))


def main():
    ap = argparse.ArgumentParser(description="SVCrtOS 分散加载文件生成器")
    ap.add_argument("--target", choices=["boot", "kernel", "driver", "app", "all"])
    ap.add_argument("--output", help="输出文件路径；target=all 时视为输出目录")
    ap.add_argument("--header", default=DEFAULT_HEADER, help="分区配置头文件")
    ap.add_argument("--check", action="store_true", help="仅校验布局")
    ap.add_argument("--dump", action="store_true", help="打印当前布局")
    args = ap.parse_args()

    header = args.header
    if not os.path.isabs(header):
        header = os.path.join(REPO_ROOT, header)
    if not os.path.isfile(header):
        raise SystemExit("找不到分区配置头文件: %s" % header)

    layout = Layout(MacroEval(parse_defines(header)))
    layout.header = header

    if args.dump or args.check or not args.target:
        print(layout.dump())
        problems = layout.check()
        if problems:
            print("\n[配置错误]")
            for p in problems:
                print("  - " + p)
            return 1
        print("\n[检查通过] 分区无重叠、无越界")
        if not args.target:
            return 0

    problems = layout.check()
    if problems:
        for p in problems:
            print("[配置错误] " + p, file=sys.stderr)
        return 1

    if not args.output:
        raise SystemExit("缺少 --output")

    if args.target == "all":
        for t, name in (("kernel", "kernel.sct"), ("driver", "driver.sct"),
                        ("app", "app.sct")):
            gen_target(layout, t, os.path.join(args.output, name))
        if layout.v("BOOT_SIZE") > 0:
            gen_target(layout, "boot", os.path.join(args.output, "boot.sct"))
    else:
        gen_target(layout, args.target, args.output)
    return 0


if __name__ == "__main__":
    sys.exit(main())
