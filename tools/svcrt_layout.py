#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
SVCrtOS device-side layout configuration tool (host side)
=========================================================

Builds, checks and exports the persistent configuration record that tells a
board how to install applications and drivers, so the same firmware can be
shipped to customers who want different layouts.

The record lives in the CONFIG region (one erase unit between the kernel and
the image pool) and is written over the serial link by tools/svcrt_cfg.py.
This tool only produces the bytes; it never touches a device by itself.

Record layout, field names and error codes mirror
kernelsrc/include/svcrt_layout_def.h exactly -- that header is the contract.
Every rule enforced here is also enforced by svcrt_layout_validate() in
kernelsrc/src/svcrt_layout.c, on purpose: a record this tool calls valid but
the device rejects is worse than useless, because the device silently falls
back to the compile-time default layout and the operator never learns why.

Usage:
    # what the current config header says (compile-time fallback)
    python tools/svcrt_layout.py show

    # write an example configuration to edit
    python tools/svcrt_layout.py template --mode fixed > layout.json

    # check only (no output files)
    python tools/svcrt_layout.py check --config layout.json

    # build the 512-byte record, the per-slot .sct files and the header
    # fragment that reproduces this layout as the compile-time default
    python tools/svcrt_layout.py build --config layout.json \
        --bin build/layout.bin --sct-dir build --emit-header
