# -*- coding: utf-8 -*-
"""收尾：修正驱动注册校验的续行缩进；补 task_stack_info 用户缓冲区校验
（该文件行尾为混合 LF/CRLF，这里两种都尝试）"""
import os

ROOT = r'D:\工作\git_project\svcrtos_new'
p = os.path.join(ROOT, 'kernelsrc', 'src', 'svcrt_task.c')
raw = open(p, 'rb').read()


def enc(s, eol):
    return s.replace('\n', eol).encode('gbk')


def rep(raw, old_lf, new_lf, label, count=1):
    for eol in ('\n', '\r\n'):
        o, n = enc(old_lf, eol), enc(new_lf, eol)
        c = raw.count(o)
        if c == count:
            print('  ok(%s): %s' % (repr(eol), label))
            return raw.replace(o, n)
    raise AssertionError('%s: 未找到匹配（LF/CRLF 都试过）' % label)


# 1) 续行缩进
raw = rep(raw,
          "                if(svcrt_kernel_user_name_ok((const void *)p[1]) == 0u ||\n"
          "svcrt_kernel_dev_drv_ok((const svcrt_dev_drv_t *)p[2]) == 0u)",
          "                if(svcrt_kernel_user_name_ok((const void *)p[1]) == 0u ||\n"
          "                   svcrt_kernel_dev_drv_ok((const svcrt_dev_drv_t *)p[2]) == 0u)",
          '驱动注册续行缩进')

# 2) task_stack_info 的用户缓冲区校验（内核要往它写 3 个字）
raw = rep(raw,
          "            case 10:\n",
          "            case 10:\n"
          "                if(svcrt_kernel_ptr_ram_ok((const void *)SVCRT_SVC_ARG(p_svc_ctx, 2), 12u) == 0u)\n"
          "                {\n"
          "                    SVCRT_SVC_RET(p_svc_ctx, (uint32)(-1));\n"
          "                    break;\n"
          "                }\n",
          'task_stack_info 缓冲区校验')

open(p, 'wb').write(raw)
print('DONE')
