#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
SVCrtOS 镜像打包工具（App / 驱动）
==================================

把 App / 驱动工程的编译产物（Keil 生成的 .axf 或 .bin）打包成 SVCrtOS
可加载镜像（.svcapp）：固定 256 字节头 + 原始镜像内容。

单一地址源头：config/svcrt_partition.h
    槽位基址、可用大小、hw_compat_id 全部由配置头推导，本脚本不硬编码地址。
    这样「改芯片容量 → 重新生成 .sct → 重新打包」三步一致，不会对不上。

镜像格式见 kernelsrc/include/svcrt_app_image.h，CRC32 算法与内核
svcrt_loader.c 使用的 zlib/IEEE 802.3 反射多项式完全一致（可分段累积）。

槽位内布局契约（与 tools/gen_scatter.py、内核 svcrt_loader.c 三方一致）：
    [槽位基址]  镜像头 256 字节
    [槽位基址 + 256]  负载（代码/只读数据）
所以 App / 驱动工程的 .sct 必须按「负载基址 = 槽位基址 + 镜像头长度」生成
（gen_scatter.py --target app|driver 的默认布局），本工具会据此校验链接基址。
镜像头里的 load_addr 记录的是「目标槽位基址」，不是负载基址。

用法：
    # 由 Keil 的 .axf 直接打包（推荐：同时从符号表解析入口偏移）
    python tools/pack_app.py --axf build/APP_DEMO/APP_DEMO.axf \
        --type app --version 1.0.0 --name "LED 闪烁示例" \
        --out build/APP_DEMO/APP_DEMO.svcapp

    # 由 fromelf 生成的 .bin 打包（需显式给出入口偏移或符号）
    python tools/pack_app.py --bin build/BLED_DRV/bled_drv.bin \
        --type driver --version 1.0.0 --entry-symbol DRVSTART \
        --out build/BLED_DRV/BLED_DRV.svcapp

    # 查看镜像头 / 校验完整性
    python tools/pack_app.py --info   build/APP_DEMO/APP_DEMO.svcapp
    python tools/pack_app.py --verify build/APP_DEMO/APP_DEMO.svcapp

入口符号说明：
    镜像入口是一个「任务型函数」：由内核 svcrt_task_register() 注册后运行，
    内部应循环调用 svcrt_task_wait() 等阻塞接口。
      推荐  APPSTART / DRVSTART：SDK 提供的启动入口，先执行 C 运行库初始化
            （.data 拷贝、.bss 清零，即 scatter loading），再调用 AppMain/DrvMain。
            镜像含初始化全局变量时必须用这个。
      可选  AppMain / DrvMain：直接进入业务函数，跳过运行时初始化，仅当镜像
            没有需要初始化的全局变量时可用。
