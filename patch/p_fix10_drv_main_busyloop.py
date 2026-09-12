# -*- coding: utf-8 -*-
"""
修复 19：驱动入口（用户态模式的 main）用空转 while(1) 吃满 CPU

kernelsrc/sdk/driver_sdk/svcrt_drv_main.c 被 BLED_DRV / DRV_DEMO 两个驱动
工程直接编译进去，内容是：

    int main(void)
    {
        DrvMain();
        while(1)
        {
        }
    }

驱动任务优先级 SVCRT_DRIVER_TASK_PRIORITY = 9，高于 App 的 10，
DrvMain() 返回后这个空转循环会把 CPU 全占掉，所有 App 永久得不到调度。
svcrt_app_main.c 里的兜底循环写法（svcrt_task_wait(1000)）才是对的。

改法：照抄 App 侧写法，让驱动的兜底循环主动让出时间片。

该文件是 GBK + LF，必须按原编码回写，不能用编辑器直接改（会写成 UTF-8）。
"""
import os
import shutil

ROOT = r'D:\工作\git_project\svcrtos_new'
BK = r'C:\Users\14905\OneDrive\Documents\lingxi-claw\20260911-13-47-19-445\backup_fix10'

REL = 'kernelsrc/sdk/driver_sdk/svcrt_drv_main.c'

OLD = """int main(void)
{
    DrvMain();
    while(1)
    {
    }
}"""

NEW = """int main(void)
{
    DrvMain();

    /* 驱动注册完就该让出 CPU。这里不能写空转的 while(1)：
     * 驱动任务优先级(9)高于 App 任务(10)，空转会吃满 CPU 把 App 全饿死。
     * 与 svcrt_app_main.c 的兜底循环保持一致，靠 svcrt_task_wait 让出时间片。 */
    while(1)
    {
        svcrt_task_wait(1000);
    }
}"""


def detect_enc(raw):
    try:
        raw.decode('utf-8')
        return 'utf-8'
    except UnicodeDecodeError:
        return 'gbk'


def main():
    os.makedirs(BK, exist_ok=True)
    p = os.path.join(ROOT, REL.replace('/', os.sep))
    raw = open(p, 'rb').read()
    enc = detect_enc(raw)
    shutil.copy2(p, os.path.join(BK, REL.replace('/', '__')))

    text = raw.decode(enc)
    if 'svcrt_task_wait(1000)' in text:
        print('  已是修好的内容，跳过')
        return

    eol = '\r\n' if '\r\n' in text else '\n'
    old = OLD.replace('\n', eol)
    new = NEW.replace('\n', eol)
    assert text.count(old) == 1, '锚点不唯一，实际出现 %d 次' % text.count(old)
    text = text.replace(old, new)
    open(p, 'wb').write(text.encode(enc))
    print('  saved %s [%s]' % (REL, enc))


if __name__ == '__main__':
    main()
