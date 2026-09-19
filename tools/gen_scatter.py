#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
SVCrtOS 分散加载文件（.sct）生成器
==================================

单一配置源头：config/svcrt_partition.h
本脚本读取其中的宏（含自动推导的地址），为各工程生成对应的 .sct，
保证「地址只定义一次，其余全部派生」，且生成结果一定不自相重叠。

布局模型（v4 起）：
    Flash:  BOOT → KERNEL → CONFIG → IMAGE_POOL（统一镜像池，App 与驱动共用）
    RAM:    SHARE_RAM → KERNEL_RAM → SLOT_RAM（所有镜像按槽等分）

镜像池按「分配单元」分配，一个单元 = 一个物理擦除扇区（IMAGE_POOL_SECTOR）。
一个镜像占 2^n 个单元，基址按自身跨度对齐——这既是 MPU region 的硬约束，
也保证擦除一个镜像不会连带擦掉相邻镜像。

用法：
    # 内核
    python tools/gen_scatter.py --target kernel --output build/kernel.sct

    # 一个镜像（默认单元 0、1 个单元、App）
    python tools/gen_scatter.py --target image --unit 1 --output build/app_demo.sct
    python tools/gen_scatter.py --target image --unit 0 --units 1 --type driver \
        --output build/drv_demo.sct

    # 开发调试裸镜像（无 256 字节头，负载直接落在单元基址）
    python tools/gen_scatter.py --target image --unit 1 --raw \
        --output build/app_demo_dev.sct

    # 按开发槽位表条目生成（条目给出 单元 / 单元数 / 类型）
    python tools/gen_scatter.py --target image --dev-slot 0 --raw --output build/dev0.sct

    # 一次生成内核 + 开发槽位表里所有条目的镜像布局
    python tools/gen_scatter.py --target all --output build

    # 校验当前配置（展开所有关键宏并做重叠检查）
    python tools/gen_scatter.py --check

可选参数：
    --header <path>   指定分区头文件（默认 config/svcrt_partition.h），便于做配置变更演练
    --ccm             为内核附加 CCM/TCM RAM 段（默认自动：CHIP_CCM_SIZE > 0 时附加）

设计约束：
    1. Loader / App 工程的 .sct 只覆盖自己的单元区间，绝不越界；
    2. 各映像的 RW 段使用各自独立的 RAM 窗口（不能共用，否则互相覆盖）；
    3. 生成的 .sct 属于构建产物，建议放在 build/ 且不纳入版本库；
    4. 镜像默认按「.svcapp 安装路径」生成：负载链接基址 =
       单元基址 + APP_IMAGE_HEADER_SIZE（前 256 字节留给镜像头）；
       加 --raw 则按「开发调试裸镜像路径」生成，负载直接位于单元基址。
