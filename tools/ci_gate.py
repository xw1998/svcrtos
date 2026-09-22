#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""SVCrtOS CI gate: the checks that must pass before anything lands on master.

Every check here exists because the corresponding mistake actually happened in
this repo.  A check that only prints and never fails is worse than no check, so
each one returns pass/fail and the script exits non-zero on any failure.

The first version of this script produced three false failures and one false
pass, all of which are now fixed and worth knowing about:
  * the F401 partition header is *supposed* to differ on chip capability
    (flash / ram / CCM / hw compat id) - only the derived geometry must match;
  * CMSIS device headers and DSP tables are third party and full of peripheral
    base addresses, so they must not be policed for hardcoded pool addresses;
  * `gen_scatter.py --check` validates the layout, it does NOT prove a .sct on
    disk matches the generator.  That is a separate question and is answered by
    "is the .sct tracked at all" - if it is not tracked, a hand edit cannot
    reach master in the first place.

Usage:
    py -3 tools/ci_gate.py                 # structural checks only
    py -3 tools/ci_gate.py --build         # also compile both kernel projects
    py -3 tools/ci_gate.py --uv4 "D:/Keil_v5/UV4/UV4.exe"
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

PART_HDR_427 = "config/svcrt_partition.h"
PART_HDR_401 = "config/stm32f401/svcrt_partition.h"

# Macros that describe the *board*, not the layout.  The F401 twin is expected
# to differ on exactly these; anything else shared between the two headers is
# layout geometry and must stay identical.
#
# "Does this board have a second dev slot / a NOR / a filesystem at all" is a
# board capability question, and the answer on F401 is "no" - which is spelled
# 0, not "absent".  Those zeros are not geometry drift.
CHIP_ONLY_PREFIXES = ("CHIP_", "SVCRT_HW_COMPAT_ID")
CHIP_ONLY_EXACT = {
    "SVCRT_DEV_RAM_WINDOW",
    "SVCRT_DEV_SLOT2_UNIT",
    "SVCRT_DEV_SLOT2_BASE",
    "SVCRT_DEV_SLOT2_SIZE",
    "SVCRT_DEV_SLOT2_TYPE",
    "SVCRT_DEV_SLOT3_UNIT",
    "SVCRT_DEV_SLOT3_TYPE",
    "SVCRT_NOR_CHIP_SIZE",
    "SVCRT_FS_SIZE",
}

# Third-party trees: not ours to police.
THIRD_PARTY_MARKERS = ("/Drivers/", "/Middlewares/", "/RTE/", "/DebugConfig/",
                       "/CMSIS/", "Drivers/CMSIS")

# Trees that get compiled into a firmware image.  tools/ is host side and is
# policed by nothing here on purpose (see check_single_address_source).
TARGET_CODE_ROOTS = ("kernelsrc/", "board/", "config/", "example/")

# Images that are linked separately from the kernel and therefore must stay
# layout-agnostic.  The kernel's own loader (kernelsrc/src/svcrt_loader.c) is
# part of the kernel image and *must* see the partition header, so kernelsrc/src
# and kernelsrc/include are deliberately absent here.
APP_SIDE_MARKERS = ("kernelsrc/sdk/", "/app_sdk/", "/driver_sdk/")

ALLOWED_ENCODINGS = ("utf-8", "gbk")

SKIP_DIRS = {".git", "build", "docs", "_nova", "RTE", "DebugConfig",
             "Objects", "Listings"}

FAILURES = []
PASSED = []


def fail(check, detail):
    FAILURES.append("[%s] %s" % (check, detail))
    print("FAIL %-26s %s" % (check, detail))


def ok(check, detail=""):
    PASSED.append(check)
    print("ok   %-26s %s" % (check, detail))


def read(path):
    with open(path, "rb") as f:
        return f.read()


GITENV = dict(os.environ)
GITENV.pop("PYTHONHOME", None)
GITENV.pop("PYTHONPATH", None)

_TRACKED = None


def tracked_files():
    global _TRACKED
    if _TRACKED is None:
        r = subprocess.run(["git", "ls-files"], cwd=ROOT, env=GITENV,
                           capture_output=True)
        _TRACKED = set(l for l in (r.stdout or b"").decode("utf-8", "replace").splitlines() if l)
    return _TRACKED


