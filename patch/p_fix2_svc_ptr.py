# -*- coding: utf-8 -*-
"""
修复 8：SVC 分发对用户指针零校验（可信边界）

问题：SVC_Server 直接把用户寄存器里的值当指针用：
  - 参数块本身 p = SVCRT_SVC_ARG(...) 未校验就 p[0..5] 解引用；
  - mq_send/mq_recv 的缓冲区指针来自 p[2]，可以指向任意地址（读写 16 字节）；
  - dev/sync/mq/timer 的名字指针、timer 回调函数指针同样未校验，
    回调在内核特权态执行 —— 配合 COM1 安装通道就是一个可控的执行原语。

修复（只在 SVC 分发入口做可信边界校验，不动内部实现，
避免影响内核自身的直接调用）：
  - 参数块必须落在“用户可见 RAM 窗口”内（共享区/App RAM/驱动 RAM）；
  - mq 缓冲区必须落在同一窗口内，且字数在 [1, SVCRT_MQ_MSG_WORDS] 内；
  - 定时器回调与参数指针必须落在“用户可见 RAM + 用户固件区（Flash）”内；
  - 内核 RAM（任务表、内核栈）与内核 Flash 一律拒绝。
"""
import os
import shutil

ROOT = r'D:\工作\git_project\svcrtos_new'
BK = r'C:\Users\14905\OneDrive\Documents\lingxi-claw\20260911-13-47-19-445\backup_fix2'


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

    def save(self):
        open(self.p, 'wb').write(self.raw)


HELPERS = """#include "svcrt_loader.h"
#include "svcrt_partition.h"    /* 用户可见内存窗口（共享区/App RAM/驱动 RAM/固件区） */

/* ------------------------------------------------------------------
 * 用户指针校验（SVC 可信边界）
 * SVC 分发把用户寄存器里的值直接当指针解引用（参数块、消息缓冲区、
 * 设备名、定时器回调……），不做校验时用户态可借这些接口读写内核任意地址、
 * 或让内核在特权态执行任意函数。
 * 这里按“地址必须落在用户可见窗口内”做最低限度校验：
 *   RAM 窗口：SHARE / APP_RAM / DRIVER_RAM（可写缓冲区允许的范围）
 *   固件窗口：DRIVER_POOL / APP_USER（用户代码与只读常量所在）
 * 内核 RAM（任务表、内核栈）与内核 Flash 一律拒绝。
 * ------------------------------------------------------------------ */
static uint8 svcrt_kernel_in_window(uint32 start, uint32 end, uint32 base, uint32 size)
{
    uint32 limit = base + size;

    if(limit <= base)                   /* 分区宏异常时不要误放行 */
    {
        return 0u;
    }
    return ((start >= base) && (end <= limit)) ? 1u : 0u;
}

static uint8 svcrt_kernel_ptr_ram_ok(const void *p, uint32 len)
{
    uint32 start = (uint32)p;
    uint32 end   = start + len;

    if(p == 0 || end < start)           /* 空指针或长度回绕 */
    {
        return 0u;
    }
    if(svcrt_kernel_in_window(start, end, SHARE_RAM_BASE, SHARE_RAM_SIZE) != 0u)
    {
        return 1u;
    }
    if(svcrt_kernel_in_window(start, end, APP_RAM_BASE, APP_RAM_SIZE) != 0u)
    {
        return 1u;
    }
    if(svcrt_kernel_in_window(start, end, DRIVER_RAM_BASE, DRIVER_RAM_SIZE) != 0u)
    {
        return 1u;
    }
    return 0u;
}

static uint8 svcrt_kernel_ptr_flash_ok(const void *p, uint32 len)
{
    uint32 start = (uint32)p;
    uint32 end   = start + len;

    if(p == 0 || end < start)
    {
        return 0u;
    }
    if(svcrt_kernel_ptr_ram_ok(p, len) != 0u)
    {
        return 1u;
    }
    if(svcrt_kernel_in_window(start, end, DRIVER_POOL_BASE, DRIVER_POOL_SIZE) != 0u)
    {
        return 1u;
    }
    if(svcrt_kernel_in_window(start, end, APP_USER_BASE, APP_USER_SIZE) != 0u)
    {
        return 1u;
    }
    return 0u;
}

/* SVC 参数块（sub-cmd + 参数数组）必须位于用户可见 RAM */
static uint8 svcrt_kernel_svc_args_ok(const void *p, uint32 len)
{
    return svcrt_kernel_ptr_ram_ok(p, len);
}

/* 消息缓冲区：字数必须在 [1, SVCRT_MQ_MSG_WORDS] 内，且整块位于用户可见 RAM */
static uint8 svcrt_kernel_mq_buf_ok(const void *p, int32 len_words)
{
    if(len_words <= 0 || len_words > SVCRT_MQ_MSG_WORDS)
    {
        return 0u;
    }
    return svcrt_kernel_ptr_ram_ok(p, (uint32)len_words * 4u);
}"""

