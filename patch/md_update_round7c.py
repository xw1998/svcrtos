# -*- coding: utf-8 -*-
"""
文档同步（第 7 轮，第三部分）：把"驱动入口不能空转"这条写清楚

背景：svcrt_drv_main.c 原来在 DrvMain() 返回后是空转 while(1)，驱动优先级(9)
高于 App(10)，会吃满 CPU 饿死 App（本轮已改为 svcrt_task_wait(1000)）。
SDK 手册和 DRV_DEMO 的 README 只写了"不能返回，必须包含无限循环"，
容易让驱动作者照着写出空转循环，这里补上"循环里必须让出 CPU"。
"""
import os

ROOT = r'D:\工作\git_project\svcrtos_new'
EDITS = []


def E(path, old, new, note):
    EDITS.append((path, old, new, note))


E('kernelsrc/sdk/sdk_user_manual.md',
  '> **注意：** 用户态驱动的 `DrvMain()` 不能返回，必须包含无限循环。',
  """> **注意：** 用户态驱动的 `DrvMain()` 不能返回，必须包含无限循环，
> 而且**循环里必须让出 CPU**（`svcrt_task_wait()`，见上面的示例），
> 不要写空转的 `while(1){}`。
>
> 原因：驱动任务优先级（`DRIVER_TASK_PRIORITY`（`config/svcrt_partition.h`），默认 9）**高于** App 任务
> （`APP_TASK_PRIORITY`，默认 10），空转会吃满 CPU，把所有 App 永久饿死。
> 只注册设备、不需要后台维护的驱动，注册完让出 CPU 即可。
> SDK 自带的 `svcrt_drv_main.c` 在 `DrvMain()` 返回后也是用 `svcrt_task_wait(1000)` 兜底。""",
  'SDK 手册补"循环必须让出 CPU"')

E('example/stm32f427/driver_sdk/DRV_DEMO/README.md',
  '2. `DrvMain()` 不能返回，必须包含无限循环；',
  '2. `DrvMain()` 不能返回，必须包含无限循环，且循环里要用 `svcrt_task_wait()` 让出 CPU ——\n'
  '   驱动优先级(9)高于 App(10)，空转 `while(1){}` 会把 App 永久饿死；',
  'DRV_DEMO README 同步')


def main():
    for path, old, new, note in EDITS:
        p = os.path.join(ROOT, path.replace('/', os.sep))
        text = open(p, 'rb').read().decode('utf-8')
        eol = '\r\n' if '\r\n' in text else '\n'
        o = old.replace('\n', eol)
        n = new.replace('\n', eol)
        assert text.count(o) == 1, '%s / %s: 命中 %d 次' % (path, note, text.count(o))
        open(p, 'wb').write(text.replace(o, n).encode('utf-8'))
        print('  [ok] %s —— %s' % (path, note))
    print('完成')


if __name__ == '__main__':
    main()
