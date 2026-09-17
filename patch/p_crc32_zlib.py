#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Make SVCrtOS's svcrt_crc32() agree with zlib.crc32().

tools/pack_app.py stamps every image with zlib.crc32(), whose operator
semantics are  crc = raw(crc ^ 0xFFFFFFFF) ^ 0xFFFFFFFF  -- i.e. the
standard CRC-32/ISO-HDLC with pre- and post-inversion.  The kernel
implementation did the bare reflected loop starting from the caller's
value with no inversion, which is a different function: for the nominal
APP_DEMO image zlib gives 0x474E26D7 while the kernel gave 0xAE1EDD7F.
So every install ended in SVCRT_LOADER_ERR_CRC (-5).

The fix keeps the streaming/segmented property (feeding the previous
return value back in still works) and only aligns the two inversions.

Byte-level patch: the file is UTF-8 with LF endings. The function body is
delimited by its signature and the first column-0 closing brace, and the
doc line above it is matched by a short unique fragment -- no giant
literal that can drift out of sync with the file.
"""
import os

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TARGET = os.path.join(REPO, 'kernelsrc', 'src', 'svcrt_loader.c')

SIG = b"uint32 svcrt_crc32(const void *data, uint32 len, uint32 crc)\n{"

NEW_BODY = b"""uint32 svcrt_crc32(const void *data, uint32 len, uint32 crc)
{
    const uint8 *p = (const uint8 *)data;
    uint32 c;
    uint32 i;
    uint32 j;

    /* \u524d\u7f6e\u53d6\u53cd\uff08zlib.crc32 \u7684 init ^= 0xFFFFFFFF\uff09\uff1a\u7a7a\u8f93\u5165\u8fd4\u56de 0\u3002 */
    c = crc ^ 0xFFFFFFFFu;

    for(i = 0u; i < len; i++)
    {
        c ^= (uint32)p[i];

        for(j = 0u; j < 8u; j++)
        {
            if((c & 1u) != 0u)
            {
                c = (c >> 1) ^ 0xEDB88320u;
            }
            else
            {
                c >>= 1;
            }
        }
    }

    /* \u540e\u7f6e\u53d6\u53cd\uff08zlib.crc32 \u7684 xorout\uff09\uff1a\u4fdd\u8bc1\u53ef\u4ee5\u7ee7\u7eed\u7d2f\u79ef\u3002 */
    return c ^ 0xFFFFFFFFu;
}"""

OLD_DOC = b" *       crc = svcrt_crc32(seg1, len1, 0); crc = svcrt_crc32(seg2, len2, crc);"
NEW_DOC = (
    b" *       \u8bed\u4e49\u4e0e zlib.crc32() \u9010\u4f4d\u4e00\u81f4\uff08\u524d\u540e\u5747\u53d6\u53cd\uff09\uff0c\u56e0\u6b64\u53ef\u4ee5\u628a\u4e0a\u4e00\u6bb5\u7684\u8fd4\u56de\u503c\n"
    b" *       \u76f4\u63a5\u4f20\u56de\u6765\u7ee7\u7eed\u7d2f\u79ef\uff0c\u7b49\u4ef7\u4e8e zlib.crc32(\u4e24\u6bb5\u62fc\u8d77\u6765)\u3002\n"
    b" *       \u65e7\u5b9e\u73b0\u53ea\u505a\u4e86\u53cd\u5c04\u5faa\u73af\u3001\u6ca1\u6709\u524d\u540e\u53d6\u53cd\uff0c\u7b97\u51fa\u6765\u662f\u53e6\u4e00\u4e2a\u503c\uff1a\u540c\u4e00\u4efd\n"
    b" *       APP_DEMO \u955c\u50cf zlib \u7ed9 0x474E26D7\u3001\u65e7\u5185\u6838\u7ed9 0xAE1EDD7F\uff0c\u5b89\u88c5\u5fc5\u5b9a\u62a5\n"
    b" *       SVCRT_LOADER_ERR_CRC\u3002\n"
    b" *\n"
    b" *       crc = svcrt_crc32(seg1, len1, 0); crc = svcrt_crc32(seg2, len2, crc);"
)


def main():
    data = open(TARGET, 'rb').read()

    if b"c = crc ^ 0xFFFFFFFFu;" in data:
        print('[patch] already applied, nothing to do')
        return

    # 1) the documentation line above the function
    n = data.count(OLD_DOC)
    if n != 1:
        raise SystemExit('doc anchor matched %d times, expected 1' % n)
    data = data.replace(OLD_DOC, NEW_DOC)

    # 2) the function body: signature .. first line-start closing brace
    start = data.find(SIG)
    if start < 0:
        raise SystemExit('signature not found')
    if data.count(SIG) != 1:
        raise SystemExit('signature not unique')

    end = data.find(b"\n}", start)
    if end < 0:
        raise SystemExit('closing brace not found')
    end += 2

    data = data[:start] + NEW_BODY + data[end:]

    open(TARGET, 'wb').write(data)
    print('[patch] svcrt_crc32 aligned with zlib.crc32')


if __name__ == '__main__':
    main()
