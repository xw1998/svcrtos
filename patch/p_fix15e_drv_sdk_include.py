# -*- coding: utf-8 -*-
"""
Add "#include \"svcrt_svc_call.h\"" to kernelsrc/sdk/driver_sdk/svcrt_driver_sdk.h
so that SVCRT_SVC_DECL_* is defined before it is used (the driver SDK header is
included by every driver source, including svcrt_driver_bridge.c).
"""
import os, sys, shutil

REPO = r"D:/工作/git_project/svcrtos_new"
REL = 'kernelsrc/sdk/driver_sdk/svcrt_driver_sdk.h'
BACKUP = r"C:/Users/14905/OneDrive/Documents/lingxi-claw/20260911-13-47-19-445/backup_fix15_svc"


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

    if any('svcrt_svc_call.h' in l for l in lines):
        print('already present')
        return 0

    for i, l in enumerate(lines):
        if l.startswith('#include'):
            lines.insert(i + 1, '#include "svcrt_svc_call.h"')
            break
    else:
        print('!! no #include anchor')
        return 1

    open(full, 'wb').write(eol.join(lines).encode(enc))
    print('OK  %s: include added (enc=%s)' % (REL, enc))
    return 0


if __name__ == '__main__':
    sys.exit(main())
