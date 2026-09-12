# -*- coding: utf-8 -*-
"""P0-3 文档同步（安装与调试指南）"""
import os
import shutil

ROOT = r'D:\工作\git_project\svcrtos_new'
BK = r'C:\Users\14905\OneDrive\Documents\lingxi-claw\20260911-13-47-19-445\backup_p0_fault'
DOC = 'docs/SVCrtOS应用安装与调试指南.md'


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


P1_OLD = r"""2. **Linker → Scatter File** 指向生成的 `.sct`：
   - 内核 → `build/kernel.sct`
   - App → `build/app.sct`
   - 驱动 → `build/driver.sct`
3. **User → Before Build/Rebuild** 已挂上生成命令（示例）：
   ```
   python ..\..\..\..\..\tools\gen_scatter.py --target app --output ..\..\..\..\..\build\app.sct
   ```
   > 该命令需要 `python` 在系统 PATH 中。若不想让编译依赖 Python，
   > 可清空这一栏，改为手工运行 `python tools/gen_scatter.py --target all --output build`。"""

P1_NEW = r"""2. **Linker → Scatter File** 指向生成的 `.sct`（注意有「安装」和「调试」两套布局，不能混用）：
   - 内核 → `build/kernel.sct`
   - App：发布/安装 → `build/app.sct`；开发调试裸镜像 → `build/app_dev.sct`
   - 驱动：发布/安装 → `build/driver.sct`；开发调试裸镜像 → `build/driver_dev.sct`
3. **User → Before Build/Rebuild** 已挂上生成命令（示例）：
   ```
   python ..\..\..\..\..\tools\gen_scatter.py --target app --output ..\..\..\..\..\build\app.sct
   ```
   > 该命令需要 `python` 在系统 PATH 中。若不想让编译依赖 Python，
   > 可清空这一栏，改为手工运行 `python tools/gen_scatter.py --target all --output build`
   > （`--target all` 会一次生成两套：安装布局 `app.sct`/`driver.sct` + 调试布局 `app_dev.sct`/`driver_dev.sct`）。
   >
   > **为什么是两套**：`.svcapp` 的槽位布局是 `[256 字节镜像头][负载]`，
   > 所以负载的链接基址 = 槽位基址 + 256；而开发调试的裸镜像没有镜像头，负载直接落在槽位基址。
   > 切换路径时 `.sct` 与 Before Build 命令要一起换（调试布局在命令里加 `--raw`）。"""

P2_OLD = """3. 打开 App 工程（如 `APP_DEMO`）→ 编译（Before Build 会自动生成 `build/app.sct`）
4. **Keil 直接 Download**（会按 `.sct` 的加载区域写入 `0x08080000`）"""

P2_NEW = """3. 打开 App 工程（如 `APP_DEMO`）→ 按 §2.1 把 Scatter File 换成 `build/app_dev.sct`
   （Before Build 里对应改成
   `python ..\\..\\..\\..\\..\\tools\\gen_scatter.py --target app --raw --output ..\\..\\..\\..\\..\\build\\app_dev.sct`）
4. **Keil 直接 Download**（会按 `.sct` 的加载区域写入 `0x08080000`）"""

P3_OLD = """- 工程换成 `BLED_DRV` / `DRV_DEMO`，链接到 `build/driver.sct`"""
P3_NEW = """- 工程换成 `BLED_DRV` / `DRV_DEMO`，调试时链接到 `build/driver_dev.sct`（裸镜像布局）"""

P4_OLD = """只要 App 的**链接基址没变**（即 `config/svcrt_partition.h` 没改），
重新编译 + Download App 即可，复位后内核会重新扫描并拉起新版本。"""

P4_NEW = """只要 App 的**链接基址没变**（即 `config/svcrt_partition.h` 没改，且仍用同一套 `.sct`），
重新编译 + Download App 即可，复位后内核会重新扫描并拉起新版本。

> 调试布局（`*_dev.sct`）与安装布局（`app.sct`/`driver.sct`）的链接基址相差 256 字节，
> 两者**不是同一份二进制**：换了 `.sct` 必须重新编译，不能拿调试产物去安装、反之亦然。"""

P5_OLD = """打包工具会**强制校验**"ELF 镜像基址 == 分区槽位基址"，不一致直接报错——
这是防止链接布局与打包布局悄悄错位的护栏。"""

P5_NEW = """打包工具会**强制校验**"ELF 镜像基址 == 分区槽位基址 + 镜像头长度（`APP_IMAGE_HEADER_SIZE`，256）"，
不一致直接报错——这是防止链接布局与打包布局悄悄错位的护栏
（即：打包 `.svcapp` 的工程必须用默认布局的 `.sct`，不能用 `--raw` 的调试布局）。
镜像头里的 `load_addr` 记录的是**目标槽位基址**，内核据此确认镜像装到了正确的槽位。"""

P6_OLD = """- `.svcapp` 是"256 字节头 + 负载"的整体，**从头开始写到槽位基址即可**，不需要拆开。
- 注意：**同一个槽位要么放 `.svcapp`，要么放裸镜像，不能混**——
  带头的镜像入口是 `基址 + entry_offset`，裸镜像入口是 `基址`，两种布局不同。"""

P6_NEW = """- `.svcapp` 是"256 字节头 + 负载"的整体，**从头开始写到槽位基址即可**，不需要拆开；
  其负载的链接基址是 `槽位基址 + 256`，所以这些镜像必须由使用默认（非 `--raw`）`.sct` 的工程产出。
- 注意：**同一个槽位要么放 `.svcapp`，要么放裸镜像，不能混**——
  带头的镜像入口是 `槽位基址 + 256 + entry_offset`，裸镜像入口是 `槽位基址`，
  两种布局相差 256 字节，混用会直接跑飞。"""

P7_OLD = """| `SVCA`（`0x53564341`） | — | 带镜像头 → 校验兼容签名与 CRC | `基址 + entry_offset` |"""
P7_NEW = """| `SVCA`（`0x53564341`） | — | 带镜像头 → 校验兼容签名与 CRC | `基址 + 256 + entry_offset` |"""


if __name__ == '__main__':
    os.makedirs(BK, exist_ok=True)
    shutil.copy2(os.path.join(ROOT, DOC.replace('/', os.sep)),
                 os.path.join(BK, DOC.replace('/', '__')))
    patch(DOC, [
        (P1_OLD, P1_NEW, 'Scatter File 两套布局'),
        (P2_OLD, P2_NEW, '调试 App 步骤'),
        (P3_OLD, P3_NEW, '调试驱动'),
        (P4_OLD, P4_NEW, '只重下 App'),
        (P5_OLD, P5_NEW, '打包校验说明'),
        (P6_OLD, P6_NEW, '路径C 布局说明'),
        (P7_OLD, P7_NEW, '认定规则表格'),
    ])
    print('OK')