"""

from __future__ import print_function

import argparse
import os
import re
import sys

def auto_int(text):
    """接受十进制与 0x 十六进制（命令行里的地址/偏移量习惯写十六进制）"""
    return int(text, 0)

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
    """惰性求值的宏展开器：支持宏之间相互引用（如 SLOT_RAM_TOTAL）"""

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

    # ---- 池 ----
    def unit_base(self, unit):
        """第 unit 个分配单元的基址"""
        return self.v("IMAGE_POOL_BASE") + unit * self.v("IMAGE_POOL_SECTOR")

    def units(self):
        return self.v("IMAGE_POOL_UNITS")

    def dev_slots(self):
        """开发槽位表：[(条目号, 起始单元, 单元数, 类型)]，类型 0 表示未使用"""
        out = []
        for i in range(self.v("SVCRT_DEV_SLOT_MAX")):
            unit = self.v("SVCRT_DEV_SLOT%d_UNIT" % i)
            units = self.v("SVCRT_DEV_SLOT%d_UNITS" % i)
            stype = self.v("SVCRT_DEV_SLOT%d_TYPE" % i)
            out.append((i, unit, units, stype))
        return out

    def nominal_rom_base(self):
        """镜像负载的 ROM 链接标称基址（见 config/svcrt_partition.h）"""
        return self.v("SVCRT_APP_NOMINAL_ROM_BASE")

    def nominal_ram_base(self):
        """镜像 RW/ZI 的 RAM 链接标称基址"""
        return self.v("SVCRT_APP_NOMINAL_RAM_BASE")

    def dev_ram_base(self, unit):
        """开发调试裸镜像的 RAM 窗口基址

        裸镜像走固定地址路径，没有伙伴分配器，因此按「每个单元一个
        SLOT_RAM_MAX_BLOCK 大小的窗口」静态划分（序号 = 起始单元号）。
        仅当 APP_ALLOW_RAW_IMAGE=1 时才有意义。
        """
        return self.v("SLOT_RAM_BASE") + (unit % self.v("SVCRT_DEV_SLOT_MAX")) \
            * self.v("SVCRT_DEV_RAM_WINDOW")

    def regions(self):
        """返回 [(名称, 基址, 大小)] 两组表：Flash 与 RAM"""
        flash = [
            ("BOOT", "BOOT_BASE", "BOOT_SIZE"),
            ("KERNEL", "KERNEL_BASE", "KERNEL_SIZE"),
            ("CONFIG", "CONFIG_BASE", "CONFIG_SIZE"),
            ("IMAGE_POOL", "IMAGE_POOL_BASE", "IMAGE_POOL_SIZE"),
        ]
        ram = [
            ("SHARE_RAM", "SHARE_RAM_BASE", "SHARE_RAM_SIZE"),
            ("KERNEL_RAM", "KERNEL_RAM_BASE", "KERNEL_RAM_SIZE"),
            ("SLOT_RAM", "SLOT_RAM_BASE", "SLOT_RAM_TOTAL"),
        ]
        return flash, ram

    def check(self):
        """检查各分区是否越界与重叠 + 池的分配约束，返回问题列表"""
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

        # 池的分配约束：单元地址必须与芯片扇区对齐，否则擦除会破坏相邻镜像
        sector = self.v("IMAGE_POOL_SECTOR")
        if self.v("IMAGE_POOL_SIZE") % sector != 0:
            problems.append("IMAGE_POOL_SIZE(%d) 不是单元大小(%d) 的整数倍"
                            % (self.v("IMAGE_POOL_SIZE"), sector))
        if self.v("KERNEL_SIZE") % sector != 0:
            problems.append("KERNEL_SIZE(%d) 不是单元大小(%d) 的整数倍：池基址会落在扇区中间"
                            % (self.v("KERNEL_SIZE"), sector))
        # 配置区：一个物理扇区，紧跟内核之后，池基址由它决定
        if self.v("CONFIG_SIZE") > 0:
            if self.v("CONFIG_SIZE") % sector != 0:
                problems.append("CONFIG_SIZE(%d) 不是单元大小(%d) 的整数倍"
                                % (self.v("CONFIG_SIZE"), sector))
            if self.v("CONFIG_BASE") != self.v("KERNEL_BASE") + self.v("KERNEL_SIZE"):
                problems.append("CONFIG_BASE(0x%08X) 不紧跟内核之后：池基址会错位"
                                % self.v("CONFIG_BASE"))
            if self.v("CONFIG_BASE") % sector != 0:
                problems.append("CONFIG_BASE(0x%08X) 未按扇区(%d) 对齐"
                                % (self.v("CONFIG_BASE"), sector))
        if self.v("IMAGE_POOL_UNITS") > self.v("SLOT_MAX"):
            problems.append("池单元数 %d 超过槽位记录上限 SLOT_MAX=%d"
                            % (self.v("IMAGE_POOL_UNITS"), self.v("SLOT_MAX")))
        if not self._is_pow2(self.v("KERNEL_SIZE")):
            problems.append("KERNEL_SIZE(%d) 不是 2 的幂：内核 Flash 窗口无法构成单个 MPU region"
                            % self.v("KERNEL_SIZE"))

        # RAM 池：总大小/最小块/最大块必须都是 2 的幂且能装下任意镜像栈
        for m in ("SLOT_RAM_TOTAL", "SLOT_RAM_MIN_BLOCK", "SLOT_RAM_MAX_BLOCK"):
            if not self._is_pow2(self.v(m)):
                problems.append("%s(%d) 不是 2 的幂：伙伴分配器无法切分" % (m, self.v(m)))
        if self.v("SLOT_RAM_MIN_BLOCK") > self.v("SLOT_RAM_MAX_BLOCK"):
            problems.append("SLOT_RAM_MIN_BLOCK 大于 SLOT_RAM_MAX_BLOCK")
        if self.v("SLOT_RAM_TOTAL") < self.v("SLOT_RAM_MAX_BLOCK"):
            problems.append("SLOT_RAM_TOTAL(%d) 小于单块上限 SLOT_RAM_MAX_BLOCK(%d)"
                            % (self.v("SLOT_RAM_TOTAL"), self.v("SLOT_RAM_MAX_BLOCK")))
        if self.v("APP_TASK_STACK_SIZE") > self.v("SLOT_RAM_MAX_BLOCK"):
            problems.append("APP_TASK_STACK_SIZE 超过单块上限，App 镜像拿不到足够的 RAM 块")
        if self.v("DRIVER_TASK_STACK_SIZE") > self.v("SLOT_RAM_MIN_BLOCK"):
            problems.append("DRIVER_TASK_STACK_SIZE 超过最小块，驱动镜像至少需要更大的一块")
        # 开发调试裸镜像按「每槽位一个固定窗口」静态划分 RAM，窗口必须落在池内
        if self.v("SVCRT_DEV_SLOT_MAX") * self.v("SVCRT_DEV_RAM_WINDOW") > self.v("SLOT_RAM_TOTAL"):
            problems.append("开发裸镜像 RAM 窗口总量 %d 字节超出 RAM 池 %d 字节"
                            % (self.v("SVCRT_DEV_SLOT_MAX") * self.v("SVCRT_DEV_RAM_WINDOW"),
                               self.v("SLOT_RAM_TOTAL")))
        if self.v("SVCRT_DEV_RAM_WINDOW") < self.v("DRIVER_TASK_STACK_SIZE"):
            problems.append("SVCRT_DEV_RAM_WINDOW 小于驱动任务栈：驱动裸镜像会找不到 RAM 窗口")
        for idx, unit, _units, stype in self.dev_slots():
            if stype == 0:
                continue
            if self.dev_ram_base(unit) + self.v("SVCRT_DEV_RAM_WINDOW") > \
                    self.v("SLOT_RAM_BASE") + self.v("SLOT_RAM_TOTAL"):
                problems.append("开发槽位 %d 的裸镜像 RAM 窗口(0x%08X) 超出 RAM 池"
                                % (idx, self.dev_ram_base(unit)))

        # 开发槽位表：单元数必须是 2 的幂，区间必须落在池内且互不重叠
        used_units = []
        for idx, unit, units, stype in self.dev_slots():
            if stype == 0:
                continue
            if not self._is_pow2(units):
                problems.append("开发槽位 %d 的单元数 %d 不是 2 的幂" % (idx, units))
            if unit + units > self.units():
                problems.append("开发槽位 %d(单元 %d+%d) 超出池范围(%d 个单元)"
                                % (idx, unit, units, self.units()))
            if (unit % units) != 0:
                problems.append("开发槽位 %d 的起始单元 %d 未按自身跨度 %d 对齐"
                                % (idx, unit, units))
            used_units.append((idx, unit, units))
        used_units.sort(key=lambda x: x[1])
        for i in range(len(used_units) - 1):
            i1, u1, n1 = used_units[i]
            i2, u2, _ = used_units[i + 1]
            if u1 + n1 > u2:
                problems.append("开发槽位 %d 与 %d 区间重叠" % (i1, i2))
        return problems

    @staticmethod
    def _is_pow2(n):
        return n > 0 and (n & (n - 1)) == 0

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
        lines.append("")
        lines.append("统一镜像池: %d 个单元 x %d KB @ 0x%08X（最多 %d 个镜像）"
                     % (self.units(), self.v("IMAGE_POOL_SECTOR") // 1024,
                        self.v("IMAGE_POOL_BASE"), self.v("SLOT_MAX")))
        lines.append("  单元基址:")
        for u in range(self.units()):
            lines.append("    [%d] 0x%08X" % (u, self.unit_base(u)))
        lines.append("开发槽位表（裸镜像路径，config/svcrt_partition.h 声明）:")
        for idx, unit, units, stype in self.dev_slots():
            if stype == 0:
                lines.append("  [%d] (未使用)" % idx)
            else:
                lines.append("  [%d] 单元 %d + %d 个单元 @ 0x%08X  类型 %-4s RAM 窗口 0x%08X"
                             % (idx, unit, units, self.unit_base(unit),
                                {1: "App", 2: "驱动"}.get(stype, "?"),
                                self.dev_ram_base(unit)))
        lines.append("镜像 RAM 池: 总 %d KB，块范围 %d K ~ %d K @ 0x%08X（伙伴分配）"
                     % (self.v("SLOT_RAM_TOTAL") // 1024,
                        self.v("SLOT_RAM_MIN_BLOCK") // 1024,
                        self.v("SLOT_RAM_MAX_BLOCK") // 1024,
                        self.v("SLOT_RAM_BASE")))
        lines.append("开发裸镜像 RAM 窗口: %d 个 x %d KB @ 0x%08X"
                     % (self.v("SVCRT_DEV_SLOT_MAX"),
                        self.v("SVCRT_DEV_RAM_WINDOW") // 1024,
                        self.v("SLOT_RAM_BASE")))
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

def image_region(layout, unit, units, img_type):
    """把「起始单元 / 单元数 / 类型」换算成 Flash 区间与 RAM 窗口

    返回 (负载链接基址, 负载可用大小, ram_base, ram_size, stack_size)；
    负载基址由调用方按 raw 决定是否加上镜像头长度。
    """
    v = layout.v
    sector = v("IMAGE_POOL_SECTOR")

    if not Layout._is_pow2(units):
        raise SystemExit("单元数 %d 不是 2 的幂：MPU region 必须覆盖整个镜像且大小是 2 的幂" % units)
    if (unit % units) != 0:
        raise SystemExit("起始单元 %d 未按自身跨度 %d 对齐" % (unit, units))
    if unit + units > layout.units():
        raise SystemExit("起始单元 %d + %d 个单元超出池范围（池共 %d 个单元）"
                         % (unit, units, layout.units()))

    base = layout.unit_base(unit)
    size = units * sector
    ram_base = layout.dev_ram_base(unit)
    ram_size = v("SVCRT_DEV_RAM_WINDOW")
    stack_size = v("APP_TASK_STACK_SIZE") if img_type == "app" else v("DRIVER_TASK_STACK_SIZE")
    return base, size, ram_base, ram_size, stack_size

def gen_target(layout, target, out_path, raw=False, unit=0, units=1, img_type="app",
               nominal=False, ram_size=None, rom_delta=0, ram_delta=0):
    """生成一个映像的分散加载文件

    target="kernel"：内核固件区。
    target="image" ：镜像池中的一块镜像区间（App 或驱动，共用同一条路径）。
    target="boot"  ：Bootloader 区（BOOT_SIZE > 0 时才有意义）。

    镜像有三条互不兼容的链接路径：

    nominal=True（动态装载路径）：按标称基址链接（ROM = 池基址 + 头长，
        RAM = RAM 池基址），运行期由内核把镜像搬到任意 2 的幂对齐落点，
        并按重定位表把绝对地址改过来。rom_delta/ram_delta 是「差异链接」
        用的额外偏移：打包工具用同一份代码链接四次（0、ROM+delta、
        RAM+delta、验证用），再逐字节比对得出重定位表。ram_size 给出
        RW/ZI + 栈 所需的 RAM 字节数（必须是 2 的幂且落在块范围内）。

    raw=False（固定落点安装路径）：按 .sct 声明的单元区间链接——区间前
        APP_IMAGE_HEADER_SIZE 字节留给镜像头，负载链接基址 = 区间基址 + 头长。
        仅用于「已知固定落点」的场景，与动态装载路径不通用。

    raw=True（开发调试裸镜像路径）：无镜像头也没有重定位表，负载直接位于
        单元基址，ROM/RAM 都是固定地址，专供 MDK 直接烧录 + 断点调试。
    """
    v = layout.v
    d = os.path.dirname(os.path.abspath(out_path))
    if d and not os.path.isdir(d):
        os.makedirs(d)

    def image_layout(base, size):
        """把「区间基址 / 区间大小」换算成「负载链接基址 / 负载可用大小」"""
        if raw:
            return base, size
        hdr = v("APP_IMAGE_HEADER_SIZE")
        if size <= hdr:
            raise SystemExit("区间过小：0x%X 字节装不下 %d 字节镜像头" % (size, hdr))
        return base + hdr, size - hdr

    ccm = (v("CHIP_CCM_BASE"), v("CHIP_CCM_SIZE"))
    with open(out_path, "w", encoding="utf-8") as out:
        if target == "kernel":
            emit("KERNEL", v("KERNEL_BASE"), v("KERNEL_SIZE"),
                 v("KERNEL_RAM_BASE"), v("KERNEL_RAM_SIZE"), out, ccm=ccm)
        elif target == "image" and nominal:
            hdr = v("APP_IMAGE_HEADER_SIZE")
            load_base = v("SVCRT_APP_NOMINAL_ROM_LOAD") + rom_delta
            # 上界只用于让链接器有足够空间；真正的跨度上限由打包工具按
            # 「总长 + 重定位表 + 2 的幂对齐」校核（见 tools/pack_app.py）。
            load_size = v("IMAGE_POOL_USABLE_SIZE") - hdr - rom_delta
            if load_size <= 0:
                raise SystemExit("rom_delta=%d 已超出镜像池可用空间" % rom_delta)
            ram_base = v("SVCRT_APP_NOMINAL_RAM_BASE") + ram_delta
            if ram_size is None:
                ram_size = v("SLOT_RAM_MAX_BLOCK")
            if not Layout._is_pow2(ram_size):
                raise SystemExit("ram_size=%d 不是 2 的幂" % ram_size)
            if ram_size < v("SLOT_RAM_MIN_BLOCK") or ram_size > v("SLOT_RAM_MAX_BLOCK"):
                raise SystemExit("ram_size=%d 超出块范围 %d ~ %d"
                                 % (ram_size, v("SLOT_RAM_MIN_BLOCK"),
                                    v("SLOT_RAM_MAX_BLOCK")))
            stack_size = v("APP_TASK_STACK_SIZE") if img_type == "app" \
                else v("DRIVER_TASK_STACK_SIZE")
            if stack_size >= ram_size:
                raise SystemExit("ram_size=%d 装不下 %d 字节栈（RW/ZI 还要占地方）"
                                 % (ram_size, stack_size))
            emit("IMG", load_base, load_size, ram_base, ram_size, out,
                 stack_size=stack_size)
        elif target == "image":
            base, size, ram_base, ram_size, stack_size = image_region(layout, unit, units, img_type)
            load_base, load_size = image_layout(base, size)
            emit("IMG", load_base, load_size, ram_base, ram_size, out,
                 stack_size=stack_size)
        elif target == "boot":
            if v("BOOT_SIZE") <= 0:
                raise SystemExit("BOOT_SIZE 为 0，未划分 Bootloader 区（如需 Boot，请先在配置头中设置 BOOT_SIZE）")
            emit("BOOT", v("BOOT_BASE"), v("BOOT_SIZE"),
                 v("BOOT_RAM_BASE"), v("BOOT_RAM_SIZE"), out)
        else:
            raise SystemExit("未知 target: %s" % target)
    tag = " (raw)" if raw else (" (nominal)" if nominal else "")
    print("[gen_scatter] %-7s%-10s -> %s" % (target, tag, out_path))

def image_name(unit, units, img_type):
    return "img_u%d%s_%s" % (unit, ("x%d" % units) if units > 1 else "", img_type)

def main():
    ap = argparse.ArgumentParser(description="SVCrtOS 分散加载文件生成器")
    ap.add_argument("--target", choices=["boot", "kernel", "image", "all"])
    ap.add_argument("--output", help="输出文件路径；target=all 时视为输出目录")
    ap.add_argument("--header", default=DEFAULT_HEADER, help="分区配置头文件")
    ap.add_argument("--raw", action="store_true",
                    help="生成开发调试用的裸镜像布局（负载直接位于单元基址，无镜像头）")
    ap.add_argument("--nominal", action="store_true",
                    help="按链接标称基址生成（动态装载路径；负载基址 = 池基址 + 头长）")
    ap.add_argument("--ram-size", type=auto_int, default=None,
                    help="标称路径下镜像 RW/ZI + 栈 的 RAM 字节数（2 的幂，且需在块范围内）")
    ap.add_argument("--rom-delta", type=auto_int, default=0,
                    help="链接基址额外偏移量（差异链接用，动态装载才需要）")
    ap.add_argument("--ram-delta", type=auto_int, default=0,
                    help="RAM 链接基址额外偏移量（差异链接用，动态装载才需要）")
    ap.add_argument("--unit", type=auto_int, default=0, help="起始分配单元号（默认 0）")
    ap.add_argument("--units", type=auto_int, default=1, help="占用单元数，必须是 2 的幂（默认 1）")
    ap.add_argument("--type", choices=["app", "driver"], default="app",
                    help="镜像类型，决定任务栈大小（默认 app）")
    ap.add_argument("--dev-slot", type=int, default=None,
                    help="按开发槽位表条目号取 单元/单元数/类型（0 ~ SVCRT_DEV_SLOT_MAX-1）")
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
        print("\n[检查通过] 分区无重叠、无越界，池与开发槽位表约束满足")
        if not args.target:
            return 0

    problems = layout.check()
    if problems:
        for p in problems:
            print("[配置错误] " + p, file=sys.stderr)
        return 1

    if not args.output:
        raise SystemExit("缺少 --output")

    # --dev-slot 是 --unit/--units/--type 的快捷方式（取自配置头里的开发槽位表）
    if args.dev_slot is not None:
        entries = layout.dev_slots()
        if args.dev_slot < 0 or args.dev_slot >= len(entries):
            raise SystemExit("--dev-slot 超出范围（0 ~ %d）" % (len(entries) - 1))
        idx, args.unit, args.units, stype = entries[args.dev_slot]
        if stype == 0:
            raise SystemExit("开发槽位 %d 未使用（类型为 0）" % idx)
        args.type = "driver" if stype == 2 else "app"

    if args.target == "all":
        gen_target(layout, "kernel", os.path.join(args.output, "kernel.sct"))
        # 每个开发槽位条目生成两套：安装路径（.svcapp）与开发调试裸镜像路径
        for idx, unit, units, stype in layout.dev_slots():
            if stype == 0:
                continue
            img_type = "driver" if stype == 2 else "app"
            name = image_name(unit, units, img_type)
            gen_target(layout, "image", os.path.join(args.output, name + "_nom.sct"),
                       nominal=True, img_type=img_type, ram_size=args.ram_size)
            gen_target(layout, "image", os.path.join(args.output, name + "_dev.sct"),
                       raw=True, unit=unit, units=units, img_type=img_type)
        if layout.v("BOOT_SIZE") > 0:
            gen_target(layout, "boot", os.path.join(args.output, "boot.sct"))
    else:
        gen_target(layout, args.target, args.output, raw=args.raw,
                   unit=args.unit, units=args.units, img_type=args.type,
                   nominal=args.nominal, ram_size=args.ram_size,
                   rom_delta=args.rom_delta, ram_delta=args.ram_delta)
    return 0

if __name__ == "__main__":
    sys.exit(main())
