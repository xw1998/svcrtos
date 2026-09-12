# -*- coding: utf-8 -*-
"""
修复 20：svcrt.h 里消息队列的超时描述漏了"0 = 不等待"

svcrt_mq.c 的 send/recv 早已实现 `timeout_ms == 0` 提前返回（不登记等待者、不阻塞），
但 svcrt.h 的 @param 只写了"负值表示永久等待"，与 event/sem/mutex 的写法不一致，
自动生成的 API 参考也跟着漏。这里补齐。

svcrt.h 是 GBK 文件，必须按字节级补丁回写（GBK 编码 + CRLF）。
"""
import os
import shutil

ROOT = r'D:\工作\git_project\svcrtos_new'
BK = r'C:\Users\14905\OneDrive\Documents\lingxi-claw\20260911-13-47-19-445\backup_fix11'
REL = 'kernelsrc/include/svcrt.h'

REPL = [
    ('* @param timeout   队列满时的等待时间（ms），负值表示永久等待',
     '* @param timeout   队列满时的等待时间（ms），0 表示不等待，负值表示永久等待'),
    ('* @param timeout   超时时间（ms），负值表示永久等待',
     '* @param timeout   超时时间（ms），0 表示不等待，负值表示永久等待'),
]


def main():
    os.makedirs(BK, exist_ok=True)
    p = os.path.join(ROOT, REL.replace('/', os.sep))
    raw = open(p, 'rb').read()
    shutil.copy2(p, os.path.join(BK, 'svcrt.h'))
    text = raw.decode('gbk')
    for old, new in REPL:
        if new in text:
            print('  已是修好的内容，跳过: %s' % old[:24])
            continue
        cnt = text.count(old)
        assert cnt == 1, '锚点不唯一（%d 次）: %s' % (cnt, old)
        text = text.replace(old, new)
        print('  替换 1 处: %s' % old[:24])
    open(p, 'wb').write(text.encode('gbk'))
    # 回读校验
    open(p, 'rb').read().decode('gbk')
    print('  saved %s [gbk]' % REL)


if __name__ == '__main__':
    main()