ARG_CHECK = """        p = (uint32 *)SVCRT_SVC_ARG(p_svc_ctx, 0);
        /* 用户参数块必须先校验再解引用：内核 RAM / 内核 Flash 一律拒绝 */
        if(svcrt_kernel_svc_args_ok(p, 16u) == 0u)
        {
            SVCRT_SVC_RET(p_svc_ctx, (uint32)(-1));
            break;
        }"""

MQ_SEND_OLD = """        case 2:
            SVCRT_SVC_RET(p_svc_ctx, svcrt_mq_send_internal((int32)p[1], (uint32 *)p[2], (int32)p[3], (int32)p[4]));"""
MQ_SEND_NEW = """        case 2:
            if(svcrt_kernel_svc_args_ok(p, 24u) == 0u ||
               svcrt_kernel_mq_buf_ok((const void *)p[2], (int32)p[3]) == 0u)
            {
                SVCRT_SVC_RET(p_svc_ctx, (uint32)(-1));
                break;
            }
            SVCRT_SVC_RET(p_svc_ctx, svcrt_mq_send_internal((int32)p[1], (uint32 *)p[2], (int32)p[3], (int32)p[4]));"""

MQ_RECV_OLD = """        case 3:
            SVCRT_SVC_RET(p_svc_ctx, svcrt_mq_recv_internal((int32)p[1], (uint32 *)p[2], (int32)p[3], (int32)p[4]));"""
MQ_RECV_NEW = """        case 3:
            if(svcrt_kernel_svc_args_ok(p, 24u) == 0u ||
               svcrt_kernel_mq_buf_ok((const void *)p[2], (int32)p[3]) == 0u)
            {
                SVCRT_SVC_RET(p_svc_ctx, (uint32)(-1));
                break;
            }
            SVCRT_SVC_RET(p_svc_ctx, svcrt_mq_recv_internal((int32)p[1], (uint32 *)p[2], (int32)p[3], (int32)p[4]));"""

TIMER_OLD = """        case 2:
            SVCRT_SVC_RET(p_svc_ctx, svcrt_timer_start_internal((int32)p[1], p[2], (uint8)p[3],
                                                       (void (*)(void *))p[4], (void *)p[5]));"""
TIMER_NEW = """        case 2:
            /* 回调会在内核特权态执行：函数指针与参数指针都必须位于用户可见区域 */
            if(svcrt_kernel_svc_args_ok(p, 24u) == 0u ||
               svcrt_kernel_ptr_flash_ok((const void *)p[4], 2u) == 0u ||
               svcrt_kernel_ptr_ram_ok((const void *)p[5], 1u) == 0u)
            {
                SVCRT_SVC_RET(p_svc_ctx, (uint32)(-1));
                break;
            }
            SVCRT_SVC_RET(p_svc_ctx, svcrt_timer_start_internal((int32)p[1], p[2], (uint8)p[3],
                                                       (void (*)(void *))p[4], (void *)p[5]));"""


def main():
    os.makedirs(BK, exist_ok=True)
    shutil.copy2(os.path.join(ROOT, 'kernelsrc', 'src', 'svcrt_task.c'),
                 os.path.join(BK, 'kernelsrc__src__svcrt_task.c'))

    d = Doc('kernelsrc/src/svcrt_task.c', 'gbk')
    d.sub('#include "svcrt_loader.h"', HELPERS, '新增用户指针校验辅助函数')
    d.sub('        p = (uint32 *)SVCRT_SVC_ARG(p_svc_ctx, 0);\n', ARG_CHECK + '\n',
          'SVC 参数块校验', count=7)
    d.sub(MQ_SEND_OLD, MQ_SEND_NEW, 'mq_send 缓冲区校验')
    d.sub(MQ_RECV_OLD, MQ_RECV_NEW, 'mq_recv 缓冲区校验')
    d.sub(TIMER_OLD, TIMER_NEW, 'timer_start 回调校验')
    d.save()
    print('OK')


if __name__ == '__main__':
    main()
