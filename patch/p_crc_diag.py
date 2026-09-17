#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Add diagnostics to the two places that return SVCRT_LOADER_ERR_CRC.

An install that ends in -5 could fail at two very different points:
  a) the transferred bytes do not match the crc32 stamped in the header
     (a transmission / packing problem), or
  b) the bytes read back from flash do not match what we programmed
     (a flash problem).
Both currently return the same code, which makes the failure unfalsifiable
from the console. Print the two numbers and a bit of context.

The file is UTF-8 with LF endings.
"""
import os

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TARGET = os.path.join(REPO, 'kernelsrc', 'src', 'svcrt_loader.c')

INC_OLD = '#include "svcrt_partition.h"    /* 内核专属：读取池布局 / 任务运行参数 */'
INC_NEW = INC_OLD + '\n#include "svcrt_log.h"'

# (a) transfer CRC
OLD_A = """    if(crc_recv != p_hdr->crc32)
    {
        svcrt_loader_nak(dev);
        return SVCRT_LOADER_ERR_CRC;
    }"""
NEW_A = """    if(crc_recv != p_hdr->crc32)
    {
        SVCRT_LOGE("LOADER", "transfer crc: recv 0x%08X hdr 0x%08X (payload %u, reloc %u)",
                   (unsigned)crc_recv, (unsigned)p_hdr->crc32,
                   (unsigned)p_hdr->image_size, (unsigned)p_hdr->reloc_count);
        svcrt_loader_nak(dev);
        return SVCRT_LOADER_ERR_CRC;
    }"""

# (b) flash read-back, in svcrt_loader_load_dev_hdr()
OLD_B = """    if(svcrt_loader_image_crc((const uint8 *)base, &hdr) != crc_written)
    {
        svcrt_ptable_free((uint32)slot);
        return SVCRT_LOADER_ERR_CRC;
    }

    /* 提交点：单字写入。写下去之前掉电算「没装成」，写下去之后就算装成了；"""
NEW_B = """    {
        uint32 rb = svcrt_loader_image_crc((const uint8 *)base, &hdr);

        if(rb != crc_written)
        {
            SVCRT_LOGE("LOADER", "readback crc: flash 0x%08X written 0x%08X (base 0x%08X)",
                       (unsigned)rb, (unsigned)crc_written, (unsigned)base);
            svcrt_ptable_free((uint32)slot);
            return SVCRT_LOADER_ERR_CRC;
        }
    }

    /* 提交点：单字写入。写下去之前掉电算「没装成」，写下去之后就算装成了；"""


def main():
    text = open(TARGET, 'rb').read().decode('utf-8')
    for old, new, what in ((INC_OLD, INC_NEW, 'include'),
                           (OLD_A, NEW_A, 'transfer crc site'),
                           (OLD_B, NEW_B, 'readback crc site')):
        n = text.count(old)
        if n != 1:
            raise SystemExit('anchor "%s" matched %d times, expected 1' % (what, n))
        text = text.replace(old, new)
    open(TARGET, 'wb').write(text.encode('utf-8'))
    print('[patch] crc failure diagnostics added')


if __name__ == '__main__':
    main()