def stored_bytes(rel, path):
    """The bytes that would actually be committed for this file.

    The working tree is *not* the truth: core.autocrlf=true rewrites LF-only
    files as CRLF on checkout, so a byte level EOL scan of the working tree
    reports failures git itself normalises away, and misses the one case that
    matters - a blob that already mixes both.  The index blob is what lands on
    master, so that is what gets scanned.  An untracked file has no blob yet,
    so its working copy *is* the future blob.

    Consequence: stage first, then run the gate.
    """
    if rel in tracked_files():
        r = subprocess.run(["git", "show", ":" + rel], cwd=ROOT, env=GITENV,
                           capture_output=True)
        if r.returncode == 0:
            return r.stdout
    return read(path)


def is_third_party(rel):
    return any(m in rel for m in THIRD_PARTY_MARKERS)


def own_sources():
    """Tracked-looking sources that are ours (no third-party trees)."""
    out = []
    for base in ("kernelsrc", "board", "config", "tools", "example"):
        root = os.path.join(ROOT, base)
        if not os.path.isdir(root):
            continue
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
            for name in filenames:
                if not name.endswith((".c", ".h", ".s", ".S", ".py")):
                    continue
                rel = os.path.relpath(os.path.join(dirpath, name), ROOT).replace("\\", "/")
                if is_third_party(rel):
                    continue
                out.append((rel, os.path.join(dirpath, name)))
    return sorted(out)


def strip_comments_bytes(raw):
    """Byte level C comment stripper.

    Works on bytes, not text: these files are GBK/UTF-8 mixed, so decoding with
    'replace' before scanning can shift what is left.  Handles // line comments,
    /* */ block comments including the multi-line form whose continuation lines
    start with '*', and skips string / char literals so that a '/*' inside a
    string does not swallow the rest of the line (which would hide a real hit).
    An address that survives this is a constant, not prose.
    """
    out = bytearray()
    i, n = 0, len(raw)
    quote = 0
    while i < n:
        c = raw[i]
        if quote:
            out.append(c)
            if c == 0x5C and i + 1 < n:          # backslash escape
                out.append(raw[i + 1])
                i += 2
                continue
            if c == quote:
                quote = 0
            i += 1
            continue
        if c in (0x22, 0x27):                    # " or '
            quote = c
            out.append(c)
            i += 1
            continue
        if c == 0x2F and i + 1 < n and raw[i + 1] == 0x2F:      # //
            while i < n and raw[i] != 0x0A:
                i += 1
            continue
        if c == 0x2F and i + 1 < n and raw[i + 1] == 0x2A:      # /*
            i += 2
            while i < n:
                if raw[i] == 0x2A and i + 1 < n and raw[i + 1] == 0x2F:
                    i += 2
                    break
                if raw[i] == 0x0A:
                    out.append(0x0A)             # keep the line structure
                i += 1
            continue
        out.append(c)
        i += 1
    return bytes(out)


def code_lines(raw):
    """Yield (lineno, bytes) for every line, comments already removed."""
    stripped = strip_comments_bytes(raw)
    for i, line in enumerate(stripped.split(b"\n"), 1):
        yield i, line


# ---------------------------------------------------------------- check 1
def check_partition_twins():
    """Layout geometry must agree between the two partition headers.

    Chip capability is allowed to differ by design (F427 1MB/128KB/CCM64 vs
    F401 512KB/96KB/no CCM, different hw compat id).  Everything else is
    derived geometry and a drift there means one board runs a layout that
    exists nowhere in the repository.
    """
    b = os.path.join(ROOT, PART_HDR_401)
    if not os.path.exists(b):
        ok("partition-twin", "no F401 copy present")
        return

    def macros(path):
        txt = read(path).decode("utf-8", "replace")
        out = {}
        for m in re.finditer(r"^\s*#define\s+(\w+)\s+(.+?)\s*$", txt, re.M):
            # Drop the trailing comment: the same value documented differently
            # in the two twins is not a drift.
            val = re.sub(r"/\*.*?\*/", "", m.group(2)).strip()
            out[m.group(1)] = val
        return out

    ma, mb = macros(os.path.join(ROOT, PART_HDR_427)), macros(b)
    geometry, chip_only = [], []
    for k in ma:
        if not (k.startswith("SVCRT_") or k.startswith("CHIP_")):
            continue
        if k.startswith(CHIP_ONLY_PREFIXES) or k in CHIP_ONLY_EXACT:
            chip_only.append(k)
        else:
            geometry.append(k)

    drift = []
    for k in geometry:
        if k in mb and ma[k] != mb[k]:
            drift.append("%s: %s vs %s" % (k, ma[k], mb[k]))
    missing = [k for k in geometry if k not in mb]
    if drift or missing:
        detail = list(drift)
        if missing:
            detail.append("missing in F401: %s" % ", ".join(missing))
        fail("partition-twin", "; ".join(detail[:6]))
    else:
        ok("partition-twin",
           "%d geometry macros agree, %d chip-only allowed to differ"
           % (len(geometry), len(chip_only)))


