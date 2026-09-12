# -*- coding: utf-8 -*-
"""
Install the English-comment rewrites of the 5 Driver SDK example files and
verify that only comments/whitespace changed (code must be identical).

Files were originally GBK and their Chinese comments had already been destroyed
(Chinese characters replaced by '?'), so the rewrite also repairs that damage.
The new files are pure ASCII: byte-identical under GBK and UTF-8, so later
patches cannot corrupt them.
"""
import os, re, sys, shutil

REPO = r"D:/工作/git_project/svcrtos_new"
SRC = r"C:/Users/14905/OneDrive/Documents/lingxi-claw/20260911-13-47-19-445/patch"
BACKUP = r"C:/Users/14905/OneDrive/Documents/lingxi-claw/20260911-13-47-19-445/backup_fix15_sdk_examples"

EXAMPLES = 'kernelsrc/sdk/driver_sdk/examples'
FILES = ['example_i2c_drv.c', 'example_adc_drv.c', 'example_gpio_drv.c',
         'example_spi_drv.c', 'example_uart_drv.c']


def detect_enc(b):
    try:
        b.decode('utf-8')
        return 'utf-8'
    except UnicodeDecodeError:
        return 'gbk'


def strip_comments(text):
    """Remove /* */ and // comments, collapse whitespace; keep code only."""
    text = re.sub(r'/\*.*?\*/', ' ', text, flags=re.S)
    text = re.sub(r'//[^\n]*', ' ', text)
    return re.sub(r'\s+', ' ', text).strip()


def norm(s):
    return re.sub(r'\s+', ' ', s).strip()


def main():
    os.makedirs(BACKUP, exist_ok=True)
    ok = True
    for name in FILES:
        src = os.path.join(SRC, 'new_' + name)
        dst = os.path.join(REPO, EXAMPLES, name)
        old_raw = open(dst, 'rb').read()
        shutil.copy2(dst, os.path.join(BACKUP, name))

        new_text = open(src, 'r', encoding='utf-8').read()
        new_text = new_text.replace('\r\n', '\n').replace('\n', '\r\n')

        # --- verification: stripped code must be identical -----------------
        old_text = old_raw.decode(detect_enc(old_raw))
        if strip_comments(old_text.replace('\r\n', '\n')) != strip_comments(new_text.replace('\r\n', '\n')):
            print('!! CODE DIFF in %s' % name)
            ok = False
            continue

        with open(dst, 'wb') as f:
            f.write(new_text.encode('gbk'))   # pure ASCII -> identical to utf-8
        print('OK  %s (code identical, comments -> English, CRLF, ascii)' % name)
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
