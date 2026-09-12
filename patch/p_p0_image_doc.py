# -*- coding: utf-8 -*-
"""P0-3 文档同步：把“负载基址 = 槽位基址 + 镜像头长度”的契约写进文档"""
import os
import shutil

ROOT = r'D:\工作\git_project\svcrtos_new'
BK = r'C:\Users\14905\OneDrive\Documents\lingxi-claw\20260911-13-47-19-445\backup_p0_fault'

DOC = 'docs/Loader工程化落地说明.md'
FILES = [DOC]


def patch(rel, pairs):
    p = os.path.join(ROOT, rel.replace('/', os.sep))
    raw = open(p, 'rb').read()
    nl = b'\r\n' if b'\r\n' in raw else b'\n'

    def t(s):
        return (s.replace('\n', '\r\n') if nl == b'\r\n' else s).encode('utf-8')

    for old, new, label in pairs:
        ob, nb = t(old), t(new)
        n = raw.count(ob)
        assert n == 1, '[%s] %s: 匹配 %d 次' % (rel, label, n)
        raw = raw.replace(ob, nb)
        print('  ok:', label)
    open(p, 'wb').write(raw)


def do_backup():
    os.makedirs(BK, exist_ok=True)
    for rel in FILES:
        shutil.copy2(os.path.join(ROOT, rel.replace('/', os.sep)),
                     os.path.join(BK, rel.replace('/', '__')))


OLD_FLOW = """4. 写入镜像（256 字节头 + 负载）；设备路径按 512 字节分块流式读写，不要求整镜像驻留 RAM；
5. 从 Flash 回读复算 CRC 确认；
6. 槽位状态置 `LOADED`，记录入口 `slot_base + entry_offset`（ARM 自动置 Thumb 位）；"""

NEW_FLOW = """4. 写入镜像到槽位：`[槽位基址]` 是 256 字节头，`[槽位基址 + 256]` 起是负载；
   设备路径按 512 字节分块流式读写，不要求整镜像驻留 RAM；
5. 从 Flash 回读复算 CRC 确认；
6. 槽位状态置 `LOADED`，记录入口 `槽位基址 + 256 + entry_offset`（ARM 自动置 Thumb 位）；
   `entry_offset` 是入口相对**负载起始**的偏移，而负载的首地址是 `槽位基址 + 256`
   （见 `svcrt_loader_entry_addr()`；这也意味着 App 工程的 .sct 必须把负载链接到
   `槽位基址 + 256`，见 5.4 与 `tools/gen_scatter.py`）；"""

OLD_54 = """**开发期「固定地址烧录 + MDK 下断点调试」是硬需求，不能被安装流程取代。**
两条路用的是**同一个链接基址**（都源于 `config/svcrt_partition.h`），因此调试态与发布态的代码布局完全一致。

| | 开发调试路径 | 发布/安装路径 |
|---|---|---|
| 产物 | 裸镜像（`.axf` / `.bin`，无镜像头） | `.svcapp`（256B 头 + CRC32） |
| 落位 | Keil 直接 Download 到分区固定地址 | 安装任务经 COM1 流式写入 |
| 入口 | 分区基址（\\|1，即 `APPSTART`） | 基址 + `entry_offset` |
| 校验 | 无（开发期不逐镜像校验） | 魔数 + 硬件兼容签名 + CRC32 |
| App | APP_DEMO / BLED_APP 工程直接编译调试 | 打包后由安装任务写入 |
| 驱动 | BLED_DRV / DRV_DEMO 工程直接编译调试 | 已支持（裸镜像识别 + `svcrt_driver_load` + 安装任务分流） |

实现方式：`svcrt_loader_identify()` 对同一个分区先试「带头 .svcapp」，不成立再看是不是擦除态；
不是擦除态且 `APP_ALLOW_RAW_IMAGE = 1` 时，当作开发期裸镜像，入口取分区基址。"""

NEW_54 = """**开发期「固定地址烧录 + MDK 下断点调试」是硬需求，不能被安装流程取代。**
两条路都保留，但**链接基址不同**——这是 `.svcapp` 在槽位里带头部的必然结果
（与 MCUboot 等方案一致：镜像头占据槽位起始，负载从「槽位基址 + 头长」开始）：

- **发布/安装路径**：槽位前 256 字节是镜像头，负载落在 `槽位基址 + 256`。
  工程 .sct 必须把负载链接到 `槽位基址 + 256`，即
  `python tools/gen_scatter.py --target app|driver --output build/app.sct`（默认布局）。
- **开发调试路径**：裸镜像没有镜像头，负载直接落在槽位基址。
  用 `python tools/gen_scatter.py --raw` 生成 `build/app_dev.sct` / `build/driver_dev.sct`。

| | 开发调试路径 | 发布/安装路径 |
|---|---|---|
| 产物 | 裸镜像（`.axf` / `.bin`，无镜像头） | `.svcapp`（256B 头 + CRC32） |
| .sct | `build/app_dev.sct` / `build/driver_dev.sct`（`--raw`） | `build/app.sct` / `build/driver.sct`（默认） |
| 链接基址 | `APP_SLOT0_BASE` = 0x08080000 | `APP_SLOT0_BASE + 256` = 0x08080100 |
| 落位 | Keil 直接 Download 到分区固定地址 | 安装任务经 COM1 流式写入 |
| 入口 | 分区基址（\\|1） | 分区基址 + 256 + `entry_offset` |
| 校验 | 无（开发期不逐镜像校验） | 魔数 + 硬件兼容签名 + CRC32 |
| App | APP_DEMO / BLED_APP 工程直接编译调试 | 打包后由安装任务写入 |
| 驱动 | BLED_DRV / DRV_DEMO 工程直接编译调试 | 已支持（裸镜像识别 + `svcrt_driver_load` + 安装任务分流） |

切换路径时需要同时改工程里的两处（两套 .sct 由 `gen_scatter.py` 自动生成）：

- `<ScatterFile>`：指向 `build\\app.sct`（发布）或 `build\\app_dev.sct`（调试）；
- `<UserProg1Name>` 里的 `gen_scatter.py` 命令：去掉或加上 `--raw`。

实现方式：`svcrt_loader_identify()` 对同一个分区先试「带头 .svcapp」，不成立再看是不是擦除态；
不是擦除态且 `APP_ALLOW_RAW_IMAGE = 1` 时，当作开发期裸镜像，入口取分区基址。"""

OLD_CHK = """工具会自动校验「ELF 镜像基址 == 分区槽位基址」，不一致直接报错并提示检查 `.sct`——
这是防止链接布局与打包布局悄悄错位的关键护栏。"""

NEW_CHK = """工具会自动校验「ELF 镜像基址 == 分区槽位基址 + 镜像头长度」
（`config/svcrt_partition.h` 的 `APP_IMAGE_HEADER_SIZE`），不一致直接报错并提示检查 `.sct`——
这是防止链接布局与打包布局悄悄错位的关键护栏。
镜像头里的 `load_addr` 记录的是**目标槽位基址**（内核据此确认镜像装到了正确的槽位），
不是负载基址；入口地址 = `load_addr + 256 + entry_offset`（`--info` 会分别打印）。"""


if __name__ == '__main__':
    do_backup()
    patch(DOC, [
        (OLD_FLOW, NEW_FLOW, '加载流程入口说明'),
        (OLD_54, NEW_54, '5.4 两条路径改写'),
        (OLD_CHK, NEW_CHK, '打包工具校验说明'),
    ])
    print('OK')