"""

from __future__ import print_function

import argparse
import json
import os
import struct
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
DEFAULT_ENTRY = {TYPE_APP: "APPSTART", TYPE_DRIVER: "DRVSTART"}
SLOT_MACRO = {"app": "APP_SLOT0", "driver": "DRIVER_POOL"}   # 键为 --type 取值

RESERVED_SIZE = 160                  # 与 svcrt_app_image.h 的 reserved[160] 保持一致
MANIFEST_MAX = RESERVED_SIZE - 1     # 首字节存清单长度


# ---------------------------------------------------------------- ELF32 解析
class ElfError(Exception):
    pass


class Elf(object):
    """极简 ELF32 解析器（只取程序头与符号表，供打包使用，不依赖 arm 工具链）"""

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

    def _cstr(self, off, table_off, table_size):
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
                name = self._cstr(st_name, strtab["offset"], strtab["size"])
                if name and name not in out:
                    out[name] = st_value & ~1               # 去掉 Thumb 位
        return out

    def flash_image(self, flash_base, flash_end):
        """按程序头的物理地址（LMA）拼接出烧录镜像，空隙填 0xFF"""
        segs = [p for p in self.phdrs
                if p["type"] == 1 and p["filesz"] > 0                 # PT_LOAD
                and flash_base <= p["paddr"] < flash_end]             # 只要 Flash 段
        if not segs:
            raise ElfError("ELF 中没有位于 Flash 的程序段（0x%08X~0x%08X）；"
                           "请改用 fromelf 生成的 .bin 并指定 --entry-offset"
                           % (flash_base, flash_end - 1))
        segs.sort(key=lambda p: p["paddr"])
        base = segs[0]["paddr"]
        end = max(p["paddr"] + p["filesz"] for p in segs)
        buf = bytearray(b"\xff" * (end - base))
        for p in segs:
            off = p["paddr"] - base
            buf[off:off + p["filesz"]] = self.data[p["offset"]:p["offset"] + p["filesz"]]
        return base, bytes(buf)


# ---------------------------------------------------------------- 布局 / 版本
def load_layout(header_path):
    if not os.path.isabs(header_path):
        header_path = os.path.join(REPO_ROOT, header_path)
    if not os.path.isfile(header_path):
        raise SystemExit("找不到分区配置头文件: %s" % header_path)
    layout = gen_scatter.Layout(gen_scatter.MacroEval(gen_scatter.parse_defines(header_path)))
    layout.header = header_path
    return layout


def parse_version(text):
    """'1.2.3' -> 0x00010203；也接受 '0x010203' 形式"""
    text = text.strip()
    if text.lower().startswith("0x"):
        return int(text, 16)
    parts = [int(p) for p in text.split(".")] if text else []
    while len(parts) < 4:
        parts.append(0)
    if len(parts) > 4 or any(p < 0 or p > 0xFF for p in parts):
        raise SystemExit("版本号非法: %s（应为 1.2.3 或 0x010203）" % text)
    return (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3]


def version_str(v):
    return "%d.%d.%d.%d" % ((v >> 24) & 0xFF, (v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF)


# ---------------------------------------------------------------- 打包
def build_header(img_type, hw_compat, version, image_size, entry_offset,
                 load_addr, crc32, manifest):
    """按 svcrt_app_header_t 组装 256 字节头"""
    reserved = bytearray(RESERVED_SIZE)
    if manifest:
        raw = json.dumps(manifest, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        if len(raw) > MANIFEST_MAX:
            raise SystemExit("清单过长（%d 字节，上限 %d）" % (len(raw), MANIFEST_MAX))
        reserved[0] = len(raw)
        reserved[1:1 + len(raw)] = raw

    # 字段顺序必须与 svcrt_app_image.h 完全一致
    head = struct.pack("<IIIIIIII", MAGIC, img_type, hw_compat, version,
                       image_size, entry_offset, load_addr, crc32)
    head += b"\x00" * 64                # signature[64]：预留（信任链落地后启用）
    head += bytes(reserved)             # reserved[192]
    assert len(head) == HEADER_SIZE
    return head


def calc_crc(header, payload):
    """与内核 svcrt_loader_image_crc 等价：头（crc32 字段置 0）+ 负载"""
    crc = zlib.crc32(header)
    crc = zlib.crc32(payload, crc)
    return crc & 0xFFFFFFFF


def do_pack(args):
    layout = load_layout(args.header)
    v = layout.v
    img_type = TYPE_APP if args.type == "app" else TYPE_DRIVER
    slot = SLOT_MACRO[args.type]
    slot_base = v(slot + "_BASE")
    slot_size = v(slot + "_SIZE")

    # 负载的链接基址 = 槽位基址 + 镜像头长度（与 gen_scatter.py 的默认布局一致）
    header_size = v("APP_IMAGE_HEADER_SIZE")
    if header_size != HEADER_SIZE:
        raise SystemExit("配置头 APP_IMAGE_HEADER_SIZE = %d 与打包工具的 HEADER_SIZE = %d 不一致"
                         % (header_size, HEADER_SIZE))
    payload_base = slot_base + header_size
    hw_compat = args.hw_compat if args.hw_compat is not None else v("SVCRT_HW_COMPAT_ID")
    flash_base, flash_end = v("CHIP_FLASH_BASE"), v("CHIP_FLASH_BASE") + v("CHIP_FLASH_SIZE")

    # ---- 取镜像内容：优先 --bin，其次从 .axf 提取 ----
    if args.bin:
        with open(args.bin, "rb") as f:
            payload = f.read()
        base = args.load_addr if args.load_addr is not None else payload_base
        symbol_addr = None
    else:
        elf = Elf(args.axf)
        base, payload = elf.flash_image(flash_base, flash_end)
        syms = elf.symbols()
        symbol_addr = syms.get(args.entry_symbol)
        if symbol_addr is None:
            raise SystemExit("在 %s 中找不到入口符号 %s；可用符号示例: %s"
                             % (args.axf, args.entry_symbol,
                                ", ".join(sorted(syms)[:8]) or "(无)"))
        print("[pack] ELF 镜像基址 0x%08X，入口符号 %s = 0x%08X"
              % (base, args.entry_symbol, symbol_addr))

    if base != payload_base:
        raise SystemExit("镜像链接基址 0x%08X 与 %s_BASE + 镜像头 %d 字节 = 0x%08X 不一致。\n"
                         "        该工程的 .sct 必须按“负载基址 = 槽位基址 + 镜像头长度”生成：\n"
                         "        python tools/gen_scatter.py --target %s --output <out.sct>\n"
                         "        （若确实要生成开发调试用的裸镜像，请用 --raw 布局并改用 --bin 打包）"
                         % (base, slot, header_size, payload_base, args.type))

    # ---- 入口偏移 ----
    if args.entry_offset is not None:
        entry_offset = args.entry_offset
    elif symbol_addr is not None:
        entry_offset = symbol_addr - base
    else:
        hint = DEFAULT_ENTRY[img_type]
        raise SystemExit("使用 --bin 时必须给出 --entry-offset，或改用 --axf 以便自动解析"
                         "（默认入口符号 %s）" % hint)

    if entry_offset < 0 or entry_offset >= len(payload):
        raise SystemExit("入口偏移 0x%X 超出镜像范围（0x0 ~ 0x%X）" % (entry_offset, len(payload) - 1))

    # ---- 长度与对齐 ----
    if header_size + len(payload) > slot_size:
        raise SystemExit("镜像过大：%d 字节 + 头 %d 超出槽位容量 %d 字节"
                         % (len(payload), header_size, slot_size))
    pad = (-len(payload)) % 4
    if pad:
        payload += b"\xff" * pad

    manifest = None
    if args.name or args.version:
        manifest = {"name": args.name or os.path.basename(args.out or ""),
                    "ver": version_str(parse_version(args.version)) if args.version else "",
                    "entry": args.entry_symbol if not args.bin else "",
                    "type": TYPE_NAME[img_type]}
        manifest = {k: x for k, x in manifest.items() if x}

    version = parse_version(args.version) if args.version else 0
    # 镜像头里的 load_addr = 目标槽位基址（内核据此校验镜像是否装到了正确的槽位）
    header = build_header(img_type, hw_compat, version, len(payload), entry_offset,
                          slot_base, 0, manifest)
    crc = calc_crc(header, payload)
    header = header[:28] + struct.pack("<I", crc) + header[32:]

    image = header + payload

    out = args.out
    if not out:
        src = args.bin or args.axf
        out = os.path.splitext(src)[0] + ".svcapp"
    outdir = os.path.dirname(os.path.abspath(out))
    if outdir and not os.path.isdir(outdir):
        os.makedirs(outdir)
    with open(out, "wb") as f:
        f.write(image)

    print("")
    print("[pack] 类型        : %s" % TYPE_NAME[img_type])
    print("[pack] 输出        : %s" % out)
    print("[pack] 槽位基址    : 0x%08X（%s_BASE，容量 %d KB）" % (slot_base, slot, slot_size // 1024))
    print("[pack] 负载基址    : 0x%08X（= 槽位基址 + 镜像头 %d 字节）" % (payload_base, header_size))
    print("[pack] 镜像负载    : %d 字节" % len(payload))
    print("[pack] 入口偏移    : 0x%08X" % entry_offset)
    print("[pack] 版本        : %s" % version_str(version))
    print("[pack] 硬件兼容 ID : 0x%08X" % hw_compat)
    print("[pack] CRC32       : 0x%08X" % crc)
    print("[pack] 文件总大小  : %d 字节" % len(image))
    return 0


# ---------------------------------------------------------------- 查看 / 校验
def read_header(path):
    with open(path, "rb") as f:
        raw = f.read()
    if len(raw) < HEADER_SIZE:
        raise SystemExit("文件过短，不是有效镜像: %s" % path)
    (magic, img_type, hw_compat, version, image_size, entry_offset,
     load_addr, crc32) = struct.unpack_from("<IIIIIIII", raw, 0)
    if magic != MAGIC:
        raise SystemExit("镜像魔数错误: 0x%08X（期望 0x%08X \"SVCA\"）" % (magic, MAGIC))
    reserved = raw[32 + 64:HEADER_SIZE]
    manifest = None
    if reserved[0]:
        try:
            manifest = json.loads(reserved[1:1 + reserved[0]].decode("utf-8"))
        except Exception:                                     # noqa: BLE001
            manifest = None
    return dict(raw=raw, type=img_type, hw_compat=hw_compat, version=version,
                image_size=image_size, entry_offset=entry_offset,
                load_addr=load_addr, crc32=crc32, manifest=manifest)


def do_info(path):
    h = read_header(path)
    print("镜像文件 : %s (%d 字节)" % (path, len(h["raw"])))
    print("类型     : %s (%d)" % (TYPE_NAME.get(h["type"], "未知"), h["type"]))
    print("版本     : %s (0x%08X)" % (version_str(h["version"]), h["version"]))
    print("硬件兼容 : 0x%08X" % h["hw_compat"])
    print("加载地址 : 0x%08X" % h["load_addr"])
    print("入口偏移 : 0x%08X" % h["entry_offset"])
    print("负载基址 : 0x%08X" % (h["load_addr"] + HEADER_SIZE))
    print("入口地址 : 0x%08X" % (h["load_addr"] + HEADER_SIZE + h["entry_offset"]))
    print("负载长度 : %d 字节" % h["image_size"])
    print("CRC32    : 0x%08X" % h["crc32"])
    if h["manifest"]:
        print("清单     : %s" % json.dumps(h["manifest"], ensure_ascii=False))
    else:
        print("清单     : (无)")
    return 0


def do_verify(path, header_path, hw_compat_expected=None):
    h = read_header(path)
    raw = h["raw"]
    total = HEADER_SIZE + h["image_size"]
    problems = []

    if len(raw) < total:
        problems.append("文件长度 %d 小于头+负载 %d" % (len(raw), total))
    elif len(raw) > total:
        print("[verify] 提示：文件尾部有 %d 字节冗余，镜像仍有效" % (len(raw) - total))

    zeroed = raw[:28] + b"\x00\x00\x00\x00" + raw[32:total]
    if len(zeroed) == total:
        crc = zlib.crc32(zeroed[:HEADER_SIZE])
        crc = zlib.crc32(raw[HEADER_SIZE:total], crc) & 0xFFFFFFFF
        if crc != h["crc32"]:
            problems.append("CRC32 不匹配：文件 0x%08X，重算 0x%08X" % (h["crc32"], crc))
        else:
            print("[verify] CRC32 校验通过：0x%08X" % crc)

    if hw_compat_expected is not None and h["hw_compat"] != hw_compat_expected:
        problems.append("硬件兼容 ID 0x%08X 与配置 0x%08X 不一致" % (h["hw_compat"], hw_compat_expected))

    if header_path:
        layout = load_layout(header_path)
        slot = "APP_SLOT0" if h["type"] == TYPE_APP else "DRIVER_POOL"
        base = layout.v(slot + "_BASE")
        size = layout.v(slot + "_SIZE")
        if h["load_addr"] != base:
            problems.append("加载地址 0x%08X 与 %s_BASE = 0x%08X 不一致" % (h["load_addr"], slot, base))
        if total > size:
            problems.append("镜像 %d 字节超出槽位容量 %d 字节" % (total, size))
        if h["hw_compat"] != layout.v("SVCRT_HW_COMPAT_ID"):
            problems.append("硬件兼容 ID 与 config/svcrt_partition.h 不一致")
        print("[verify] 槽位 %s = 0x%08X（容量 %d KB），镜像占用 %d 字节"
              % (slot, base, size // 1024, total))

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
        description="SVCrtOS App/驱动镜像打包工具",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    src = ap.add_argument_group("输入")
    src.add_argument("--axf", help="Keil 生成的 ELF（推荐，可自动解析入口符号）")
    src.add_argument("--bin", help="Keil/fromelf 生成的原始二进制")
    src.add_argument("--type", choices=["app", "driver"], default="app", help="镜像类型")
    src.add_argument("--version", help="版本号，如 1.2.3 或 0x010203")
    src.add_argument("--name", help="应用名称（写入预留清单，便于后续应用商店展示）")
    src.add_argument("--entry-symbol", help="入口符号（默认 app:APPSTART / driver:DRVSTART）")
    src.add_argument("--entry-offset", type=lambda x: int(x, 0), help="入口偏移（手工指定，优先级最高）")
    src.add_argument("--load-addr", type=lambda x: int(x, 0), help="镜像负载基址（默认取 槽位基址+镜像头长度，即 APP_IMAGE_HEADER_SIZE）")
    src.add_argument("--hw-compat", type=lambda x: int(x, 0), help="硬件兼容 ID（默认取配置头）")
    src.add_argument("--header", default="config/svcrt_partition.h", help="分区配置头文件")
    out = ap.add_argument_group("输出与动作")
    out.add_argument("--out", help="输出 .svcapp 路径（默认与输入同目录同名）")
    out.add_argument("--info", metavar="SVCRT_IMAGE", help="查看已有镜像头")
    out.add_argument("--verify", metavar="SVCRT_IMAGE", help="校验已有镜像")
    args = ap.parse_args()

    if args.info:
        return do_info(args.info)
    if args.verify:
        expected = load_layout(args.header).v("SVCRT_HW_COMPAT_ID")
        return do_verify(args.verify, args.header, expected)

    if not args.axf and not args.bin:
        ap.error("请给出 --axf 或 --bin（或用 --info / --verify 查看已有镜像）")

    img_type = TYPE_APP if args.type == "app" else TYPE_DRIVER
    if not args.entry_symbol:
        args.entry_symbol = DEFAULT_ENTRY[img_type]

    return do_pack(args)


if __name__ == "__main__":
    sys.exit(main())
