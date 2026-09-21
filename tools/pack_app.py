#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
SVCrtOS 镜像打包工具（App / 驱动，动态装载路径）
================================================

把 App / 驱动工程的编译产物打包成 SVCrtOS 可加载镜像（.svcapp）：

    [0, 256)                     镜像头
    [256, 256 + N*4)             重定位表（N 项，每项 4 字节）
    [256 + N*4, ...)             负载（链接基址 = nominal_base）

单一地址源头：config/svcrt_partition.h
    标称基址、池布局、RAM 池、hw_compat_id 全部由配置头派生，本脚本不硬编码
    地址。运行期落点由内核分配器决定，与打包无关。

为什么需要重定位表
------------------
镜像被链接在标称基址上，实际却会被内核装到池里任意一个 2 的幂对齐的空位上。
代码里的绝对地址（函数指针、跳转表、指向自身 RAM 的指针……）必须随落点平移，
所以打包时必须找出「哪些 32 位字是绝对地址」。

怎么找出绝对地址（差分链接）
----------------------------
同一份源码链接四次，只有链接基址不同：

    A = 标称基址                       （打包用的那一份）
    B = ROM 基址 + delta
    C = RAM 基址 + delta
    D = ROM 基址 + 2*delta, RAM + delta （验证用）

逐字比对得到：
    A→B 平移 delta 且 A→C 不变  →  ROM 类表项
    A→C 平移 delta 且 A→B 不变  →  RAM 类表项
    A、B、C 完全相同              →  不是绝对地址，不登记
    其余情况                      →  打包失败（镜像不可重定位）

再把生成出来的表应用回 A，必须逐字节等于 D；不等就说明表是错的。
因此「镜像能不能搬到任意 2 的幂对齐落点」这件事在打包阶段就被证明，
而不是留到板子上随机崩溃。

用法
----
    # 全自动：改 .sct -> 编译四遍 -> 差分 -> 打包 -> 还原开发用 .sct
    python tools/pack_app.py --project example/stm32f427/app_sdk/APP_DEMO/MDK-ARM/app_demo.uvprojx \
        --type app --name APP_DEMO --version 1.0.0 \
        --out build/APP_DEMO/APP_DEMO.svcapp

    # 离线：用已有的四份 bin 打包（CI / 回归用，不需要 Keil）
    python tools/pack_app.py --bin-a a.bin --bin-b b.bin --bin-c c.bin --bin-d d.bin \
        --axf app_demo.axf --type app --name APP_DEMO --version 1.0.0 --out x.svcapp

    # 查看 / 校验
    python tools/pack_app.py --info   build/APP_DEMO/APP_DEMO.svcapp
    python tools/pack_app.py --verify build/APP_DEMO/APP_DEMO.svcapp

工程侧约定（.uvprojx 的 BeforeMake 钩子）
----------------------------------------
钩子调用 tools/gen_app_sct.py，该脚本读环境变量决定生成哪一套 .sct：

    SVCRT_SCT_MODE = dev（默认） → 固定落点裸镜像 .sct，供 MDK 下载 + 断点调试
                   = nominal     → 标称基址 .sct，供本工具打包
    SVCRT_ROM_DELTA / SVCRT_RAM_DELTA  差异化链接偏移（仅 nominal 模式）