# ---------------------------------------------------------------- check 2
def check_single_address_source():
    """No hardcoded pool / slot / image-ram address in code that ships in an image.

    The partition headers (and the generated .sct) are the only sources.
    Comments are excluded: a comment that *names* an address is documentation,
    not a constant a layout change can leave stale.

    Scope is deliberately the trees that get compiled into a firmware image
    (kernelsrc / board / example).  tools/ is host side: its only literals are
    GUI form defaults and an error message that offers 0x08040000 as an example
    of a legal integer (svcrt_host_gui.py).  Those are UI text, not layout
    constants, and policing them produced the first version's false failures.
    """
    pat = re.compile(rb"0x0[28]0[0-9A-Fa-f]{5}")
    hits = []
    for rel, path in own_sources():
        if not rel.startswith(TARGET_CODE_ROOTS):
            continue
        if rel.endswith((".sct", "svcrt_partition.h")):
            continue
        for i, line in code_lines(stored_bytes(rel, path)):
            if b"0x080" not in line and b"0x200" not in line:
                continue
            if pat.search(line):
                hits.append("%s:%d" % (rel, i))
    if hits:
        fail("single-address-source", "%d literal(s): %s" % (len(hits), ", ".join(hits[:8])))
    else:
        ok("single-address-source", "no literals in image code")


# ---------------------------------------------------------------- check 3
def check_encoding_eol():
    """Every source decodes as one allowed encoding; no blob mixes CRLF and LF.

    Mixed endings are a failure, not a note: they are what turned a real
    153-line change into a 5739-line diff that nobody could review.

    Scanned on the index blob (see stored_bytes) - the working tree is
    autocrlf-rewritten and says nothing about what would be committed.
    """
    bad, mixed = [], []
    for rel, path in own_sources():
        raw = stored_bytes(rel, path)
        if not raw:
            continue
        decoded = None
        for enc in ALLOWED_ENCODINGS:
            try:
                raw.decode(enc)
                decoded = enc
                break
            except UnicodeDecodeError:
                continue
        if decoded is None:
            bad.append(rel)
            continue
        crlf = raw.count(b"\r\n")
        lf = raw.count(b"\n") - crlf
        if crlf and lf:
            mixed.append("%s (crlf=%d lf=%d)" % (rel, crlf, lf))
    if bad:
        fail("encoding", "%d file(s) decode as neither utf-8 nor gbk: %s"
             % (len(bad), ", ".join(bad[:5])))
    else:
        ok("encoding", "all own sources decode")
    if mixed:
        fail("mixed-eol", "%d file(s): %s" % (len(mixed), ", ".join(mixed[:6])))
    else:
        ok("mixed-eol", "no file mixes CRLF and LF")


# ---------------------------------------------------------------- check 4
def check_layout_and_scatter():
    """Two different questions, answered separately.

    (a) Does the layout in the partition header satisfy every constraint?
        gen_scatter.py --check answers this for real.
    (b) Is a .sct on disk guaranteed to be the generator's output?
        Only if the file is not tracked: an untracked build product cannot
        carry a hand edit into master.  Claiming (b) from (a) is a false pass.
    """
    env = dict(os.environ)
    env.pop("PYTHONHOME", None)
    env.pop("PYTHONPATH", None)
    r = subprocess.run([sys.executable, os.path.join("tools", "gen_scatter.py"), "--check"],
                       cwd=ROOT, env=env, capture_output=True, encoding="utf-8",
                       errors="replace")
    if r.returncode == 0 and "检查通过" in (r.stdout or ""):
        ok("layout-check", "partition layout has no overlap / out of range")
    else:
        fail("layout-check", ((r.stdout or "") + (r.stderr or "")).strip()[:300])

    r = subprocess.run(["git", "ls-files", "*.sct"], cwd=ROOT, env=env,
                       capture_output=True, encoding="utf-8", errors="replace")
    tracked = [l for l in (r.stdout or "").splitlines() if l.strip()]
    if tracked:
        fail("sct-not-tracked", "generated .sct is in the index: %s" % ", ".join(tracked[:5]))
    else:
        ok("sct-not-tracked", "no .sct is tracked (build product stays out of git)")