"""

from __future__ import print_function

import argparse
import json
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from gen_scatter import DEFAULT_HEADER, REPO_ROOT, MacroEval, parse_defines  # noqa: E402

# ------------------------------------------------------------------ ABI
# Keep in sync with kernelsrc/include/svcrt_layout_def.h.
CFG_MAGIC = 0x47464353
CFG_VERSION = 1
CFG_RECORD_SIZE = 512
CFG_SLOT_SIZE = 32
CFG_CRC_OFFSET = CFG_RECORD_SIZE - 4
# Tail geometry: 7 knob words + 48 reserved words + the CRC word.
CFG_KNOB_WORDS = 7
CFG_RESERVED_WORDS = 48

TYPE_UNUSED = 0
TYPE_APP = 1
TYPE_DRIVER = 2
TYPE_NAME = {TYPE_UNUSED: "unused", TYPE_APP: "app", TYPE_DRIVER: "driver"}
TYPE_VALUE = {"unused": TYPE_UNUSED, "app": TYPE_APP, "driver": TYPE_DRIVER, "drv": TYPE_DRIVER}

MODE_AUTO = 0
MODE_FIXED = 1
MODE_VALUE = {"auto": MODE_AUTO, "fixed": MODE_FIXED}
MODE_NAME = {MODE_AUTO: "auto", MODE_FIXED: "fixed"}

RECLAIM_VALUE = {"global": 0, "minimal": 1, "none": 2}
RECLAIM_NAME = {0: "global", 1: "minimal", 2: "none"}

# svcrt_layout_validate() result codes, same numbering as the kernel.
ERR = {
    0: "OK",
    1: "ERR_MAGIC",
    2: "ERR_VERSION",
    3: "ERR_CRC",
    4: "ERR_HW",
    5: "ERR_MODE",
    6: "ERR_COUNT",
    7: "ERR_SLOT_TYPE",
    8: "ERR_SLOT_RANGE",
    9: "ERR_SLOT_SIZE",
    10: "ERR_SLOT_ALIGN",
    11: "ERR_SLOT_OVERLAP",
    12: "ERR_SLOT_RAM",
    13: "ERR_SLOT_COUNT_MAX",
    14: "ERR_RECLAIM",
    15: "ERR_KNOB",
    16: "ERR_NOT_IMPL",
    17: "ERR_RESERVED",
    18: "ERR_SLOTS_IN_MODE",
}
ERR_HINT = {
    4: "hw_compat_id does not match this board; check SVCRT_HW_COMPAT_ID in the config header",
    5: "mode must be 'auto' or 'fixed'",
    6: "slot_count is outside 0..SVCRT_CFG_SLOT_MAX",
    7: "slot type must be 1 (app) or 2 (driver); 0 means the entry is not used",
    8: "slot base/size is not inside the image pool",
    9: "slot is smaller than the image header plus a minimal payload",
    10: ("slot base is not aligned to SVCRT_POOL_ALLOC_UNIT, or (fixed mode) "
         "base/size does not span whole IMAGE_POOL_SECTOR sectors"),
    11: "two slots overlap in Flash or in their RAM windows",
    12: ("RAM window is not a power of two, out of the RAM pool, misaligned, "
         "or (fixed mode) not pinned at all"),
    13: "more slots than the kernel can hold",
    14: "reclaim_mode must be 'global' or 'minimal'",
    15: ("a runtime knob is out of range (log_level 0..4, fault_restart_max 0..64, "
         "boot_delay_ms 0..60000)"),
    16: ("the field is valid but this kernel build does not honour it: "
         "watchdog_ms / heap_size / thread_stack_default must stay 0; "
         "RAW_ALLOW needs a build with APP_ALLOW_RAW_IMAGE=1"),
    17: "reserved words must be zero",
    18: "'fixed' mode must carry a slot table, 'auto' mode must not",
}

# Knob limits as svcrt_layout_validate() checks them.
LOG_LEVEL_MAX = 4          # SVCRT_LOG_DEBUG
FAULT_RESTART_MAX = 64
BOOT_DELAY_MAX = 60000     # SVCRT_CFG_BOOT_DELAY_MAX

# svcrt_cfg_record_t.flags bits (SVCRT_CFG_FLAG_x).
FLAG_RAW_ALLOW = 0x1       # accept a bare image burned directly in the pool

# ------------------------------------------------------------------ helpers


def die(msg):
    print("ERROR: %s" % msg, file=sys.stderr)
    raise SystemExit(2)


def as_int(value, what="value"):
    """Accept 123, "123", "0x1F", "0b1010" and None -> 0."""
    if value is None:
        return 0
    if isinstance(value, bool):
        return 1 if value else 0
    if isinstance(value, int):
        return value
    if isinstance(value, str):
        text = value.strip()
        try:
            return int(text, 0)
        except ValueError:
            die("%s: cannot parse %r as a number" % (what, value))
    die("%s: expected a number, got %r" % (what, value))


def pow2(n):
    return n > 0 and (n & (n - 1)) == 0


class Board(object):
    """Layout geometry, expanded from config/svcrt_partition.h."""

    NAMES = (
        "IMAGE_POOL_BASE", "IMAGE_POOL_SIZE", "IMAGE_POOL_END",
        "IMAGE_POOL_SECTOR", "IMAGE_POOL_UNITS", "SVCRT_POOL_ALLOC_UNIT",
        "APP_IMAGE_HEADER_SIZE",
        "SLOT_RAM_BASE", "SLOT_RAM_TOTAL", "SLOT_RAM_MIN_BLOCK",
        "SLOT_RAM_MAX_BLOCK", "SVCRT_HW_COMPAT_ID", "SVCRT_CFG_SLOT_MAX",
        "SVCRT_LAYOUT_DEFAULT_MODE", "SVCRT_RECLAIM_MODE",
    )

    def __init__(self, header):
        self.header = header
        defines = parse_defines(header)
        self.ev = MacroEval(defines)
        self.missing = []
        for name in self.NAMES:
            try:
                self.ev.value(name)
            except (KeyError, RuntimeError):
                self.missing.append(name)
        if self.missing:
            die("config header %s does not define: %s"
                % (header, ", ".join(self.missing)))

    def v(self, name):
        return self.ev.value(name)

    def slot_max(self):
        """Slot capacity of the record, with the ABI geometry re-derived.

        svcrt_layout_def.h asserts sizeof(svcrt_cfg_record_t) == 512, so a
        SVCRT_CFG_SLOT_MAX that no longer fits means the header and the tools
        talk about different records. Refuse rather than write a misaligned
        record the device would reject with a CRC error.
        """
        n = self.v("SVCRT_CFG_SLOT_MAX")
        total = 32 + n * CFG_SLOT_SIZE \
            + (CFG_KNOB_WORDS + CFG_RESERVED_WORDS + 1) * 4
        if total != CFG_RECORD_SIZE:
            die("SVCRT_CFG_SLOT_MAX=%d makes the record %d bytes, but the ABI "
                "in svcrt_layout_def.h is fixed at %d" % (n, total, CFG_RECORD_SIZE))
        return n

    def unit_of(self, addr):
        base = self.v("IMAGE_POOL_BASE")
        sector = self.v("IMAGE_POOL_SECTOR")
        if (addr < base) or ((addr - base) % sector) != 0:
            return None
        return (addr - base) // sector

    def default_slots(self):
        """The compile-time default slot table (SVCRT_CFG_SLOTn_* macros)."""
        out = []
        for i in range(self.v("SVCRT_CFG_SLOT_MAX")):
            get = lambda k: self.ev.value("SVCRT_CFG_SLOT%d_%s" % (i, k))  # noqa: E731
            out.append({
                "base": get("BASE"),
                "size": get("SIZE"),
                "type": get("TYPE"),
                "ram_size": get("RAM_SIZE"),
                "autostart": get("AUTOSTART"),
            })
        return out


# ------------------------------------------------------------------ record


def build_record(cfg, board, seq=1, tool_version=1):
    """Turn a parsed configuration into the 512-byte record (bytes)."""
    rec = bytearray(CFG_RECORD_SIZE)

    mode = MODE_VALUE.get(str(cfg.get("mode", "auto")).lower())
    slots = cfg.get("slots") or []
    knobs = cfg.get("knobs") or {}
    reclaim = RECLAIM_VALUE.get(str(cfg.get("reclaim_mode", "global")).lower())

    def put(off, value):
        rec[off:off + 4] = struct.pack("<I", value & 0xFFFFFFFF)

    put(0, CFG_MAGIC)
    put(4, CFG_VERSION)
    put(8, board.v("SVCRT_HW_COMPAT_ID"))
    put(12, mode if mode is not None else 0xFFFFFFFF)
    put(16, seq & 0xFFFFFFFF)
    put(20, len(slots))
    put(24, as_int(cfg.get("flags"), "flags"))
    put(28, reclaim if reclaim is not None else 0xFFFFFFFF)

    slot_max = board.slot_max()
    for i, slot in enumerate(slots):
        if i >= slot_max:
            break
        off = 32 + i * CFG_SLOT_SIZE
        stype = slot.get("type")
        stype = TYPE_VALUE.get(str(stype).lower()) if isinstance(stype, str) else as_int(stype, "slot.type")
        put(off + 0, as_int(slot.get("base"), "slot.base"))
        put(off + 4, as_int(slot.get("size"), "slot.size"))
        put(off + 8, stype if stype is not None else 0xFF)
        put(off + 12, as_int(slot.get("ram_base"), "slot.ram_base"))
        put(off + 16, as_int(slot.get("ram_size"), "slot.ram_size"))
        put(off + 20, as_int(slot.get("autostart"), "slot.autostart"))
        put(off + 24, as_int(slot.get("flags"), "slot.flags"))
        put(off + 28, as_int(slot.get("reserved"), "slot.reserved"))

    base = 32 + slot_max * CFG_SLOT_SIZE             # 32 + 256 = 288
    put(base + 0, as_int(knobs.get("log_level"), "knobs.log_level"))
    put(base + 4, as_int(knobs.get("fault_restart_max"), "knobs.fault_restart_max"))
    put(base + 8, as_int(knobs.get("boot_delay_ms"), "knobs.boot_delay_ms"))
    put(base + 12, as_int(knobs.get("watchdog_ms"), "knobs.watchdog_ms"))
    put(base + 16, as_int(knobs.get("heap_size"), "knobs.heap_size"))
    put(base + 20, as_int(knobs.get("thread_stack_default"), "knobs.thread_stack_default"))
    put(base + 24, as_int(tool_version, "tool_version"))

    rec[CFG_CRC_OFFSET:CFG_RECORD_SIZE] = b"\x00\x00\x00\x00"
    crc = zlib.crc32(bytes(rec)) & 0xFFFFFFFF
    rec[CFG_CRC_OFFSET:CFG_RECORD_SIZE] = struct.pack("<I", crc)
    return bytes(rec)


def parse_record(rec, board):
    """Inverse of build_record, used by the self check and by --dump-bin."""
    if len(rec) != CFG_RECORD_SIZE:
        die("record is %d bytes, expected %d" % (len(rec), CFG_RECORD_SIZE))
    w = lambda off: struct.unpack_from("<I", rec, off)[0]  # noqa: E731
    cfg = {
        "mode": MODE_NAME.get(w(12), w(12)),
        "seq": w(16),
        "flags": w(24),
        "reclaim_mode": RECLAIM_NAME.get(w(28), w(28)),
        "slots": [],
        "knobs": {},
    }
    slot_max = board.slot_max()
    for i in range(w(20) if w(20) <= slot_max else slot_max):
        off = 32 + i * CFG_SLOT_SIZE
        cfg["slots"].append({
            "base": w(off + 0), "size": w(off + 4), "type": w(off + 8),
            "ram_base": w(off + 12), "ram_size": w(off + 16),
            "autostart": w(off + 20), "flags": w(off + 24), "reserved": w(off + 28),
        })
    base = 32 + slot_max * CFG_SLOT_SIZE
    cfg["knobs"] = {
        "log_level": w(base + 0), "fault_restart_max": w(base + 4),
        "boot_delay_ms": w(base + 8), "watchdog_ms": w(base + 12),
        "heap_size": w(base + 16), "thread_stack_default": w(base + 20),
    }
    cfg["tool_version"] = w(base + 24)
    return cfg


# ------------------------------------------------------------------ check


def validate(cfg, board):
    """Mirror of svcrt_layout_validate(): return (code, detail)."""
    mode_name = str(cfg.get("mode", "auto")).lower()
    if mode_name not in MODE_VALUE:
        return 5, ERR_HINT[5]
    mode = MODE_VALUE[mode_name]

    slots = cfg.get("slots") or []
    knob = cfg.get("knobs") or {}

    if len(slots) > board.v("SVCRT_CFG_SLOT_MAX"):
        return 13, "slot_count=%d exceeds SVCRT_CFG_SLOT_MAX=%d" % (
            len(slots), board.v("SVCRT_CFG_SLOT_MAX"))

    pool_base = board.v("IMAGE_POOL_BASE")
    pool_end = board.v("IMAGE_POOL_END")
    unit = board.v("SVCRT_POOL_ALLOC_UNIT")
    hdr = board.v("APP_IMAGE_HEADER_SIZE")
    ram_base_pool = board.v("SLOT_RAM_BASE")
    ram_total = board.v("SLOT_RAM_TOTAL")
    ram_min = board.v("SLOT_RAM_MIN_BLOCK")
    ram_max = board.v("SLOT_RAM_MAX_BLOCK")

    used = []
    for i, slot in enumerate(slots):
        stype = slot.get("type")
        stype = TYPE_VALUE.get(str(stype).lower()) if isinstance(stype, str) else as_int(stype, "slot.type")
        if stype == TYPE_UNUSED:
            continue
        if stype not in (TYPE_APP, TYPE_DRIVER):
            return 7, "slot[%d] type must be 'app' or 'driver'" % i
        if as_int(slot.get("flags")) != 0 or as_int(slot.get("reserved")) != 0:
            return 17, "slot[%d] flags/reserved must be 0" % i
        base = as_int(slot.get("base"), "slot[%d].base" % i)
        size = as_int(slot.get("size"), "slot[%d].size" % i)
        if (base < pool_base) or (base >= pool_end) or (size > (pool_end - base)):
            return 8, ("slot[%d] 0x%08X+0x%X is not inside the pool "
                       "(0x%08X..0x%08X)" % (i, base, size, pool_base, pool_end - 1))
        if size < (hdr + 256):
            return 9, "slot[%d] size 0x%X < header(%d)+256" % (i, size, hdr)
        if (base % unit) != 0:
            return 10, "slot[%d] base 0x%08X is not aligned to %d" % (i, base, unit)
        if mode == MODE_FIXED:
            sector = board.v("IMAGE_POOL_SECTOR")
            if ((base % sector) != 0) or ((size % sector) != 0):
                return 10, ("slot[%d] 0x%08X+0x%X must lie on whole %d-byte "
                            "physical sectors: uninstalling a fixed slot erases "
                            "exactly that slot, and a partial sector would take "
                            "a neighbour with it" % (i, base, size, sector))
        ram_size = as_int(slot.get("ram_size"), "slot[%d].ram_size" % i)
        ram_base = as_int(slot.get("ram_base"), "slot[%d].ram_base" % i)
        if mode == MODE_FIXED and (ram_size == 0 or ram_base == 0):
            return 12, ("slot[%d] must pin its RAM window (ram_base and "
                        "ram_size) in fixed-slot mode" % i)
        if ram_size != 0:
            if not pow2(ram_size) or ram_size < ram_min or ram_size > ram_max:
                return 12, ("slot[%d] ram_size 0x%X must be a power of two in "
                            "0x%X..0x%X" % (i, ram_size, ram_min, ram_max))
            if ram_base != 0:
                if ((ram_base % ram_size) != 0) or (ram_base < ram_base_pool) or \
                        (ram_size > ((ram_base_pool + ram_total) - ram_base)):
                    return 12, "slot[%d] ram_base 0x%08X/0x%X is not a valid window" % (
                        i, ram_base, ram_size)
        used.append((i, base, size, ram_base, ram_size))

    for a in range(len(used)):
        i, base, size, rb, rs = used[a]
        for b in range(a):
            j, ob, osz, orb, ors = used[b]
            if (base < (ob + osz)) and (ob < (base + size)):
                return 11, "slot[%d] and slot[%d] overlap in Flash" % (i, j)
            if rs and ors and rb and orb:
                if (rb < (orb + ors)) and (orb < (rb + rs)):
                    return 11, "slot[%d] and slot[%d] overlap in RAM" % (i, j)

    if mode == MODE_FIXED and not used:
        return 18, "fixed mode needs at least one usable slot"
    if mode == MODE_AUTO and used:
        return 18, "auto mode must not carry a slot table (%d entries given)" % len(used)

    reclaim = RECLAIM_VALUE.get(str(cfg.get("reclaim_mode", "global")).lower())
    if reclaim is None or reclaim not in (0, 1):
        return 14, "reclaim_mode must be 'global' or 'minimal'"

    log_level = as_int(knob.get("log_level"), "knobs.log_level")
    restart = as_int(knob.get("fault_restart_max"), "knobs.fault_restart_max")
    if log_level > LOG_LEVEL_MAX:
        return 15, "log_level %d > %d" % (log_level, LOG_LEVEL_MAX)
    if restart > FAULT_RESTART_MAX:
        return 15, "fault_restart_max %d > %d" % (restart, FAULT_RESTART_MAX)

    boot_delay = as_int(knob.get("boot_delay_ms"), "knobs.boot_delay_ms")
    if boot_delay > BOOT_DELAY_MAX:
        return 15, "boot_delay_ms %d > %d" % (boot_delay, BOOT_DELAY_MAX)

    flags = as_int(cfg.get("flags"), "flags")
    if flags & ~FLAG_RAW_ALLOW:
        return 17, ("flags 0x%X carries bits this kernel does not define "
                    "(only RAW_ALLOW=0x1)" % flags)
    # The three fields below are still placeholders in the record: the kernel
    # rejects a non-zero value instead of silently ignoring it. Sending one
    # from here would produce a configuration the operator believes in and the
    # device does not honour, so the tool must refuse it first.
    for name in ("watchdog_ms", "heap_size", "thread_stack_default"):
        if as_int(knob.get(name), "knobs.%s" % name) != 0:
            return 16, "%s is valid but this kernel build ignores it" % name
    return 0, ""


# ------------------------------------------------------------------ output


def cmd_show(args):
    board = Board(args.header)
    print("board: %s" % args.header)
    print("  hw_compat_id     0x%08X" % board.v("SVCRT_HW_COMPAT_ID"))
    print("  pool             0x%08X..0x%08X  (%d KB, %d units x %d KB, alloc unit %d)"
          % (board.v("IMAGE_POOL_BASE"), board.v("IMAGE_POOL_END") - 1,
             board.v("IMAGE_POOL_SIZE") // 1024, board.v("IMAGE_POOL_UNITS"),
             board.v("IMAGE_POOL_SECTOR") // 1024, board.v("SVCRT_POOL_ALLOC_UNIT")))
    print("  image RAM pool   0x%08X (%d KB, block 0x%X..0x%X)"
          % (board.v("SLOT_RAM_BASE"), board.v("SLOT_RAM_TOTAL") // 1024,
             board.v("SLOT_RAM_MIN_BLOCK"), board.v("SLOT_RAM_MAX_BLOCK")))
    print("  default mode     %s" % MODE_NAME.get(board.v("SVCRT_LAYOUT_DEFAULT_MODE"),
                                                 board.v("SVCRT_LAYOUT_DEFAULT_MODE")))
    print("  reclaim mode     %s" % RECLAIM_NAME.get(board.v("SVCRT_RECLAIM_MODE"),
                                                    board.v("SVCRT_RECLAIM_MODE")))
    print("  slot_max         %d" % board.v("SVCRT_CFG_SLOT_MAX"))
    print("compile-time default slot table:")
    for i, slot in enumerate(board.default_slots()):
        if slot["type"] == TYPE_UNUSED:
            print("  [%d] (unused)" % i)
            continue
        print("  [%d] type=%-6s base=0x%08X size=0x%X ram_size=0x%X autostart=%d"
              % (i, TYPE_NAME.get(slot["type"], "?"), slot["base"], slot["size"],
                 slot["ram_size"], slot["autostart"]))
    return 0


def cmd_template(args):
    board = Board(args.header)
    base = args.pool_base or board.v("IMAGE_POOL_BASE")
    sector = args.sector or board.v("IMAGE_POOL_SECTOR")
    ram_min = board.v("SLOT_RAM_MIN_BLOCK")
    ram_max = board.v("SLOT_RAM_MAX_BLOCK")
    ram_app = min(ram_min * 4, ram_max)
    tpl = {
        "mode": args.mode,
        "reclaim_mode": "global",
        # flags: 0 = strict installation only. 0x1 (RAW_ALLOW) additionally
        # accepts a bare image burned directly at a slot base; it only means
        # something on a build whose board header sets APP_ALLOW_RAW_IMAGE=1.
        "flags": 0,
        "knobs": {
            "log_level": 0,        # 0 = keep the compile-time level
            "fault_restart_max": 0,
            "boot_delay_ms": 0,    # ms to hold before autostart (0 = straight to work)
        },
        "slots": [],
    }
    if args.mode == "fixed":
        # A fixed slot pins both its Flash placement and its RAM window: the
        # kernel refuses a fixed slot without one, because a fixed-address
        # image that shares the buddy allocator can be handed RAM that another
        # slot already owns. The template therefore emits both.
        ram_pool = board.v("SLOT_RAM_BASE")
        tpl["slots"] = [
            {"type": "app", "base": "0x%08X" % base, "size": "0x%X" % sector,
             "ram_base": "0x%08X" % ram_pool,
             "ram_size": "0x%X" % ram_app, "autostart": 1},
            {"type": "driver", "base": "0x%08X" % (base + sector),
             "size": "0x%X" % sector,
             "ram_base": "0x%08X" % (ram_pool + ram_app),
             "ram_size": "0x%X" % ram_min, "autostart": 1},
        ]
    print(json.dumps(tpl, indent=2))
    return 0


def _load(args):
    if args.config:
        with open(args.config, "r", encoding="utf-8") as fh:
            try:
                cfg = json.load(fh)
            except ValueError as exc:
                die("%s is not valid JSON: %s" % (args.config, exc))
        if not isinstance(cfg, dict):
            die("%s must contain a JSON object" % args.config)
    else:
        cfg = json.loads(args.json) if args.json else {}
    return cfg


def cmd_check(args):
    board = Board(args.header)
    cfg = _load(args)
    code, detail = validate(cfg, board)
    if code != 0:
        print("REJECTED: %s (%d) -- %s" % (ERR[code], code, detail))
        if code in ERR_HINT:
            print("  hint: %s" % ERR_HINT[code])
        return 1
    print("OK: configuration is valid for %s" % os.path.basename(board.header))
    return 0


def header_fragment(cfg, board):
    """Emit the SVCRT_CFG_SLOTn_* block that reproduces this layout."""
    slots = cfg.get("slots") or []
    lines = []
    for i in range(board.v("SVCRT_CFG_SLOT_MAX")):
        slot = slots[i] if i < len(slots) else {}
        stype = slot.get("type")
        stype = TYPE_VALUE.get(str(stype).lower()) if isinstance(stype, str) else as_int(stype)
        lines.append("#define SVCRT_CFG_SLOT%d_BASE      (0x%08X)" % (i, as_int(slot.get("base"))))
        lines.append("#define SVCRT_CFG_SLOT%d_SIZE      (0x%08X)" % (i, as_int(slot.get("size"))))
        lines.append("#define SVCRT_CFG_SLOT%d_TYPE      (%d)" % (i, stype or 0))
        lines.append("#define SVCRT_CFG_SLOT%d_RAM_SIZE  (0x%08X)" % (i, as_int(slot.get("ram_size"))))
        lines.append("#define SVCRT_CFG_SLOT%d_AUTOSTART (%d)" % (i, as_int(slot.get("autostart"))))
        lines.append("")
    return "\n".join(lines)


def cmd_build(args):
    board = Board(args.header)
    cfg = _load(args)

    code, detail = validate(cfg, board)
    if code != 0:
        print("REJECTED: %s (%d) -- %s" % (ERR[code], code, detail), file=sys.stderr)
        if code in ERR_HINT:
            print("  hint: %s" % ERR_HINT[code], file=sys.stderr)
        return 1

    seq = as_int(args.seq, "seq") or 1
    rec = build_record(cfg, board, seq=seq, tool_version=args.tool_version)

    # Read back what we just wrote: a build tool that trusts its own encoder
    # is exactly how a wrong layout gets flashed.
    back = parse_record(rec, board)
    if (len(back["slots"]) != len(cfg.get("slots") or [])) or \
            (MODE_NAME.get(back["mode"], back["mode"]) != str(cfg.get("mode", "auto")).lower()):
        print("INTERNAL: record did not round-trip, refusing to write", file=sys.stderr)
        return 3

    if args.bin:
        with open(args.bin, "wb") as fh:
            fh.write(rec)
        print("wrote %s (%d bytes, seq=%d, crc=0x%08X)"
              % (args.bin, len(rec), seq, struct.unpack_from("<I", rec, CFG_CRC_OFFSET)[0]))

    if args.sct_dir:
        made = emit_sct(cfg, board, args.sct_dir)
        print("wrote %d .sct file(s) to %s" % (made, args.sct_dir))

    if args.emit_header:
        print()
        print("/* ---- paste into config/svcrt_partition.h section 11 ---- */")
        print(header_fragment(cfg, board))

    if args.hex:
        for off in range(0, CFG_RECORD_SIZE, 16):
            chunk = rec[off:off + 16]
            print("%04X  %s  |%s|" % (
                off,
                " ".join("%02X" % b for b in chunk),
                "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)))
    return 0


def emit_sct(cfg, board, outdir):
    """One .sct per slot, generated through gen_scatter so the numbers agree."""
    import subprocess

    if not os.path.isdir(outdir):
        os.makedirs(outdir)
    gen = os.path.join(REPO_ROOT, "tools", "gen_scatter.py")
    made = 0
    for i, slot in enumerate(cfg.get("slots") or []):
        base = as_int(slot.get("base"), "slot[%d].base" % i)
        unit = board.unit_of(base)
        if unit is None:
            die("slot[%d] base 0x%08X is not on an allocation unit boundary" % (i, base))
        units = as_int(slot.get("size"), "slot[%d].size" % i) // board.v("IMAGE_POOL_SECTOR")
        stype = slot.get("type")
        stype = TYPE_VALUE.get(str(stype).lower()) if isinstance(stype, str) else as_int(stype)
        name = "%s_slot%d.sct" % (TYPE_NAME.get(stype, "image"), i)
        cmd = [sys.executable, gen, "--target", "image",
               "--unit", str(unit), "--units", str(units),
               "--type", "driver" if stype == TYPE_DRIVER else "app",
               "--header", board.header,
               "--output", os.path.join(outdir, name)]
        rc = subprocess.call(cmd)
        if rc != 0:
            die("gen_scatter failed for slot %d (exit %d)" % (i, rc))
        made += 1
    return made


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Build the SVCrtOS device-side layout configuration record.")
    ap.add_argument("--header", default=DEFAULT_HEADER,
                    help="partition header (default: config/svcrt_partition.h)")
    sub = ap.add_subparsers(dest="cmd")

    p = sub.add_parser("show", help="print the compile-time fallback layout")
    p.set_defaults(func=cmd_show)

    p = sub.add_parser("template", help="print an example configuration JSON")
    p.add_argument("--mode", default="fixed", choices=("auto", "fixed"))
    p.add_argument("--pool-base", type=lambda s: int(s, 0), default=None)
    p.add_argument("--sector", type=lambda s: int(s, 0), default=None)
    p.set_defaults(func=cmd_template)

    p = sub.add_parser("check", help="validate a configuration, write nothing")
    p.add_argument("--config")
    p.add_argument("--json")
    p.set_defaults(func=cmd_check)

    p = sub.add_parser("build", help="validate and build the record")
    p.add_argument("--config")
    p.add_argument("--json")
    p.add_argument("--bin", help="write the 512-byte record here")
    p.add_argument("--sct-dir", help="also emit one .sct per slot into this directory")
    p.add_argument("--emit-header", action="store_true",
                   help="print the SVCRT_CFG_SLOTn_* block for the config header")
    p.add_argument("--seq", default=1)
    p.add_argument("--tool-version", type=int, default=1)
    p.add_argument("--hex", action="store_true", help="dump the record as hex")
    p.set_defaults(func=cmd_build)

    args = ap.parse_args(argv)
    if not getattr(args, "func", None):
        ap.print_help()
        return 2
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
