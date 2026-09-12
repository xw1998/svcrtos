# -*- coding: utf-8 -*-
"""
P1-2 修复：外部镜像加载缺校验

问题：
  1) total = SVCRT_APP_HEADER_SIZE + p_hdr->image_size 会回绕：
     image_size = 0xFFFFFFFF 时 total = 255，后面的“total > 槽位容量”检查被绕过，
     随后的擦除/写入/CRC 都按错误的长度进行；
  2) entry_offset 完全未校验：镜像可以把入口指到分区内任意地址
     （甚至落到镜像头/数据区），配合 SVC 安装通道就是一个可控的取指跳转；
  3) 上电扫描 svcrt_loader_identify() 同样没有这两项检查。

修复：
  - svcrt_loader_check_header() 增加：image_size 不允许导致 total 回绕；
    entry_offset 必须落在负载范围内（< image_size）；
  - svcrt_loader_identify() 同步这两项检查；
  - 新增错误码 SVCRT_LOADER_ERR_ENTRY(-11)。
"""
import os
import shutil

ROOT = r'D:\工作\git_project\svcrtos_new'
BK = r'C:\Users\14905\OneDrive\Documents\lingxi-claw\20260911-13-47-19-445\backup_p0_fault'

FILES = ['kernelsrc/include/svcrt_loader.h', 'kernelsrc/src/svcrt_loader.c']


def do_backup():
    os.makedirs(BK, exist_ok=True)
    for rel in FILES:
        shutil.copy2(os.path.join(ROOT, rel.replace('/', os.sep)),
                     os.path.join(BK, rel.replace('/', '__')))


class Doc(object):
    def __init__(self, rel):
        self.rel = rel
        self.p = os.path.join(ROOT, rel.replace('/', os.sep))
        self.raw = open(self.p, 'rb').read()
        self.nl = b'\r\n' if b'\r\n' in self.raw else b'\n'

    def t(self, s):
        s = s.replace('\n', '\r\n') if self.nl == b'\r\n' else s
        return s.encode('utf-8')

    def sub(self, old, new, label, count=1):
        ob, nb = self.t(old), self.t(new)
        n = self.raw.count(ob)
        assert n == count, '[%s] %s: 期望 %d 次，实际 %d 次' % (self.rel, label, count, n)
        self.raw = self.raw.replace(ob, nb)
        print('  ok:', label)

    def save(self):
        open(self.p, 'wb').write(self.raw)


H_OLD = """#define SVCRT_LOADER_ERR_ADDR     (-10)  /* 镜像头 load_addr 与目标分区基址不一致 */"""

H_NEW = """#define SVCRT_LOADER_ERR_ADDR     (-10)  /* 镜像头 load_addr 与目标分区基址不一致 */
#define SVCRT_LOADER_ERR_ENTRY    (-11)  /* 镜像头 entry_offset 越界（入口不在负载范围内） */"""

C_OLD = """    if(p_hdr->image_size == 0u)
    {
        return SVCRT_LOADER_ERR_SIZE;
    }

    return 0;
}"""

C_NEW = """    if(p_hdr->image_size == 0u)
    {
        return SVCRT_LOADER_ERR_SIZE;
    }

    /* image_size 不能让 total = 头长 + image_size 回绕：
     * 一旦回绕（如 image_size = 0xFFFFFFFF 时 total = 255），调用方的
     * “total > 槽位容量”检查会被绕过，随后的擦除/写入/CRC 全部按错误长度进行。 */
    if(p_hdr->image_size > (0xFFFFFFFFu - SVCRT_APP_HEADER_SIZE))
    {
        return SVCRT_LOADER_ERR_SIZE;
    }

    /* entry_offset 是入口相对「负载起始」的偏移，必须落在负载范围内，
     * 否则镜像能把入口指到分区内任意地址（含镜像头或数据区）。 */
    if(p_hdr->entry_offset >= p_hdr->image_size)
    {
        return SVCRT_LOADER_ERR_ENTRY;
    }

    return 0;
}"""

ID_OLD = """        if((p_hdr->hw_compat_id != SVCRT_HW_COMPAT_ID) ||
           (p_hdr->image_size == 0u) || (p_hdr->type != expect_type))
        {
            return -1;
        }

        total = SVCRT_APP_HEADER_SIZE + p_hdr->image_size;
        if(total > region_size)
        {
            return -1;
        }"""

ID_NEW = """        if((p_hdr->hw_compat_id != SVCRT_HW_COMPAT_ID) ||
           (p_hdr->image_size == 0u) || (p_hdr->type != expect_type))
        {
            return -1;
        }

        /* 与 svcrt_loader_check_header 同口径：禁止 total 回绕，且入口必须落在负载内 */
        if(p_hdr->image_size > (0xFFFFFFFFu - SVCRT_APP_HEADER_SIZE))
        {
            return -1;
        }

        if(p_hdr->entry_offset >= p_hdr->image_size)
        {
            return -1;
        }

        total = SVCRT_APP_HEADER_SIZE + p_hdr->image_size;
        if(total > region_size)
        {
            return -1;
        }"""


if __name__ == '__main__':
    do_backup()
    print('--- svcrt_loader.h ---')
    d = Doc('kernelsrc/include/svcrt_loader.h')
    d.sub(H_OLD, H_NEW, '新增 ENTRY 错误码')
    d.save()

    print('--- svcrt_loader.c ---')
    d = Doc('kernelsrc/src/svcrt_loader.c')
    d.sub(C_OLD, C_NEW, 'check_header 补回绕与入口校验')
    d.sub(ID_OLD, ID_NEW, 'identify 同步校验')
    d.save()
    print('OK')
