# -*- coding: utf-8 -*-
"""
修复 A1/A2：SVC 校验只覆盖参数块本身，内层指针仍未校验

A1 DEV_IO：
  case 1 的设备名 p[1]、case 2/3 的读写缓冲区 p[2]（长度 p[3]）都没有窗口校验。
  内核按这些指针读写：dev_read 会把外设收到的数据写进调用者给的任意地址，
  dev_write 会把任意地址的内容发往外设 —— 比参数块更直接的任意读写通道。

A2 DRV_MGR：
  case 1 注册驱动只校验了 16 字节参数块，p[2] 指向的 svcrt_dev_drv_t 里
  5 个回调函数指针完全没校验 —— 任意任务都能注册指向内核 Flash 的回调，
  之后设备操作时内核跳到内核代码执行。

同时统一补上各处 create 接口 / open / unregister 的 name 指针校验
（任意地址读到 NUL 的信息泄露）。

实现方式：对每个目标行做“整行捕获 → 前面插入校验 → 原样重放该行”，
避免手写括号层级出错。
"""
import os
import re
import shutil

ROOT = r'D:\工作\git_project\svcrtos_new'
BK = r'C:\Users\14905\OneDrive\Documents\lingxi-claw\20260911-13-47-19-445\backup_fix3'

HELPERS_TAIL = """    return svcrt_kernel_ptr_ram_ok(p, (uint32)len_words * 4u);
}"""

HELPERS = HELPERS_TAIL + """

/* 名字类参数（设备名/信号量名/消息队列名…）：内核最多读 8 字节（含 NUL），
 * 字符串常量通常在用户固件区，因此按“RAM 窗口 + 用户固件窗口”判定。 */
static uint8 svcrt_kernel_user_name_ok(const void *p)
{
    return svcrt_kernel_ptr_flash_ok(p, 8u);
}

/* 设备读写缓冲区：长度必须为正，且整块落在用户可见 RAM 内。
 * 不校验时调用者能让内核向任意地址写入（dev_read）、
 * 或把任意地址的内容读出来送给外设（dev_write）。 */
static uint8 svcrt_kernel_dev_buf_ok(const void *p, int32 len)
{
    if(len <= 0)
    {
        return 0u;
    }
    return svcrt_kernel_ptr_ram_ok(p, (uint32)len);
}

/* 驱动回调表：表本身位于用户可见 RAM，5 个回调指针必须落在用户代码区。
 * （这挡不住“用自己的代码提权”，但能挡住内核跳到内核 Flash 执行。） */
static uint8 svcrt_kernel_dev_drv_ok(const svcrt_dev_drv_t *drv)
{
    if(drv == 0)
    {
        return 0u;
    }
    if(svcrt_kernel_ptr_ram_ok(drv, (uint32)sizeof(svcrt_dev_drv_t)) == 0u)
    {
        return 0u;
    }
    if((drv->drv_open != 0) &&
       (svcrt_kernel_ptr_flash_ok((const void *)drv->drv_open, 2u) == 0u))
    {
        return 0u;
    }
    if((drv->drv_close != 0) &&
       (svcrt_kernel_ptr_flash_ok((const void *)drv->drv_close, 2u) == 0u))
    {
        return 0u;
    }
    if((drv->drv_read != 0) &&
       (svcrt_kernel_ptr_flash_ok((const void *)drv->drv_read, 2u) == 0u))
    {
        return 0u;
    }
    if((drv->drv_write != 0) &&
       (svcrt_kernel_ptr_flash_ok((const void *)drv->drv_write, 2u) == 0u))
    {
        return 0u;
    }
    if((drv->drv_ctrl != 0) &&
       (svcrt_kernel_ptr_flash_ok((const void *)drv->drv_ctrl, 2u) == 0u))
    {
        return 0u;
    }
    return 1u;
}"""


