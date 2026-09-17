#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Cleanup: the previous patch embedded literal \\uXXXX escapes (they are not
escape sequences inside a bytes literal). Rewrite those comment blocks with
real UTF-8 text."""
import os
import re

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TARGET = os.path.join(REPO, 'kernelsrc', 'src', 'svcrt_loader.c')

DOC_OLD = """ * @note 与打包工具 tools/pack_app.py 使用同一算法，支持分段累积：
 *       \\u8bed\\u4e49\\u4e0e zlib.crc32() \\u9010\\u4f4d\\u4e00\\u81f4\\uff08\\u524d\\u540e\\u5747\\u53d6\\u53cd\\uff09\\uff0c\\u56e0\\u6b64\\u53ef\\u4ee5\\u628a\\u4e0a\\u4e00\\u6bb5\\u7684\\u8fd4\\u56de\\u503c
 *       \\u76f4\\u63a5\\u4f20\\u56de\\u6765\\u7ee7\\u7eed\\u7d2f\\u79ef\\uff0c\\u7b49\\u4ef7\\u4e8e zlib.crc32(\\u4e24\\u6bb5\\u62fc\\u8d77\\u6765)\\u3002
 *       \\u65e7\\u5b9e\\u73b0\\u53ea\\u505a\\u4e86\\u53cd\\u5c04\\u5faa\\u73af\\u3001\\u6ca1\\u6709\\u524d\\u540e\\u53d6\\u53cd\\uff0c\\u7b97\\u51fa\\u6765\\u662f\\u53e6\\u4e00\\u4e2a\\u503c\\uff1a\\u540c\\u4e00\\u4efd
 *       APP_DEMO \\u955c\\u50cf zlib \\u7ed9 0x474E26D7\\u3001\\u65e7\\u5185\\u6838\\u7ed9 0xAE1EDD7F\\uff0c\\u5b89\\u88c5\\u5fc5\\u5b9a\\u62a5
 *       SVCRT_LOADER_ERR_CRC\\u3002
 *
 *       crc = svcrt_crc32(seg1, len1, 0); crc = svcrt_crc32(seg2, len2, crc);"""

DOC_NEW = """ * @note 语义与 zlib.crc32() 逐位一致（输入/输出均取反），因此可以把上一段的
 *       返回值直接传回来继续累积：
 *           crc = svcrt_crc32(seg1, len1, 0);
 *           crc = svcrt_crc32(seg2, len2, crc);
 *       等价于 zlib.crc32(两段拼起来)。打包工具 tools/pack_app.py 用的就是
 *       zlib，两边必须是同一个函数。
 *
 *       旧实现只做了反射循环、没有前后取反，算出来是另一个值：同一份
 *       APP_DEMO 镜像 zlib 给 0x474E26D7、旧内核给 0xAE1EDD7F，
 *       安装必定报 SVCRT_LOADER_ERR_CRC。"""

BODY_OLD_1 = "    /* \\u524d\\u7f6e\\u53d6\\u53cd\\uff08zlib.crc32 \\u7684 init ^= 0xFFFFFFFF\\uff09\\uff1a\\u7a7a\\u8f93\\u5165\\u8fd4\\u56de 0\\u3002 */"
BODY_NEW_1 = "    /* 前置取反（zlib.crc32 的 init ^= 0xFFFFFFFF）：空输入返回 0。 */"

BODY_OLD_2 = "    /* \\u540e\\u7f6e\\u53d6\\u53cd\\uff08zlib.crc32 \\u7684 xorout\\uff09\\uff1a\\u4fdd\\u8bc1\\u53ef\\u4ee5\\u7ee7\\u7eed\\u7d2f\\u79ef\\u3002 */"
BODY_NEW_2 = "    /* 后置取反（zlib.crc32 的 xorout）：保证可以继续累积。 */"


def main():
    text = open(TARGET, 'rb').read().decode('utf-8')
    for old, new in ((DOC_OLD, DOC_NEW), (BODY_OLD_1, BODY_NEW_1),
                     (BODY_OLD_2, BODY_NEW_2)):
        n = text.count(old)
        if n != 1:
            raise SystemExit('anchor matched %d times, expected 1:\n%s' % (n, old[:60]))
        text = text.replace(old, new)

    if re.search(r'\\u[0-9a-fA-F]{4}', text):
        raise SystemExit('literal \\uXXXX escapes still present')

    open(TARGET, 'wb').write(text.encode('utf-8'))
    print('[patch] comment blocks rewritten as UTF-8')


if __name__ == '__main__':
    main()
