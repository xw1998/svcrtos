# -*- coding: utf-8 -*-
"""
AC5 -> AC6 readiness: replace the armcc-only "__svc(n)" declarations in the
App / Driver SDK with the toolchain-neutral SVCRT_SVC_DECL_* macros from
kernelsrc/include/svcrt_svc_call.h.

Behaviour:
  * compiled with ARM Compiler 5 the macros expand to the same __svc(n)
    declarations as before - the existing build is bit-for-bit unchanged in
    behaviour;
  * compiled with ARM Compiler 6 (or GCC/Clang) they expand to static inline
    wrappers that emit "svc #n" with the arguments pinned to r0/r1/r2.

Only lines that are pure __svc declarations are touched; nothing else changes.
"""
import os, re, sys, shutil

REPO = r"D:/工作/git_project/svcrtos_new"
BACKUP = r"C:/Users/14905/OneDrive/Documents/lingxi-claw/20260911-13-47-19-445/backup_fix15_svc"

TARGETS = [
    ('kernelsrc/sdk/app_sdk/svcrt_oslib.c', 'svcrt.h'),
    ('kernelsrc/sdk/driver_sdk/svcrt_drv_oslib.c', 'svcrt_driver_sdk.h'),
    ('kernelsrc/sdk/driver_sdk/svcrt_driver_sdk.h', None),
]

DECL_RE = re.compile(
    r'^(?P<indent>\s*)(?P<ret>int32|uint32|void)\s+__svc\((?P<num>[^)]+)\)\s+'
    r'(?P<name>\w+)\((?P<args>[^)]*)\);\s*$')


def detect_enc(b):
    try:
        b.decode('utf-8')
        return 'utf-8'
    except UnicodeDecodeError:
        return 'gbk'


def convert(line):
    m = DECL_RE.match(line)
    if not m:
        return None
    ret = m.group('ret')
    num = m.group('num').strip()
    name = m.group('name')
    args = [a.strip() for a in m.group('args').split(',') if a.strip()]

    types = []
    for a in args:
        # handles both plain ("uint32 fn") and pointer ("uint32 *p") declarators
        am = re.match(r'^(?P<base>[\w\s]+?)(?P<stars>\**)\s*(?P<name>\w+)$', a)
        if not am:
            return None
        base = am.group('base').strip()
        stars = am.group('stars')
        types.append(base + (' ' + stars if stars else ''))

    if ret == 'void':
        if len(types) == 1:
            macro = 'SVCRT_SVC_DECL_V1'
            call = '%s(%s, %s, %s);' % (macro, num, name, types[0])
        elif len(types) == 2:
            macro = 'SVCRT_SVC_DECL_V2'
            call = '%s(%s, %s, %s, %s);' % (macro, num, name, types[0], types[1])
        else:
            return None
    else:
        if not 1 <= len(types) <= 3:
            return None
        macro = 'SVCRT_SVC_DECL_%d' % len(types)
        call = '%s(%s, %s, %s);' % (macro, ret, num, name)
        if types:
            call = '%s(%s, %s, %s, %s);' % (macro, ret, num, name, ', '.join(types))

    return m.group('indent') + call


def main():
    total = 0
    for rel, inc_after in TARGETS:
        full = os.path.join(REPO, rel)
        # start from the pristine copy so the script is re-runnable
        bak = os.path.join(BACKUP, os.path.basename(rel))
        if os.path.exists(bak):
            shutil.copy2(bak, full)
        raw = open(full, 'rb').read()
        enc = detect_enc(raw)
        text = raw.decode(enc)
        eol = '\r\n' if '\r\n' in text else '\n'
        lines = text.split(eol)

        # 1) add the include (after the first #include, once)
        if inc_after is not None:
            has = any('svcrt_svc_call.h' in l for l in lines)
            if not has:
                for i, l in enumerate(lines):
                    if l.startswith('#include'):
                        lines.insert(i + 1, '#include "svcrt_svc_call.h"')
                        break
                else:
                    print('!! no #include in %s' % rel)
                    return 1

        # 2) convert declarations
        n = 0
        for i, l in enumerate(lines):
            new = convert(l)
            if new is not None:
                lines[i] = new
                n += 1
            elif '__svc' in l:
                print('!! unhandled __svc line in %s: %r' % (rel, l))
                return 1
        total += n
        open(full, 'wb').write(eol.join(lines).encode(enc))
        print('OK  %s: %d declarations converted (enc=%s)' % (rel, n, enc))

    print('total converted:', total)
    return 0


if __name__ == '__main__':
    sys.exit(main())