class Doc(object):
    def __init__(self, rel, enc):
        self.rel = rel
        self.enc = enc
        self.p = os.path.join(ROOT, rel.replace('/', os.sep))
        self.raw = open(self.p, 'rb').read()
        self.nl = b'\r\n' if b'\r\n' in self.raw else b'\n'

    def t(self, s):
        s = s.replace('\n', '\r\n') if self.nl == b'\r\n' else s
        return s.encode(self.enc)

    def sub(self, old, new, label, count=1):
        ob, nb = self.t(old), self.t(new)
        n = self.raw.count(ob)
        assert n == count, '[%s] %s: 期望 %d 次，实际 %d 次' % (self.rel, label, count, n)
        self.raw = self.raw.replace(ob, nb)
        print('  ok:', label)

    def guard_line(self, pat, cond, label, count):
        """按“整行捕获 → 前置校验 → 原样重放”的方式插入校验"""
        nl = self.nl
        rx = re.compile(pat + rb'(\r\n|\n)')
        found = rx.findall(self.raw)
        assert len(found) == count, '[%s] %s: 期望 %d 次，实际 %d 次' % (self.rel, label, count, len(found))

        def repl(m):
            ind = m.group(1)
            line = m.group(2)
            eol = m.group(3)
            return (ind + b'if(' + cond + b')' + eol
                    + ind + b'{' + eol
                    + ind + b'    SVCRT_SVC_RET(p_svc_ctx, (uint32)(-1));' + eol
                    + ind + b'    break;' + eol
                    + ind + b'}' + eol
                    + ind + line + eol)

        self.raw = rx.sub(repl, self.raw, count=count)
        print('  ok:', label)

    def save(self):
        open(self.p, 'wb').write(self.raw)


def main():
    os.makedirs(BK, exist_ok=True)
    shutil.copy2(os.path.join(ROOT, 'kernelsrc', 'src', 'svcrt_task.c'),
                 os.path.join(BK, 'kernelsrc__src__svcrt_task.c'))

    d = Doc('kernelsrc/src/svcrt_task.c', 'gbk')

    # 1) 新增三个校验辅助函数（挂在 mq 缓冲区校验函数之后）
    d.sub(HELPERS_TAIL, HELPERS, '新增名字/缓冲区/回调表校验')

    NAME = b'svcrt_kernel_user_name_ok((const void *)p[1]) == 0u'
    BUF = b'svcrt_kernel_dev_buf_ok((const void *)p[2], (int32)p[3]) == 0u'

    # 2) 各类 create 的名字指针（event/sem/mtx/mq/timer）
    d.guard_line(rb'( +)(SVCRT_SVC_RET\(p_svc_ctx, svcrt_\w+_create_internal\([^\r\n]*)',
                 NAME, 'create 名字指针校验', 5)

    # 3) 设备打开 / 注销的名字指针
    d.guard_line(rb'( +)(SVCRT_SVC_RET\(p_svc_ctx, svcrt_dev_open_internal\([^\r\n]*)',
                 NAME, '设备打开名字校验', 1)
    d.guard_line(rb'( +)(SVCRT_SVC_RET\(p_svc_ctx, svcrt_dev_unregister\([^\r\n]*)',
                 NAME, '设备注销名字校验', 1)

    # 4) 设备读写缓冲区
    d.guard_line(rb'( +)(SVCRT_SVC_RET\(p_svc_ctx, svcrt_dev_read_internal\([^\r\n]*)',
                 BUF, '设备读缓冲区校验', 1)
    d.guard_line(rb'( +)(SVCRT_SVC_RET\(p_svc_ctx, svcrt_dev_write_internal\([^\r\n]*)',
                 BUF, '设备写缓冲区校验', 1)

    # 5) 驱动注册：名字 + 回调表
    d.guard_line(rb'( +)(SVCRT_SVC_RET\(p_svc_ctx, svcrt_dev_register\([^\r\n]*)',
                 NAME + b' ||\n' + b'svcrt_kernel_dev_drv_ok((const svcrt_dev_drv_t *)p[2]) == 0u',
                 '驱动注册回调表校验', 1)

    d.save()
    print('OK')


if __name__ == '__main__':
    main()
