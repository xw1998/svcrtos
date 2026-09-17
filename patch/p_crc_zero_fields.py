#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Feed four zero bytes for the crc32/state fields instead of skipping them.

tools/pack_app.py computes the stamped crc32 as

    zlib.crc32(header)  with header[28:32] and header[100:104] set to zero
    zlib.crc32(reloc_table, crc)
    zlib.crc32(payload, crc)

i.e. those two fields *participate as zero*.  The kernel sliced them out
of the stream instead (crc32 .. state, then state+4 .. 256), which is a
different number: appending four 0x00 bytes is not the same as appending
nothing.  Measured on APP_DEMO: the kernel produced 0x27DE38AB where the
header says 0x474E26D7, so every transfer ended in SVCRT_LOADER_ERR_CRC.

The file is UTF-8 with LF endings.
"""
import os

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TARGET = os.path.join(REPO, 'kernelsrc', 'src', 'svcrt_loader.c')

ZERO_DECL = """/* 计算镜像 CRC 时代入 crc32 / state 两个字段的占位：打包工具算这两个字段时
 * 它们都是 0，内核必须**真的喂四个 0 字节**，而不是把这两段从流里跳过去——
 * 对 CRC 来说「追加 4 个 0」和「什么都不追加」是两个不同的值。 */
static const uint8 svcrt_loader_zero4[4] = { 0u, 0u, 0u, 0u };

"""

OLD_IMAGE = """static uint32 svcrt_loader_image_crc(const uint8 *image, const svcrt_app_header_t *p_hdr)
{
    uint32 total = SVCRT_APP_TOTAL_LEN(p_hdr);
    uint32 crc;

    crc = svcrt_crc32(image, SVCRT_APP_OFF_CRC32, 0u);
    crc = svcrt_crc32(image + (SVCRT_APP_OFF_CRC32 + 4u),
                      SVCRT_APP_OFF_STATE - (SVCRT_APP_OFF_CRC32 + 4u), crc);
    crc = svcrt_crc32(image + (SVCRT_APP_OFF_STATE + 4u),
                      SVCRT_APP_HEADER_SIZE - (SVCRT_APP_OFF_STATE + 4u), crc);
    crc = svcrt_crc32(image + SVCRT_APP_HEADER_SIZE,
                      total - SVCRT_APP_HEADER_SIZE, crc);

    return crc;
}"""

NEW_IMAGE = """static uint32 svcrt_loader_image_crc(const uint8 *image, const svcrt_app_header_t *p_hdr)
{
    uint32 total = SVCRT_APP_TOTAL_LEN(p_hdr);
    uint32 crc;

    crc = svcrt_crc32(image, SVCRT_APP_OFF_CRC32, 0u);
    crc = svcrt_crc32(svcrt_loader_zero4, 4u, crc);
    crc = svcrt_crc32(image + (SVCRT_APP_OFF_CRC32 + 4u),
                      SVCRT_APP_OFF_STATE - (SVCRT_APP_OFF_CRC32 + 4u), crc);
    crc = svcrt_crc32(svcrt_loader_zero4, 4u, crc);
    crc = svcrt_crc32(image + (SVCRT_APP_OFF_STATE + 4u),
                      SVCRT_APP_HEADER_SIZE - (SVCRT_APP_OFF_STATE + 4u), crc);
    crc = svcrt_crc32(image + SVCRT_APP_HEADER_SIZE,
                      total - SVCRT_APP_HEADER_SIZE, crc);

    return crc;
}"""

OLD_HEADER = """static uint32 svcrt_loader_crc_header(const svcrt_app_header_t *p_hdr)
{
    const uint8 *p = (const uint8 *)p_hdr;
    uint32 crc;

    crc = svcrt_crc32(p, SVCRT_APP_OFF_CRC32, 0u);
    crc = svcrt_crc32(p + (SVCRT_APP_OFF_CRC32 + 4u),
                      SVCRT_APP_OFF_STATE - (SVCRT_APP_OFF_CRC32 + 4u), crc);
    crc = svcrt_crc32(p + (SVCRT_APP_OFF_STATE + 4u),
                      SVCRT_APP_HEADER_SIZE - (SVCRT_APP_OFF_STATE + 4u), crc);

    return crc;
}"""

NEW_HEADER = """static uint32 svcrt_loader_crc_header(const svcrt_app_header_t *p_hdr)
{
    const uint8 *p = (const uint8 *)p_hdr;
    uint32 crc;

    crc = svcrt_crc32(p, SVCRT_APP_OFF_CRC32, 0u);
    crc = svcrt_crc32(svcrt_loader_zero4, 4u, crc);
    crc = svcrt_crc32(p + (SVCRT_APP_OFF_CRC32 + 4u),
                      SVCRT_APP_OFF_STATE - (SVCRT_APP_OFF_CRC32 + 4u), crc);
    crc = svcrt_crc32(svcrt_loader_zero4, 4u, crc);
    crc = svcrt_crc32(p + (SVCRT_APP_OFF_STATE + 4u),
                      SVCRT_APP_HEADER_SIZE - (SVCRT_APP_OFF_STATE + 4u), crc);

    return crc;
}"""

# anchor for where to drop the zero4 declaration: right before the image_crc
# doc comment, which is the first user of it.
ANCHOR = "/* 镜像 CRC：头（crc32 与 state 两个字段按 0 参与）+ 重定位表 + 负载。"


def main():
    text = open(TARGET, 'rb').read().decode('utf-8')

    if 'svcrt_loader_zero4' in text:
        print('[patch] already applied, nothing to do')
        return

    for old, new, what in ((OLD_IMAGE, NEW_IMAGE, 'image_crc'),
                           (OLD_HEADER, NEW_HEADER, 'crc_header')):
        n = text.count(old)
        if n != 1:
            raise SystemExit('anchor "%s" matched %d times, expected 1' % (what, n))
        text = text.replace(old, new)

    n = text.count(ANCHOR)
    if n != 1:
        raise SystemExit('decl anchor matched %d times, expected 1' % n)
    text = text.replace(ANCHOR, ZERO_DECL + ANCHOR)

    open(TARGET, 'wb').write(text.encode('utf-8'))
    print('[patch] crc32/state now enter the CRC as four zero bytes')


if __name__ == '__main__':
    main()
