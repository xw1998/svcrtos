# -*- coding: utf-8 -*-
"""
Repair the damaged comments in kernelsrc/port/arm/cortex-m4/svcrt_context.S
and rewrite them in English (ASCII). The Chinese text in that file had already
been destroyed (every non-ASCII byte became '?'), so the comments carry no
information any more; this restores them in English, matching the M3 port.

Only comment lines are touched - every changed line is a line that starts with
';' (or a leading-whitespace ';' inline comment) and contains '?'.
"""
import os, sys, shutil

REPO = r"D:/工作/git_project/svcrtos_new"
REL = 'kernelsrc/port/arm/cortex-m4/svcrt_context.S'
BACKUP = r"C:/Users/14905/OneDrive/Documents/lingxi-claw/20260911-13-47-19-445/backup_fix15_sdk_examples"

RULES = [
    ('PendSV_Handler - Cortex-M4',
     '; PendSV_Handler - Cortex-M4 task context switch.'),
    ('SVCRT_USE_FPU=1',
     '; When SVCRT_USE_FPU=1 a task may own an FPU extended frame, which also'),
    ('EXC_RETURN(LR)',
     '; holds S16-S31; EXC_RETURN (LR) says which frame the interrupted code used.'),
    ('EXC_RETURN bit4=0',
     '    ; extended frame in use (EXC_RETURN bit4=0): save S16-S31 as well'),
    ('SVC_Handler -',
     '; SVC_Handler - system service call entry.'),
]


def detect_enc(b):
    try:
        b.decode('utf-8')
        return 'utf-8'
    except UnicodeDecodeError:
        return 'gbk'


def main():
    full = os.path.join(REPO, REL)
    raw = open(full, 'rb').read()
    enc = detect_enc(raw)
    text = raw.decode(enc)
    eol = '\r\n' if '\r\n' in text else '\n'
    lines = text.split(eol)

    os.makedirs(BACKUP, exist_ok=True)
    shutil.copy2(full, os.path.join(BACKUP, 'svcrt_context_m4.S'))

    changed = 0
    for i, l in enumerate(lines):
        if '?' not in l:
            continue
        stripped = l.strip()
        if not stripped.startswith(';'):
            print('!! non-comment line with "?" kept as-is: %r' % l)
            continue
        for key, repl in RULES:
            if key in l:
                lines[i] = repl
                changed += 1
                break
        else:
            print('!! no rule for line %d: %r' % (i + 1, l))

    out = eol.join(lines)
    open(full, 'wb').write(out.encode(enc))
    print('OK  %s: %d comment lines rewritten to English' % (REL, changed))

    left = [l for l in out.split(eol) if '?' in l]
    print('remaining lines containing "?":', len(left))
    for l in left:
        print('   ', l)
    return 0


if __name__ == '__main__':
    sys.exit(main())