本脚本在每遍编译前设置这些变量，并在结束后还原成 dev，保证开发闭环不受影响。
"""

from __future__ import print_function

import argparse
import io
import json
import os
import struct
import subprocess
import sys
import zlib

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO_ROOT, "tools"))

import gen_scatter  # noqa: E402  复用宏解析 / 布局模型，保证地址只有一个源头

# ---------------------------------------------------------------- 常量
MAGIC = 0x53564341              # "SVCA"
HEADER_SIZE = 256
TYPE_APP = 1
TYPE_DRIVER = 2
TYPE_NAME = {TYPE_APP: "app", TYPE_DRIVER: "driver"}

VERSION_FILE = "app_version.txt"    # 版本号唯一来源（与工程同级）
SIG_OFFSET = 32                 # signature[64]
FLAGS_OFFSET = 96
STATE_OFFSET = 100
IMAGE_ID_OFFSET = 104
RAM_SIZE_OFFSET = 108
RELOC_OFFSET_OFF = 112
RELOC_COUNT_OFF = 116
RELOC_KIND_OFF = 120
PAYLOAD_OFFSET_OFF = 124
NOMINAL_RAM_OFFSET = 128
RESERVED_OFFSET = 132
# 预留区（清单写在这里）与头里最后一个字段 runtime_ram_base 的关系：
# 清单从预留区头部向后增长，runtime_ram_base 钉在头的末尾 4 字节，两块不重叠。
# 见 kernelsrc/include/svcrt_app_image.h 的 SVCRT_APP_OFF_RUNTIME_RAM_BASE。
RESERVED_SIZE = 120
MANIFEST_MAX = RESERVED_SIZE - 1
RUNTIME_RAM_OFFSET = 252        # runtime_ram_base：安装时由内核写，打包产物恒为 0
                                # （计算 CRC 时这四个字节按 0 参与，同代码里“喂 4 个零”）

CRC32_OFFSET = 28               # 计算 CRC 时本字段按 0
STATE_CRC_OFFSET = 100          # 计算 CRC 时本字段按 0

RELOC_SIZE = 4
RELOC_KIND_ROM = 0
RELOC_KIND_RAM = 1
RELOC_KIND_ROM_MOVW = 2
RELOC_KIND_RAM_MOVW = 3
RELOC_KIND_MASK = 3

RELOC_MOV_SIZE = 8

# 表项编码版本（写进镜像头 reloc_kind 字段）：偏移按半字存放
#     表项 = (off / 2) << 2 | kind
# 不能按字节直接存 off：低 2 位被 kind 占用，而 Thumb 的 32 位指令只保证半字
# 对齐，MOVW/MOVT 指令对完全可能落在 2 mod 4 的偏移上，按字节存会把它静默
# 截掉 2 字节——内存里的表是对的（第四遍验证照过），写进镜像再读出来才错。
RELOC_TABLE_KIND = 1


def reloc_entry(off, kind):
    """把 (偏移, 类型) 组装成表项（偏移按半字存放）"""
    if off < 0 or (off & 1):
        raise PackError("重定位偏移 0x%X 不是半字对齐" % off)
    return ((off >> 1) << 2) | (kind & RELOC_KIND_MASK)


def reloc_off(entry):
    """从表项取出偏移"""
    return (entry & ~RELOC_KIND_MASK) >> 1


def reloc_kind(entry):
    """从表项取出类型"""
    return entry & RELOC_KIND_MASK

# ---- Thumb-2 指令内的地址：MOVW/MOVT 立即数对 ----
# 编译器会把一部分 32 位绝对地址拆成「MOVW 低 16 位 + MOVT 高 16 位」两条指令，
# 这类位置在四遍链接之间不是「整字 +delta」，只能按指令解码后再比较；装载期
# 也要按同样方式重新编码。Thumb 指令是半字对齐、长度 2 或 4 字节的，所以扫描
# 必须按指令长度走，不能按 4 字节字走。
def _thumb_is32(h):
    """半字是否是 32 位 Thumb 指令的前缀半字"""
    return (h & 0xF800) in (0xE800, 0xF000, 0xF800)

def _mov_kind(h):
    """前缀半字是不是 MOVW / MOVT（'w' / 't'），不是则 None"""
    if (h & 0xF800) != 0xF000:
        return None
    t = (h >> 4) & 0x3F
    if t == 0x24:
        return "w"
    if t == 0x2C:
        return "t"
    return None

def _mov_decode(h1, h2):
    """解出 (Rd, imm16)：h1 是前缀半字，h2 是第二个半字"""
    rd = (h2 >> 8) & 0xF
    imm4 = h1 & 0xF
    i = (h1 >> 10) & 1
    imm3 = (h2 >> 12) & 7
    imm8 = h2 & 0xFF
    return rd, (imm4 << 12) | (i << 11) | (imm3 << 8) | imm8

def _mov_encode(h1, h2, imm16):
    """把新的 imm16 编回这一对半字，Rd 与 MOVW/MOVT 种类都不变"""
    rd = (h2 >> 8) & 0xF
    imm4 = (imm16 >> 12) & 0xF
    i = (imm16 >> 11) & 1
    imm3 = (imm16 >> 8) & 7
    imm8 = imm16 & 0xFF
    return (h1 & 0xFBF0) | (i << 10) | imm4, (imm3 << 12) | (rd << 8) | imm8

def _u16(buf, pos):
    return struct.unpack_from("<H", buf, pos)[0]

def _u32(buf, pos):
    return struct.unpack_from("<I", buf, pos)[0]

def _read_mov_pair(buf, pos):
    """pos 处若是相邻的 MOVW+MOVT（同 Rd），返回拼出的 32 位值，否则 None"""
    if pos + 8 > len(buf):
        return None
    h1w = _u16(buf, pos)
    h1t = _u16(buf, pos + 4)
    if _mov_kind(h1w) != "w" or _mov_kind(h1t) != "t":
        return None
    rd_w, imm_w = _mov_decode(h1w, _u16(buf, pos + 2))
    rd_t, imm_t = _mov_decode(h1t, _u16(buf, pos + 6))
    if rd_w != rd_t:
        return None
    return (imm_t << 16) | imm_w

def _write_mov_pair(buf, pos, value):
    """把 32 位值重新编回 pos 处的 MOVW/MOVT 对"""
    n1w, n2w = _mov_encode(_u16(buf, pos), _u16(buf, pos + 2), value & 0xFFFF)
    n1t, n2t = _mov_encode(_u16(buf, pos + 4), _u16(buf, pos + 6), (value >> 16) & 0xFFFF)
    struct.pack_into("<HHHH", buf, pos, n1w, n2w, n1t, n2t)

def apply_reloc_entry(buf, pos, kind, delta):
    """在负载缓冲 buf 的 pos 处应用一项重定位

    @param kind  RELOC_KIND_*（MOVW 类按指令重新编码，其余按整字相加）
    @param delta 地址增量（可能是负向的，用 32 位模加）
    """
    if kind in (RELOC_KIND_ROM, RELOC_KIND_RAM):
        struct.pack_into("<I", buf, pos, (_u32(buf, pos) + delta) & 0xFFFFFFFF)
        return
    value = _read_mov_pair(buf, pos)
    if value is None:
        raise PackError("负载偏移 0x%X 处不是 MOVW/MOVT 立即数对，无法按指令重定位" % pos)
    _write_mov_pair(buf, pos, (value + delta) & 0xFFFFFFFF)

FLAG_AUTOSTART = 1 << 0
FLAG_KNOWN_MASK = FLAG_AUTOSTART

DEFAULT_UV4 = r"D:\Keil_v5\UV4\UV4.exe"
FROMELF_CANDIDATES = [r"D:\Keil_v5\ARM\ARMCC\bin\fromelf.exe",
                      r"D:\Keil_v5\ARM\ARMCLANG\bin\fromelf.exe"]


class PackError(Exception):
    """打包流程中的可预期失败（打印成因后直接退出，不抛栈）"""
    pass


# ---------------------------------------------------------------- ELF32 解析
class ElfError(Exception):
    pass


class Elf(object):
    """极简 ELF32 解析器（只取程序头与符号表，不依赖 arm 工具链）"""

    ELFCLASS32, ELFDATA2LSB = 1, 1

    def __init__(self, path):
        with open(path, "rb") as f:
            self.data = f.read()
        d = self.data
        if len(d) < 52 or d[:4] != b"\x7fELF":
            raise ElfError("不是 ELF 文件: %s" % path)
        if d[4] != self.ELFCLASS32:
            raise ElfError("仅支持 ELF32（Keil AC5/AC6 的 .axf 均为 ELF32）")
        if d[5] != self.ELFDATA2LSB:
            raise ElfError("仅支持小端 ELF")

        (self.e_type, self.e_machine, _ver, self.e_entry, self.e_phoff, self.e_shoff,
         self.e_flags, self.e_ehsize, self.e_phentsize, self.e_phnum,
         self.e_shentsize, self.e_shnum, self.e_shstrndx) = \
            struct.unpack_from("<HHIIIIIHHHHHH", d, 16)

        self.phdrs = [self._phdr(i) for i in range(self.e_phnum)]
        self.shdrs = [self._shdr(i) for i in range(self.e_shnum)]

    def _phdr(self, i):
        off = self.e_phoff + i * self.e_phentsize
        p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, _al = \
            struct.unpack_from("<IIIIIIII", self.data, off)
        return dict(type=p_type, offset=p_offset, vaddr=p_vaddr, paddr=p_paddr,
                    filesz=p_filesz, memsz=p_memsz, flags=p_flags)

    def _shdr(self, i):
        off = self.e_shoff + i * self.e_shentsize
        (sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size, sh_link,
         sh_info, sh_addralign, sh_entsize) = struct.unpack_from("<IIIIIIIIII", self.data, off)
        return dict(name=sh_name, type=sh_type, flags=sh_flags, addr=sh_addr,
                    offset=sh_offset, size=sh_size, link=sh_link, info=sh_info,
                    addralign=sh_addralign, entsize=sh_entsize)

    def _cstr(self, off, table_off):
        end = self.data.find(b"\x00", table_off + off)
        return self.data[table_off + off:end].decode("utf-8", "replace")

    def symbols(self):
        """返回 {符号名: 地址}（已去掉 Thumb 标志位）"""
        out = {}
        for sh in self.shdrs:
            if sh["type"] != 2 or sh["entsize"] == 0:      # SHT_SYMTAB
                continue
            if sh["link"] >= len(self.shdrs):
                continue
            strtab = self.shdrs[sh["link"]]
            n = sh["size"] // sh["entsize"]
            for i in range(n):
                off = sh["offset"] + i * sh["entsize"]
                if off + sh["entsize"] > len(self.data):
                    break
                st_name, st_value, _st_size, _info, _other, st_shndx = \
                    struct.unpack_from("<IIIBBH", self.data, off)
                if st_name == 0 or st_shndx == 0:
                    continue
                name = self._cstr(st_name, strtab["offset"])
                if name and name not in out:
                    out[name] = st_value & ~1               # 去掉 Thumb 位
        return out

    def flash_base(self):
        """所有 PT_LOAD 段中最小的物理地址（= 负载链接基址）"""
        segs = [p for p in self.phdrs if p["type"] == 1 and p["filesz"] > 0]
        if not segs:
            raise ElfError("ELF 中没有可加载段")
        return min(p["paddr"] for p in segs)


# ---------------------------------------------------------------- 布局 / 版本
def load_layout(header_path):
    if not os.path.isabs(header_path):
        header_path = os.path.join(REPO_ROOT, header_path)
    if not os.path.isfile(header_path):
        raise PackError("找不到分区配置头文件: %s" % header_path)
    layout = gen_scatter.Layout(gen_scatter.MacroEval(gen_scatter.parse_defines(header_path)))
    layout.header = header_path
    return layout


def version_from_project(project):
    """从 App 工程自带的版本文件读版本号。

    约定：``<app_root>/app_version.txt``（app_root 是 MDK-ARM 的上一级）。
    文件里第一个非空、非 ``#`` 开头的行就是版本号。这样「这个 App 是哪个
    版本」只有一处定义，改版本不需要改命令行，也不会出现"忘了带 --version
    于是打出一个 0.0.0.0 的包"。
    """
    if not project:
        return ""
    uvprojx = os.path.abspath(project)
    app_root = os.path.dirname(os.path.dirname(uvprojx))
    path = os.path.join(app_root, VERSION_FILE)
    if not os.path.isfile(path):
        return ""
    with io.open(path, "r", encoding="utf-8-sig") as f:
        for line in f:
            line = line.strip()
            if line and not line.startswith("#"):
                return line
    return ""


def resolve_version(args):
    """版本号的唯一入口：--version > 工程内 app_version.txt > 报错。

    不再回落到 0：安装路径按 image_id 做版本单调把关，一个静默的 v0 会让
    「装第二遍」和「降级回灌」都变成合法安装。
    """
    if args.version:
        return args.version
    v = version_from_project(args.project)
    if v:
        print("[pack] 版本取自工程: %s -> %s" % (VERSION_FILE, v))
        return v
    raise PackError(
        "没有版本号：给 --version，或在工程根目录放一份 %s（内容为版本号，"
        "如 1.0.0）。镜像头里的版本号是安装路径版本单调把关的判据，"
        "不能留空。" % VERSION_FILE)


def parse_version(text):
    """'1.2.3' -> 0x00010203；也接受 '0x010203' 形式"""
    text = (text or "").strip()
    if not text:
        return 0
    if text.lower().startswith("0x"):
        return int(text, 16)
    parts = [int(p) for p in text.split(".")]
    while len(parts) < 4:
        parts.append(0)
    if len(parts) > 4 or any(p < 0 or p > 0xFF for p in parts):
        raise PackError("版本号非法: %s（应为 1.2.3 或 0x010203）" % text)
    return (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3]


def version_str(v):
    return "%d.%d.%d.%d" % ((v >> 24) & 0xFF, (v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF)


def pow2_ceil(n):
    """向上取整到 2 的幂（n<=0 时返回 1）"""
    p = 1
    while p < n:
        p <<= 1
    return p


def auto_int(text):
    return int(text, 0)


# ---------------------------------------------------------------- 差分重定位
def diff_reloc(img_a, img_b, img_c, delta, payload_prefix):
    """比对三份镜像，生成重定位表

    @param img_a/b/c 三份负载二进制（长度必须相同）
    @param delta     B/C 相对 A 的链接基址偏移量
    @param payload_prefix 负载在镜像内的起始偏移（= 头长 + 表长）
    @return [(entry, kind, offset)] 列表，按偏移升序
    """
    if not (len(img_a) == len(img_b) == len(img_c)):
        raise PackError("三次链接的镜像长度不一致（%d / %d / %d）：说明链接结果依赖基址，"
                        "重定位表不可靠" % (len(img_a), len(img_b), len(img_c)))
    if len(img_a) % 4 != 0:
        raise PackError("镜像长度 %d 不是 4 的倍数" % len(img_a))

    out = []
    n = len(img_a)
    pos = 0
    while pos < n:
        # ---- 1) 先按 4 字节纯数据字判（literal pool 里的指针、常量表）----
        if pos + 4 <= n:
            wa, wb, wc = _u32(img_a, pos), _u32(img_b, pos), _u32(img_c, pos)
            db = (wb - wa) & 0xFFFFFFFF
            dc = (wc - wa) & 0xFFFFFFFF
            if db or dc:
                if db == delta and dc == 0:
                    off = payload_prefix + pos
                    out.append((reloc_entry(off, RELOC_KIND_ROM), RELOC_KIND_ROM, off))
                    pos += 4
                    continue
                if dc == delta and db == 0:
                    off = payload_prefix + pos
                    out.append((reloc_entry(off, RELOC_KIND_RAM), RELOC_KIND_RAM, off))
                    pos += 4
                    continue

        # ---- 2) 按 Thumb 指令长度走到下一条 ----
        ha, hb, hc = _u16(img_a, pos), _u16(img_b, pos), _u16(img_c, pos)
        if ha == hb == hc:
            pos += 4 if _thumb_is32(ha) else 2
            continue

        # ---- 3) 指令本身不同：只能是 MOVW/MOVT 立即数对被链接期改写 ----
        va, vb, vc = (_read_mov_pair(img_a, pos), _read_mov_pair(img_b, pos),
                      _read_mov_pair(img_c, pos))
        if va is None or vb is None or vc is None:
            raise PackError(
                "负载偏移 0x%X 处的指令在三份链接里对不上：A=0x%04X B=0x%04X C=0x%04X，"
                "既不是整字可平移的地址，也不是相邻的 MOVW/MOVT 立即数对。\n"
                "        该位置无法被搬移到动态落点。" % (pos, ha, hb, hc))
        db = (vb - va) & 0xFFFFFFFF
        dc = (vc - va) & 0xFFFFFFFF
        if db == delta and dc == 0:
            kind = RELOC_KIND_ROM_MOVW
        elif dc == delta and db == 0:
            kind = RELOC_KIND_RAM_MOVW
        else:
            raise PackError(
                "负载偏移 0x%X 处的 MOVW/MOVT 立即数对在三份链接里不是简单的基址平移："
                "A=0x%08X B=0x%08X C=0x%08X（delta=0x%X）。"
                % (pos, va, vb, vc, delta))
        off = payload_prefix + pos
        out.append((reloc_entry(off, kind), kind, off))
        pos += 8

    return out


def verify_reloc(img_a, img_d, entries, payload_prefix, delta):
    """把重定位表应用到 A 上，必须逐字节等于 D（ROM 类 +2*delta，RAM 类 +delta）"""
    if len(img_a) != len(img_d):
        raise PackError("验证镜像长度与基准镜像不一致（%d vs %d）" % (len(img_a), len(img_d)))

    rom_kinds = (RELOC_KIND_ROM, RELOC_KIND_ROM_MOVW)
    patched = bytearray(img_a)
    seen = set()
    for entry, kind, off in entries:
        i = off - payload_prefix
        if i in seen:
            raise PackError("重定位表出现重复偏移 0x%X" % off)
        seen.add(i)
        apply_reloc_entry(patched, i, kind, 2 * delta if kind in rom_kinds else delta)

    if bytes(patched) != img_d:
        step = 2
        for i in range(0, len(img_d), step):
            if patched[i:i + step] != img_d[i:i + step]:
                raise PackError(
                    "重定位表验证失败：负载偏移 0x%X 处打了补丁后是 %s，"
                    "而验证链接给出的是 %s。表不完整或有误判。"
                    % (i, bytes(patched[i:i + step]).hex(), img_d[i:i + step].hex()))
        raise PackError("重定位表验证失败（长度一致但内容不同）")


# ---------------------------------------------------------------- 镜像头
def build_header(img_type, hw_compat, version, image_size, entry_offset, nominal_base,
                 crc32, flags, image_id, ram_size, reloc_count, nominal_ram_base,
                 manifest=None, state=0):
    """按 svcrt_app_header_t 组装 256 字节头（字段偏移见 kernelsrc/include/svcrt_app_image.h）"""
    payload_offset = HEADER_SIZE + reloc_count * RELOC_SIZE

    reserved = bytearray(RESERVED_SIZE)
    if manifest:
        raw = json.dumps(manifest, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        if len(raw) > MANIFEST_MAX:
            raise PackError("清单过长（%d 字节，上限 %d）" % (len(raw), MANIFEST_MAX))
        reserved[0] = len(raw)
        reserved[1:1 + len(raw)] = raw

    head = struct.pack("<IIIIIIII", MAGIC, img_type, hw_compat, version,
                       image_size, entry_offset, nominal_base, crc32)
    head += b"\x00" * 64                                    # signature[64]
    head += struct.pack("<IIII", flags, state, image_id, ram_size)
    head += struct.pack("<IIII", HEADER_SIZE, reloc_count, RELOC_TABLE_KIND,
                       payload_offset)
    head += struct.pack("<I", nominal_ram_base)
    head += bytes(reserved)
    # runtime_ram_base：打包产物里恒为 0（它由内核在安装时写）。占位必须写出来，
    # 否则头长度对不上，而且 CRC 是按「这四个字节参与」算的。
    head += b"\x00" * 4
    if len(head) != HEADER_SIZE:
        raise PackError("镜像头长度算错：%d != %d" % (len(head), HEADER_SIZE))
    return head


def calc_crc(header, reloc_table, payload):
    """与内核 svcrt_loader_image_crc 等价：头（crc32/state 按 0）+ 重定位表 + 负载"""
    zeroed = header[:CRC32_OFFSET] + b"\x00" * 4 + \
        header[CRC32_OFFSET + 4:STATE_CRC_OFFSET] + b"\x00" * 4 + \
        header[STATE_CRC_OFFSET + 4:]
    crc = zlib.crc32(zeroed)
    crc = zlib.crc32(reloc_table, crc)
    crc = zlib.crc32(payload, crc)
    return crc & 0xFFFFFFFF


def assemble(img_type, hw_compat, version, payload, entry_offset, nominal_base,
             nominal_ram_base, ram_size, entries, flags, manifest=None):
    """组装最终镜像（头 + 重定位表 + 负载）"""
    reloc_count = len(entries)
    if reloc_count > 1024:
        raise PackError("重定位表条目数 %d 超过内核上限 1024：镜像里的绝对地址太多，"
                        "需要与内核协商提高上限" % reloc_count)
    reloc_table = b"".join(struct.pack("<I", e) for e, _k, _o in entries)

    payload_offset = HEADER_SIZE + len(reloc_table)
    for e, _k, off in entries:
        if off < payload_offset:
            raise PackError("重定位表项 0x%X 落在表自身范围内（表尾 0x%X）" % (off, payload_offset))

    image_id = zlib.crc32(payload) & 0xFFFFFFFF
    header = build_header(img_type, hw_compat, version, len(payload), entry_offset,
                          nominal_base, 0, flags, image_id, ram_size, reloc_count,
                          nominal_ram_base, manifest=manifest)
    crc = calc_crc(header, reloc_table, payload)
    header = header[:CRC32_OFFSET] + struct.pack("<I", crc) + header[CRC32_OFFSET + 4:]
    return header + reloc_table + payload, crc, image_id


# ---------------------------------------------------------------- 工程编译
def find_fromelf():
    for p in FROMELF_CANDIDATES:
        if os.path.isfile(p):
            return p
    raise PackError("找不到 fromelf.exe（试过: %s）" % ", ".join(FROMELF_CANDIDATES))


def read_project_targets(uvprojx):
    """返回 [(target 名, OutputDirectory, OutputName)]，按文件顺序"""
    import xml.etree.ElementTree as ET
    tree = ET.parse(uvprojx)
    root = tree.getroot()
    out = []
    for t in root.iter("Target"):
        name = t.findtext("TargetName") or ""
        opt = t.find("TargetOption/TargetCommonOption")
        outdir = (opt.findtext("OutputDirectory") if opt is not None else "") or ""
        outname = (opt.findtext("OutputName") if opt is not None else "") or ""
        out.append((name, outdir, outname))
    if not out:
        raise PackError("%s 里没找到任何 Target" % uvprojx)
    return out


def run_uv4(uv4, uvprojx, target, log_path):
    """全量重建。UV4 退出码：0 = 无错无警告，1 = 有警告，>=2 = 有错"""
    cmd = [uv4, "-r", uvprojx, "-t", target, "-j0", "-o", log_path]
    print("[pack] UV4 -r %s" % target)
    # UV4 会把环境原样传给 BeforeMake 里钩子，而本工具自身可能跑在一个设置了
    # PYTHONHOME/PYTHONPATH 的嵌入式 Python 里（那会让钩子里的 py -3 起不来）。
    # 因此给子进程一份干净的环境，钩子用哪个解释器由 py -3 自己决定。
    env = dict(os.environ)
    for k in ("PYTHONHOME", "PYTHONPATH", "PYTHONSTARTUP"):
        env.pop(k, None)
    try:
        rc = subprocess.call(cmd, env=env)
    except OSError as exc:
        raise PackError("无法启动 UV4（%s）：%s" % (uv4, exc))

    log = ""
    if os.path.isfile(log_path):
        with open(log_path, "r", encoding="utf-8", errors="replace") as f:
            log = f.read()
    if rc >= 2:
        raise PackError("编译失败（UV4 退出码 %d）。日志尾部：\n%s" % (rc, tail(log)))
    return log


def tail(text, lines=25):
    parts = [ln for ln in text.strip().splitlines() if ln.strip()]
    return "\n".join(parts[-lines:])


def _clean_env():
    env = dict(os.environ)
    for k in ("PYTHONHOME", "PYTHONPATH", "PYTHONSTARTUP"):
        env.pop(k, None)
    return env


def run_fromelf(fromelf, axf, bin_path):
    cmd = [fromelf, "--bin", "--output=" + bin_path, axf]
    rc = subprocess.call(cmd, env=_clean_env())
    if rc != 0 or not os.path.isfile(bin_path):
        raise PackError("fromelf 生成 %s 失败（退出码 %d）" % (bin_path, rc))
    with open(bin_path, "rb") as f:
        return f.read()


def map_rw_size(map_path):
    """从 Keil 的 .map 里取 RW/ZI 执行区的实际占用字节数（不含栈区）

    Keil 的 "Total RW Size" 把 ARM_LIB_STACK 这种 EMPTY 栈区也算了进去，
    直接用它会把栈重复计一遍，所以优先取 RW_IMG 执行区的 Size。
    取不到时返回 None。
    """
    if not os.path.isfile(map_path):
        return None
    total = None
    with open(map_path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            s = line.strip()
            if s.startswith("Execution Region RW_") and "Size:" in s:
                for part in s.split(","):
                    part = part.strip()
                    if part.startswith("Size:"):
                        try:
                            return int(part.split(":", 1)[1].strip(), 16)
                        except ValueError:
                            break
            if s.startswith("Total RW") and "Size" in s:
                # "Total RW  Size (RW Data + ZI Data)   1234 (  ...)"
                body = s.split(")", 1)[-1].strip()
                tok = body.split()
                if tok:
                    try:
                        return int(tok[0])
                    except ValueError:
                        pass
            if s.startswith("Total ROM") and "Size" in s:
                body = s.split(")", 1)[-1].strip()
                tok = body.split()
                if tok:
                    try:
                        total = int(tok[0])
                    except ValueError:
                        pass
    return None


# ---------------------------------------------------------------- 打包主流程
def probe_elf(axf, entry_sym):
    """读一份 .axf：负载链接基址与入口符号地址（都随该遍的 .sct 变化）"""
    elf = Elf(axf)
    syms = elf.symbols()
    if entry_sym not in syms:
        raise PackError("在 %s 中找不到入口符号 %s（可用: %s）"
                        % (axf, entry_sym, ", ".join(sorted(syms)[:10])))
    return {"base": elf.flash_base(), "entry": syms[entry_sym]}

def build_passes(args, layout, proj_ctx=None):
    """取得四份负载镜像（A/B/C/D）与该遍 .axf 的链接信息

    args.bin_a/b/c/d 走离线路径；否则走工程编译路径。

    注意：四遍编译共用同一个 .axf 输出路径，所以每遍的链接基址与入口地址
    必须在**该遍编译刚结束时**读出来，否则会读到最后一篇的产物。
    返回 (bins, elfinfo, delta)；elfinfo[tag] = {"base", "entry", "rw"?}
    """
    v = layout.v
    delta = v("SVCRT_RELOC_DELTA")
    entry_sym = args.entry_symbol or ("APPSTART" if args.type == "app" else "DRVSTART")

    if args.bin_a:
        for name in ("bin_b", "bin_c"):
            if not getattr(args, name):
                raise PackError("离线模式必须同时给出 --bin-a/--bin-b/--bin-c"
                                "（建议再加 --bin-d 做验证）")
        # 键名必须与主流程的 bins["A"]/["B"]/["C"]/["D"] 一致（tag 即遍次）
        bins = {}
        for tag, path in (("A", args.bin_a), ("B", args.bin_b),
                          ("C", args.bin_c), ("D", args.bin_d)):
            if path:
                with open(path, "rb") as f:
                    bins[tag] = f.read()
                print("[pack] 读入镜像 %s: %s（%d 字节）" % (tag, path, len(bins[tag])))
        info = {}
        if args.axf:
            info["A"] = probe_elf(args.axf, entry_sym)
        return bins, info, delta

    # ---- 工程编译路径 ----
    uvprojx = os.path.abspath(args.project)
    if not os.path.isfile(uvprojx):
        raise PackError("找不到工程文件: %s" % uvprojx)
    uv4 = args.uv4
    if not os.path.isfile(uv4):
        raise PackError("找不到 UV4.exe: %s（可用 --uv4 指定）" % uv4)
    fromelf = find_fromelf()

    targets = read_project_targets(uvprojx)
    tname = args.target_name or targets[0][0]
    match = [t for t in targets if t[0] == tname]
    if not match:
        raise PackError("工程 %s 里没有 Target \"%s\"（现有: %s）"
                        % (uvprojx, tname, ", ".join(t[0] for t in targets)))
    _tn, outdir_rel, outname = match[0]
    projdir = os.path.dirname(uvprojx)
    outdir = os.path.join(projdir, outdir_rel) if outdir_rel else projdir
    axf = os.path.join(outdir, outname + ".axf")
    mapfile = os.path.join(outdir, outname + ".map")

    workdir = os.path.join(REPO_ROOT, "build", "pack", args.name or outname)
    if not os.path.isdir(workdir):
        os.makedirs(workdir)

    passes = [("A", 0, 0),                        # 标称基址
              ("B", delta, 0),                    # ROM +delta
              ("C", 0, delta)]                    # RAM +delta
    if not args.no_verify:
        passes.append(("D", 2 * delta, delta))     # 验证：ROM +2delta，RAM +delta

    bins = {}
    elfinfo = {}
    env_prev = {k: os.environ.get(k) for k in
                ("SVCRT_SCT_MODE", "SVCRT_ROM_DELTA", "SVCRT_RAM_DELTA")}
    try:
        for tag, rom_d, ram_d in passes:
            os.environ["SVCRT_SCT_MODE"] = "nominal"
            os.environ["SVCRT_ROM_DELTA"] = hex(rom_d)
            os.environ["SVCRT_RAM_DELTA"] = hex(ram_d)
            log_path = os.path.join(workdir, "uv4_%s.log" % tag)
            run_uv4(uv4, uvprojx, tname, log_path)
            if not os.path.isfile(axf):
                raise PackError("编译后找不到 %s" % axf)
            bin_path = os.path.join(workdir, "payload_%s.bin" % tag)
            bins[tag] = run_fromelf(fromelf, axf, bin_path)
            elfinfo[tag] = probe_elf(axf, entry_sym)
            if tag == "A":
                elfinfo[tag]["rw"] = map_rw_size(mapfile)
            print("[pack] 第 %s 遍：ROM%+d RAM%+d -> %d 字节，链接基址 0x%08X"
                  % (tag, rom_d, ram_d, len(bins[tag]), elfinfo[tag]["base"]))
    finally:
        for k, val in env_prev.items():
            if val is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = val
        if args.restore_dev_scatter:
            restore_dev_scatter(proj_ctx)

    return bins, elfinfo, delta


def restore_dev_scatter(proj_ctx):
    """把工程的 .sct 还原成开发调试用的固定落点版本"""
    if not proj_ctx:
        return
    cmd = [sys.executable, os.path.join(REPO_ROOT, "tools", "gen_app_sct.py")] + proj_ctx + \
        ["--mode", "dev"]
    try:
        subprocess.call(cmd, env=_clean_env())
    except OSError as exc:                                # noqa: BLE001
        print("[pack] 警告：还原开发用 .sct 失败：%s" % exc)


def do_pack(args):
    layout = load_layout(args.header)
    v = layout.v
    img_type = TYPE_APP if args.type == "app" else TYPE_DRIVER
    stack_size = v("APP_TASK_STACK_SIZE") if img_type == TYPE_APP else v("DRIVER_TASK_STACK_SIZE")

    proj_ctx = None
    if args.project:
        proj_ctx = ["--project", os.path.abspath(args.project),
                    "--type", args.type,
                    "--dev-slot", str(args.dev_slot)]
        if args.ram_size is not None:
            proj_ctx += ["--ram-size", str(args.ram_size)]
        if args.target_name:
            proj_ctx += ["--target-name", args.target_name]

    bins, elfinfo, delta = build_passes(args, layout, proj_ctx)

    img_a, img_b, img_c = bins["A"], bins["B"], bins["C"]

    # ---- 标称基址与负载基址 ----
    nominal_rom_base = v("SVCRT_APP_NOMINAL_ROM_BASE")
    nominal_ram_base = v("SVCRT_APP_NOMINAL_RAM_BASE")
    payload_link_base = v("SVCRT_APP_NOMINAL_ROM_LOAD")   # 负载的链接基址
    if args.payload_base is not None and args.payload_base != payload_link_base:
        raise PackError("--payload-base 0x%08X 与配置头的标称负载基址 0x%08X 不一致"
                        % (args.payload_base, payload_link_base))

    if elfinfo:
        # 四遍的链接基址都必须与 gen_scatter 的标称布局对得上：A 是标称，
        # B 只动 ROM（+delta），C 只动 RAM，D 两个都动。任一不符都说明
        # 那一遍实际用的 .sct 不是 nominal 的，必须先修好再打包。
        expect_base = {"A": payload_link_base,
                       "B": payload_link_base + delta,
                       "C": payload_link_base,
                       "D": payload_link_base + 2 * delta}
        for tag in sorted(elfinfo):
            if tag in expect_base and elfinfo[tag]["base"] != expect_base[tag]:
                raise PackError("第 %s 遍的链接基址 0x%08X 与期望 0x%08X 不一致："
                                "该遍的 .sct 必须由 gen_scatter.py --nominal 生成"
                                % (tag, elfinfo[tag]["base"], expect_base[tag]))
        if "A" in elfinfo:
            entry_offset = elfinfo["A"]["entry"] - payload_link_base
        else:
            raise PackError("离线模式没给 --axf，必须用 --entry-offset 指定入口偏移")
    else:
        if args.entry_offset is None:
            raise PackError("离线模式没给 --axf，必须用 --entry-offset 指定入口偏移")
        entry_offset = args.entry_offset

    if entry_offset < 0 or entry_offset >= len(img_a):
        raise PackError("入口偏移 0x%X 超出负载范围（0x0 ~ 0x%X）" % (entry_offset, len(img_a) - 1))

    # ---- 差分求重定位表 ----
    # 表长取决于条目数，而条目偏移要含表长，所以先按「表长未知」跑一遍得到条目数，
    # 再用真实表长重算偏移（两次结果一致才说明自洽）。
    entries0 = diff_reloc(img_a, img_b, img_c, delta, 0)
    reloc_count = len(entries0)
    payload_prefix = HEADER_SIZE + reloc_count * RELOC_SIZE
    entries = diff_reloc(img_a, img_b, img_c, delta, payload_prefix)
    if len(entries) != reloc_count:
        raise PackError("重定位表长度自相矛盾（%d vs %d）" % (len(entries), reloc_count))
    n_rom = sum(1 for _e, k, _o in entries if k in (RELOC_KIND_ROM, RELOC_KIND_ROM_MOVW))
    n_ram = reloc_count - n_rom
    n_mov = sum(1 for _e, k, _o in entries
                if k in (RELOC_KIND_ROM_MOVW, RELOC_KIND_RAM_MOVW))
    print("[pack] 重定位表：%d 项（ROM %d / RAM %d，其中指令内立即数 %d 项），负载偏移 0x%X"
          % (reloc_count, n_rom, n_ram, n_mov, payload_prefix))

    if "D" in bins:
        verify_reloc(img_a, bins["D"], entries, payload_prefix, delta)
        print("[pack] 重定位表验证通过：应用表后与第四遍链接逐字节一致")
    else:
        print("[pack] 警告：未做第四遍验证（--no-verify）")

    # ---- RAM 需求核对 ----
    ram_size = args.ram_size
    if "app" == args.type:
        ram_size = ram_size or pow2_ceil(2 * stack_size)
    else:
        ram_size = ram_size or pow2_ceil(4 * stack_size)
    if ram_size < v("SLOT_RAM_MIN_BLOCK") or ram_size > v("SLOT_RAM_MAX_BLOCK") or \
            (ram_size & (ram_size - 1)) != 0:
        raise PackError("ram_size=%d 必须是 [%d, %d] 内的 2 的幂"
                        % (ram_size, v("SLOT_RAM_MIN_BLOCK"), v("SLOT_RAM_MAX_BLOCK")))
    if stack_size >= ram_size:
        raise PackError("ram_size=%d 装不下 %d 字节栈 + RW/ZI" % (ram_size, stack_size))

    rw_size = args.rw_size
    if rw_size is None and elfinfo.get("A", {}).get("rw"):
        rw_size = elfinfo["A"]["rw"]
    if rw_size is not None:
        need = rw_size + stack_size
        if need > ram_size:
            raise PackError("镜像需要 %d 字节 RAM（RW/ZI %d + 栈 %d），超过声明的 ram_size=%d"
                            % (need, rw_size, stack_size, ram_size))
        print("[pack] RAM 需求：RW/ZI %d + 栈 %d = %d 字节（声明 %d）"
              % (rw_size, stack_size, need, ram_size))

    # ---- 组装 ----
    version = parse_version(resolve_version(args))
    manifest = {"name": args.name, "ver": version_str(version),
                "type": TYPE_NAME[img_type], "entry": args.entry_symbol}
    manifest = {k: x for k, x in manifest.items() if x}
    flags = FLAG_AUTOSTART if args.autostart else 0
    image, crc, image_id = assemble(img_type, args.hw_compat or v("SVCRT_HW_COMPAT_ID"),
                                    version, img_a, entry_offset,
                                    payload_link_base, nominal_ram_base, ram_size,
                                    entries, flags, manifest)

    # ---- 落点跨度是否装得进池 ----
    total = len(image)
    pool_usable = v("IMAGE_POOL_USABLE_SIZE")
    if total > pool_usable:
        raise PackError("镜像总长 %d 字节超过池可用空间 %d 字节" % (total, pool_usable))
    span_units = pow2_ceil(-(-total // v("SVCRT_POOL_ALLOC_UNIT")))    # 向上取整到 2 的幂
    span_bytes = span_units * v("SVCRT_POOL_ALLOC_UNIT")
    if span_bytes > pool_usable:
        raise PackError("镜像按 2 的幂对齐后需要 %d 字节（%d 个分配单元），超过池可用空间 %d"
                        % (span_bytes, span_units, pool_usable))

    out = args.out
    if not out:
        out = os.path.join(REPO_ROOT, "build", (args.name or "image") + ".svcapp")
    outdir = os.path.dirname(os.path.abspath(out))
    if outdir and not os.path.isdir(outdir):
        os.makedirs(outdir)
    with open(out, "wb") as f:
        f.write(image)

    # ---- 落盘自检：按镜像文件里的表（而非内存里的表）重放一遍，必须等于第四遍链接 ----
    # 内存表正确不代表落盘后仍正确：表项是压成 4 字节写的，编码有损就会在
    # 这里暴露出来（曾经就是这么漏掉「偏移少 2 字节」的）。
    if "D" in bins:
        with open(out, "rb") as f:
            written = f.read()
        replay = bytearray(written[payload_prefix:payload_prefix + len(img_a)])
        for i in range(reloc_count):
            e = struct.unpack_from("<I", written, HEADER_SIZE + i * RELOC_SIZE)[0]
            kind = reloc_kind(e)
            delta_i = 2 * delta if kind in (RELOC_KIND_ROM, RELOC_KIND_ROM_MOVW) else delta
            apply_reloc_entry(replay, reloc_off(e) - payload_prefix, kind, delta_i)
        if bytes(replay) != bins["D"]:
            raise PackError("落盘自检失败：按镜像文件里的重定位表重放后与第四遍链接不一致")
        print("[pack] 落盘自检通过：镜像内的表重放后与第四遍链接逐字节一致")

    print("")
    print("[pack] 类型        : %s" % TYPE_NAME[img_type])
    print("[pack] 输出        : %s" % out)
    print("[pack] 标称 ROM 基址: 0x%08X（负载链接基址）" % payload_link_base)
    print("[pack] 标称 RAM 基址: 0x%08X" % nominal_ram_base)
    print("[pack] 负载长度    : %d 字节" % len(img_a))
    print("[pack] 入口偏移    : 0x%08X" % entry_offset)
    print("[pack] 重定位表    : %d 项（ROM %d / RAM %d）" % (reloc_count, n_rom, n_ram))
    print("[pack] RAM 声明    : %d 字节" % ram_size)
    print("[pack] 版本        : %s" % version_str(version))
    print("[pack] 硬件兼容 ID : 0x%08X" % (args.hw_compat or v("SVCRT_HW_COMPAT_ID")))
    print("[pack] flags       : 0x%08X（%s）" % (flags, "自启" if flags & FLAG_AUTOSTART else "不自启"))
    print("[pack] CRC32       : 0x%08X，image_id 0x%08X" % (crc, image_id))
    print("[pack] 文件总大小  : %d 字节（落点跨度需要 %d 字节）" % (total, span_bytes))
    return 0


# ---------------------------------------------------------------- 查看 / 校验
def read_header(path):
    with open(path, "rb") as f:
        raw = f.read()
    if len(raw) < HEADER_SIZE:
        raise PackError("文件过短，不是有效镜像: %s" % path)
    (magic, img_type, hw_compat, version, image_size, entry_offset,
     nominal_base, crc32) = struct.unpack_from("<IIIIIIII", raw, 0)
    if magic != MAGIC:
        raise PackError("镜像魔数错误: 0x%08X（期望 0x%08X \"SVCA\"）" % (magic, MAGIC))
    flags = struct.unpack_from("<I", raw, FLAGS_OFFSET)[0]
    state = struct.unpack_from("<I", raw, STATE_OFFSET)[0]
    reloc_kind_v = struct.unpack_from("<I", raw, RELOC_KIND_OFF)[0]
    image_id = struct.unpack_from("<I", raw, IMAGE_ID_OFFSET)[0]
    ram_size = struct.unpack_from("<I", raw, RAM_SIZE_OFFSET)[0]
    reloc_count = struct.unpack_from("<I", raw, RELOC_COUNT_OFF)[0]
    payload_offset = struct.unpack_from("<I", raw, PAYLOAD_OFFSET_OFF)[0]
    nominal_ram_base = struct.unpack_from("<I", raw, NOMINAL_RAM_OFFSET)[0]
    runtime_ram_base = struct.unpack_from("<I", raw, RUNTIME_RAM_OFFSET)[0]
    reserved = raw[RESERVED_OFFSET:RESERVED_OFFSET + RESERVED_SIZE]
    manifest = None
    if reserved[0]:
        try:
            manifest = json.loads(reserved[1:1 + reserved[0]].decode("utf-8"))
        except Exception:                                     # noqa: BLE001
            manifest = None
    return dict(raw=raw, type=img_type, hw_compat=hw_compat, version=version,
                reloc_kind=reloc_kind_v,
                image_size=image_size, entry_offset=entry_offset, nominal_base=nominal_base,
                crc32=crc32, flags=flags, state=state, image_id=image_id, ram_size=ram_size,
                reloc_count=reloc_count, payload_offset=payload_offset,
                nominal_ram_base=nominal_ram_base, runtime_ram_base=runtime_ram_base,
                manifest=manifest)


def do_info(path):
    h = read_header(path)
    print("镜像文件   : %s (%d 字节)" % (path, len(h["raw"])))
    print("类型       : %s (%d)" % (TYPE_NAME.get(h["type"], "未知"), h["type"]))
    print("版本       : %s (0x%08X)" % (version_str(h["version"]), h["version"]))
    print("硬件兼容   : 0x%08X" % h["hw_compat"])
    print("标称 ROM 基址: 0x%08X（负载链接基址）" % h["nominal_base"])
    print("标称 RAM 基址: 0x%08X" % h["nominal_ram_base"])
    print("负载偏移   : 0x%X（头 256 + 表 %d 项）" % (h["payload_offset"], h["reloc_count"]))
    print("负载长度   : %d 字节" % h["image_size"])
    print("入口偏移   : 0x%08X（相对负载起始）" % h["entry_offset"])
    print("RAM 需求   : %d 字节" % h["ram_size"])
    print("CRC32      : 0x%08X   image_id: 0x%08X" % (h["crc32"], h["image_id"]))
    print("state      : %d（%s）" % (h["state"], "已提交" if h["state"] == 0 else "未提交"))
    print("flags      : 0x%08X（%s）"
          % (h["flags"], "开机自启" if h["flags"] & FLAG_AUTOSTART else "不自启（需显式启动）"))
    if h["flags"] & ~FLAG_KNOWN_MASK:
        print("             注意：含本工具未知的标志位 0x%08X" % (h["flags"] & ~FLAG_KNOWN_MASK))
    print("清单       : %s" % (json.dumps(h["manifest"], ensure_ascii=False)
                              if h["manifest"] else "(无)"))
    return 0


def do_verify(path, header_path, delta_from_cfg=None):
    h = read_header(path)
    raw = h["raw"]
    total = h["payload_offset"] + h["image_size"]
    problems = []

    if len(raw) < total:
        problems.append("文件长度 %d 小于头+表+负载 %d" % (len(raw), total))
    elif len(raw) > total:
        print("[verify] 提示：文件尾部有 %d 字节冗余，镜像仍有效" % (len(raw) - total))

    if raw[CRC32_OFFSET:CRC32_OFFSET + 4] == b"\x00" * 4 or \
            raw[STATE_OFFSET:STATE_OFFSET + 4] == b"\x00" * 4:
        pass
    crc = calc_crc(raw[:HEADER_SIZE], raw[HEADER_SIZE:h["payload_offset"]],
                   raw[h["payload_offset"]:total])
    if crc != h["crc32"]:
        problems.append("CRC32 不匹配：文件 0x%08X，重算 0x%08X" % (h["crc32"], crc))
    else:
        print("[verify] CRC32 校验通过：0x%08X" % crc)

    if (zlib.crc32(raw[h["payload_offset"]:total]) & 0xFFFFFFFF) != h["image_id"]:
        problems.append("image_id 与负载 CRC32 不一致")
    else:
        print("[verify] image_id 校验通过（= 负载 CRC32）")

    if h["state"] != 0:
        problems.append("state = %d：镜像是「未提交」状态（打包产物不应如此）" % h["state"])

    # runtime_ram_base 是内核在安装那一刻填的（写到哪个 RAM 块就跑在哪里），
    # 打包产物必须是 0。「非 0 却还在当打包产物分发」只有一种来源：把板上
    # 拆下来的镜像又当输入喂了进来 —— 那样的镜像里所有地址都是别的设备的
    # 布局，装上去只会一启动就 MemManage，所以在这里直接报错。
    if h["runtime_ram_base"] != 0:
        problems.append("runtime_ram_base = 0x%08X：这是已安装过的镜像，不是打包产物"
                        % h["runtime_ram_base"])
    else:
        print("[verify] runtime_ram_base = 0（未固化，符合打包产物）")

    if h["payload_offset"] != HEADER_SIZE + h["reloc_count"] * RELOC_SIZE:
        problems.append("payload_offset 与 reloc_count 不自洽")
    if h["reloc_count"] > 1024:
        problems.append("reloc_count %d 超过内核上限 1024" % h["reloc_count"])

    if h["reloc_kind"] != RELOC_TABLE_KIND:
        problems.append("reloc_kind = %d：不是当前表编码版本 %d"
                        % (h["reloc_kind"], RELOC_TABLE_KIND))

    # 重定位表项：落在负载内、升序；数据类 4 字节对齐，指令类半字对齐
    prev = 0
    for i in range(h["reloc_count"]):
        e = struct.unpack_from("<I", raw, HEADER_SIZE + i * 4)[0]
        off = reloc_off(e)
        kind = reloc_kind(e)
        if kind in (RELOC_KIND_ROM_MOVW, RELOC_KIND_RAM_MOVW):
            need = RELOC_MOV_SIZE
            if off % 2:
                problems.append("第 %d 项偏移 0x%X 未半字对齐" % (i, off))
        else:
            need = RELOC_SIZE
            if off % RELOC_SIZE:
                problems.append("第 %d 项偏移 0x%X 未 4 字节对齐" % (i, off))
        if off < h["payload_offset"] or off > total - need:
            problems.append("第 %d 项偏移 0x%X 落在负载之外（负载 0x%X ~ 0x%X）"
                            % (i, off, h["payload_offset"], total - need))
        if i and off < prev:
            problems.append("第 %d 项偏移 0x%X 未按升序排列（前一项 0x%X）" % (i, off, prev))
        prev = off

    if header_path:
        layout = load_layout(header_path)
        v = layout.v
        if h["hw_compat"] != v("SVCRT_HW_COMPAT_ID"):
            problems.append("硬件兼容 ID 与 config/svcrt_partition.h 不一致")
        if h["nominal_base"] != v("SVCRT_APP_NOMINAL_ROM_LOAD"):
            problems.append("标称 ROM 基址 0x%08X 与配置 0x%08X 不一致"
                            % (h["nominal_base"], v("SVCRT_APP_NOMINAL_ROM_LOAD")))
        if h["nominal_ram_base"] != v("SVCRT_APP_NOMINAL_RAM_BASE"):
            problems.append("标称 RAM 基址 0x%08X 与配置 0x%08X 不一致"
                            % (h["nominal_ram_base"], v("SVCRT_APP_NOMINAL_RAM_BASE")))
        rs = h["ram_size"]
        if rs < v("SLOT_RAM_MIN_BLOCK") or rs > v("SLOT_RAM_MAX_BLOCK") or (rs & (rs - 1)):
            problems.append("ram_size %d 不在 [%d, %d] 内或不是 2 的幂"
                            % (rs, v("SLOT_RAM_MIN_BLOCK"), v("SLOT_RAM_MAX_BLOCK")))
        pool_usable = v("IMAGE_POOL_USABLE_SIZE")
        if total > pool_usable:
            problems.append("镜像总长 %d 超过池可用空间 %d" % (total, pool_usable))

    if problems:
        print("[verify] 失败：")
        for p in problems:
            print("  - " + p)
        return 1
    print("[verify] 通过：镜像可被 Loader 接受")
    return 0


# ---------------------------------------------------------------- CLI
def main():
    ap = argparse.ArgumentParser(
        description="SVCrtOS App/驱动镜像打包工具（动态装载路径）",
        formatter_class=argparse.RawDescriptionHelpFormatter)

    src = ap.add_argument_group("输入")
    src.add_argument("--project", help="Keil 工程（.uvprojx）：自动编译四遍并差分")
    src.add_argument("--target-name", help="工程里的 Target 名（默认取第一个）")
    src.add_argument("--bin-a", help="离线模式：标称基址链接出来的负载 bin")
    src.add_argument("--bin-b", help="离线模式：ROM 基址 +delta 的负载 bin")
    src.add_argument("--bin-c", help="离线模式：RAM 基址 +delta 的负载 bin")
    src.add_argument("--bin-d", help="离线模式：验证用（ROM +2delta、RAM +delta）")
    src.add_argument("--header", default="config/svcrt_partition.h", help="分区配置头文件")
    src.add_argument("--payload-base", type=auto_int, help="仅校验：期望的标称负载基址")

    out = ap.add_argument_group("输出与元数据")
    out.add_argument("--out", help="输出 .svcapp 路径")
    out.add_argument("--type", choices=["app", "driver"], default="app", help="镜像类型")
    out.add_argument("--name", help="镜像名（写进清单，并作为默认输出文件名）")
    out.add_argument("--version", default="",
                     help="版本号，如 1.0.0 或 0x010203；缺省时读工程根目录的 %s" % VERSION_FILE)
    out.add_argument("--hw-compat", type=auto_int, help="覆盖 hw_compat_id（默认取配置头）")
    out.add_argument("--no-autostart", dest="autostart", action="store_false",
                     help="置成「不自启」：安装后需要显式启动")
    out.add_argument("--entry-symbol", default=None,
                     help="入口符号（默认 APPSTART / DRVSTART，按类型选）")
    out.add_argument("--entry-offset", type=auto_int, help="离线模式：入口相对负载的偏移")
    out.add_argument("--axf", help="离线模式：用哪个 .axf 取入口符号与链接基址")
    out.add_argument("--rw-size", type=auto_int, help="离线模式：RW/ZI 字节数（用于核对 ram_size）")

    misc = ap.add_argument_group("其他")
    misc.add_argument("--ram-size", type=auto_int, default=None,
                      help="镜像 RAM 声明（2 的幂，块范围内）；默认按类型取 2x/4x 栈")
    misc.add_argument("--dev-slot", type=int, default=0,
                      help="开发调试裸镜像使用的槽位号（还原 .sct 时用，默认 0）")
    misc.add_argument("--uv4", default=DEFAULT_UV4, help="UV4.exe 路径")
    misc.add_argument("--no-verify", action="store_true",
                      help="跳过第四遍验证链接（不推荐：失去了打包期证明）")
    misc.add_argument("--keep-dev-scatter", dest="restore_dev_scatter",
                      action="store_false", help="打包后不还原开发用 .sct")
    misc.add_argument("--info", metavar="IMAGE", help="查看镜像头")
    misc.add_argument("--verify", metavar="IMAGE", help="校验镜像")
    args = ap.parse_args()

    if args.entry_symbol is None:
        args.entry_symbol = "APPSTART" if args.type == "app" else "DRVSTART"

    try:
        if args.info:
            return do_info(args.info)
        if args.verify:
            return do_verify(args.verify, args.header)
        if not args.project and not args.bin_a:
            ap.error("必须给出 --project 或 --bin-a（或使用 --info/--verify）")
        return do_pack(args)
    except (PackError, ElfError, KeyError, RuntimeError) as exc:  # noqa: BLE001
        print("[pack] 失败：%s" % exc, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
