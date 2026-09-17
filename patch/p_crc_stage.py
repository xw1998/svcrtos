#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Record the rolling transfer CRC after each stage so a mismatch can be
attributed to one of: header / relocation table / payload.

The file is UTF-8 with LF endings.
"""
import os

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TARGET = os.path.join(REPO, 'kernelsrc', 'src', 'svcrt_loader.c')

A1_OLD = """    uint32 crc_recv;            /* 标称形态（收到的原始字节） */
    uint32 crc_written;         /* 落盘形态（打过补丁的字节） */

    crc_recv    = svcrt_loader_crc_header(p_hdr);
    crc_written = crc_recv;"""
A1_NEW = """    uint32 crc_recv;            /* 标称形态（收到的原始字节） */
    uint32 crc_written;         /* 落盘形态（打过补丁的字节） */
    uint32 crc_hdr;             /* 只算完镜像头时的值（诊断用） */
    uint32 crc_rel;             /* 只算完重定位表时的值（诊断用） */

    crc_recv    = svcrt_loader_crc_header(p_hdr);
    crc_written = crc_recv;
    crc_hdr     = crc_recv;
    crc_rel     = crc_recv;"""

A2_OLD = """    /* 表体到这一刻才落盘，也才能被寻址：在这里校验它。"""
A2_NEW = """    crc_rel = crc_recv;

    /* 表体到这一刻才落盘，也才能被寻址：在这里校验它。"""

A3_OLD = """        SVCRT_LOGE("LOADER", "transfer crc: recv 0x%08X hdr 0x%08X (payload %u, reloc %u)",
                   (unsigned)crc_recv, (unsigned)p_hdr->crc32,
                   (unsigned)p_hdr->image_size, (unsigned)p_hdr->reloc_count);"""
A3_NEW = """        SVCRT_LOGE("LOADER", "transfer crc: hdr 0x%08X rel 0x%08X end 0x%08X want 0x%08X",
                   (unsigned)crc_hdr, (unsigned)crc_rel,
                   (unsigned)crc_recv, (unsigned)p_hdr->crc32);"""


def main():
    text = open(TARGET, 'rb').read().decode('utf-8')
    for old, new, what in ((A1_OLD, A1_NEW, 'stage vars'),
                           (A2_OLD, A2_NEW, 'reloc stage'),
                           (A3_OLD, A3_NEW, 'diag message')):
        n = text.count(old)
        if n != 1:
            raise SystemExit('anchor "%s" matched %d times, expected 1' % (what, n))
        text = text.replace(old, new)
    open(TARGET, 'wb').write(text.encode('utf-8'))
    print('[patch] staged transfer crc diagnostics added')


if __name__ == '__main__':
    main()
