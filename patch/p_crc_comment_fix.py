#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Fix the two comments that still described the old (skip) semantics."""
import os

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TARGET = os.path.join(REPO, 'kernelsrc', 'src', 'svcrt_loader.c')

OLD1 = """/* 镜像 CRC：头（crc32 与 state 两个字段按 0 参与）+ 重定位表 + 负载。
 * 之所以把 state 排除在外，是因为它必须在写入之后被单独改写（提交动作），
 * 而 CRC 不能因此失效；crc32 字段自身当然也要排除。 */"""
NEW1 = """/* 镜像 CRC：头（crc32 与 state 两个字段按 0 代入）+ 重定位表 + 负载。
 * state 要按 0 代入，是因为它必须在写入之后被单独改写（提交动作），
 * 而 CRC 不能因此失效；crc32 字段自身同理——文件里它装着最终校验值，
 * 不能把它的字节也算进去，否则就成了「用结果算结果」。 */"""

OLD2 = """/* 镜像头在镜像 CRC 里的参与方式：crc32 与 state 两个字段整体跳过
 * （打包时两者都是 0，跳过与按 0 参与等价，已用打包工具逐字节核对）。 */"""
NEW2 = """/* 镜像头在镜像 CRC 里的参与方式：crc32 与 state 两个字段各按四个 0 字节代入。
 * 注意不能把这两段「跳过」：打包工具算校验时这两个字段确实是 0，但零字节
 * 是要参与 CRC 运算的，跳过与代入 0 得到的值不同（实测 APP_DEMO 差
 * 0x27DE38AB vs 0x474E26D7），跳过会让每一次安装都误报 CRC 错。 */"""


def main():
    text = open(TARGET, 'rb').read().decode('utf-8')
    for old, new, what in ((OLD1, NEW1, 'image_crc comment'),
                           (OLD2, NEW2, 'crc_header comment')):
        n = text.count(old)
        if n != 1:
            raise SystemExit('anchor "%s" matched %d times, expected 1' % (what, n))
        text = text.replace(old, new)
    open(TARGET, 'wb').write(text.encode('utf-8'))
    print('[patch] comments corrected')


if __name__ == '__main__':
    main()
