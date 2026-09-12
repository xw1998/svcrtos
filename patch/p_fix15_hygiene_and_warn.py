# -*- coding: utf-8 -*-
"""
SVCrtOS patch round 15 - part A
board/stm32f427/svcrt_board_config.h

* add "#undef APP_ALLOW_RAW_IMAGE" before the board override so the
  -Wmacro-redefined warning goes away and the override matches the style of
  every other macro in this file;
* rewrite the adjacent comment block in English (ASCII), which is
  byte-identical under GBK and UTF-8 and therefore safe to patch later.
"""
import os, sys

REPO = r"D:/工作/git_project/svcrtos_new"
TARGET = 'board/stm32f427/svcrt_board_config.h'


def detect_enc(b):
    try:
        b.decode('utf-8')
        return 'utf-8'
    except UnicodeDecodeError:
        return 'gbk'


def main():
    full = os.path.join(REPO, TARGET)
    with open(full, 'rb') as f:
        raw = f.read()
    enc = detect_enc(raw)
    text = raw.decode(enc)
    eol = '\r\n' if '\r\n' in text else '\n'
    lines = text.split(eol)

    # locate the board override of APP_ALLOW_RAW_IMAGE
    idx = None
    for i, l in enumerate(lines):
        if l.strip().startswith('#define APP_ALLOW_RAW_IMAGE'):
            idx = i
            break
    if idx is None:
        print('!! APP_ALLOW_RAW_IMAGE define not found')
        return 1

    # walk backwards over the comment block that documents it
    start = idx
    while start > 0:
        s = lines[start - 1].strip()
        if s.startswith('*') or s.startswith('/*'):
            start -= 1
        else:
            break

    new_block = [
        "/* Development bypass: let the kernel treat a raw image burned directly",
        " * at the slot base as runnable, so the fixed-address flash + MDK",
        " * breakpoint workflow keeps working. Release firmware must reset it to 0",
        " * and accept only CRC-checked .svcapp packages. */",
        "#undef  APP_ALLOW_RAW_IMAGE",
        "#define APP_ALLOW_RAW_IMAGE       1",
    ]

    lines[start:idx + 1] = new_block
    out = eol.join(lines)
    with open(full, 'wb') as f:
        f.write(out.encode(enc))
    print('OK  %s (enc=%s eol=%s, replaced lines %d..%d)' % (TARGET, enc, repr(eol), start + 1, idx + 1))
    return 0


if __name__ == '__main__':
    sys.exit(main())