# ---------------------------------------------------------------- check 5
def check_no_partition_include_in_app():
    """Separately linked images must not include the partition header.

    App / driver SDK code and the example app / driver projects take their
    layout at run time through SVC 0x18.  Only #include lines count - a comment
    that points at the header as the place where the layout is decided is
    exactly the documentation we want.

    board/ and the kernel projects are *allowed* to include it (they are the
    single source), and so is kernelsrc/src - the kernel's own loader reads the
    pool layout from there by design.  The first version of this check policed
    everything outside kernelsrc/src and flagged all five legitimate includers.
    """
    bad = []
    for rel, path in own_sources():
        if not any(m in rel for m in APP_SIDE_MARKERS):
            continue
        if rel.endswith("svcrt_partition.h"):
            continue
        for _, line in code_lines(stored_bytes(rel, path)):
            s = line.strip()
            if s.startswith(b"#") and b"include" in s and b"svcrt_partition.h" in s:
                bad.append(rel)
                break
    if bad:
        fail("no-app-partition-include", ", ".join(sorted(set(bad))[:5]))
    else:
        ok("no-app-partition-include", "App / SDK stay layout-agnostic")


# ---------------------------------------------------------------- check 6
def check_builds(uv4):
    """Both kernel projects must compile with 0 errors (1 warning is the baseline)."""
    if not uv4 or not os.path.exists(uv4):
        fail("build", "UV4 not found: %s" % uv4)
        return
    for proj in ("example/stm32f427/kernel/SVCRTOS_TEST/MDK-ARM/SVCRTOS_TEST.uvprojx",
                 "example/stm32f401/kernel/SVCRTOS_TEST/MDK-ARM/SVCRTOS_TEST.uvprojx"):
        full = os.path.join(ROOT, proj)
        name = "F427" if "f427" in proj else "F401"
        if not os.path.exists(full):
            fail("build:%s" % name, "missing project")
            continue
        # UV4 needs an absolute -o path that actually exists; C:\tmp is the
        # convention in this repo (D:\tmp does not exist and UV4 then silently
        # writes no log at all).
        logdir = "C:/tmp" if os.path.isdir("C:/tmp") else os.environ.get("TEMP", ".")
        log = os.path.join(logdir, "ci_gate_%s.log" % name)
        env = dict(os.environ)
        env.pop("PYTHONHOME", None)
        env.pop("PYTHONPATH", None)
        try:
            r = subprocess.run([uv4, "-r", full, "-o", log], env=env,
                               capture_output=True, encoding="utf-8",
                               errors="replace", timeout=900)
        except subprocess.TimeoutExpired:
            fail("build:%s" % name, "compile timed out")
            continue
        txt = open(log, "r", errors="replace").read() if os.path.exists(log) else ""
        m = re.search(r"(\d+)\s+Error\(s\)", txt)
        errs = int(m.group(1)) if m else -1
        if errs == 0:
            ok("build:%s" % name, "0 Error(s)")
        else:
            fail("build:%s" % name, "exit=%d errors=%s" % (r.returncode, errs))


def main(argv):
    do_build = "--build" in argv
    uv4 = None
    if "--uv4" in argv:
        uv4 = argv[argv.index("--uv4") + 1]
    else:
        for cand in ("D:/Keil_v5/UV4/UV4.exe", "C:/Keil_v5/UV4/UV4.exe"):
            if os.path.exists(cand):
                uv4 = cand
                break

    check_partition_twins()
    check_single_address_source()
    check_encoding_eol()
    check_layout_and_scatter()
    check_no_partition_include_in_app()
    if do_build:
        check_builds(uv4)

    print("\n%d check(s) passed, %d failed" % (len(PASSED), len(FAILURES)))
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
