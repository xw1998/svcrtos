#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
SVCrtOS 工程 .sct 生成包装（App / 驱动）
========================================

Keil 工程在 BeforeMake 钩子里调用本脚本，由它决定当前该生成哪一套 .sct：

    SVCRT_SCT_MODE = dev（默认）
        固定落点的裸镜像布局：负载直接落在开发槽位表声明的单元基址上，
        没有镜像头也没有重定位表。**这一份是给 MDK 直接下载 + 断点调试用的**，
        是必须长期保留的开发旁路。

    SVCRT_SCT_MODE = nominal
        标称基址布局：负载链接在池基址 + 镜像头长处，运行期由内核搬到
        任意 2 的幂对齐落点、并按重定位表打补丁。打包工具
        （tools/pack_app.py）在每遍编译前设置本模式与下面两个变量，
        用来做「同一份代码、四个链接基址」的差分。

    SVCRT_ROM_DELTA / SVCRT_RAM_DELTA
        差异链接偏移量（十六进制或十进制），仅 nominal 模式有效。

输出的 .sct 路径恒为 build/<工程输出名>.sct，与 .uvprojx 里的
<ScatterFile> 一致——两条路径共用同一个文件名，靠环境变量切换内容。
这样工程文件里不需要为「调试」和「打包」维护两套配置。

用法（一般由 BeforeMake 钩子调用）：

    python tools/gen_app_sct.py --project <uvprojx> --type app \
        --dev-slot 1 --ram-size 8192

    --heap-size 给 C 库留一块堆（夹在 RW 与栈之间），需要 malloc 的工程才加；
    不加就没有堆，malloc 在链接期直接报 L6915E。
"""

from __future__ import print_function

import argparse
import os
import subprocess
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO_ROOT, "tools"))

import pack_app  # noqa: E402  复用 uvprojx 解析（含 OutputDirectory/OutputName）


def auto_int(text):
    return int(text, 0)


def main():
    ap = argparse.ArgumentParser(description="SVCrtOS 工程 .sct 生成包装")
    ap.add_argument("--project", required=True, help="Keil 工程（.uvprojx）")
    ap.add_argument("--target-name", help="工程里的 Target 名（默认取第一个）")
    ap.add_argument("--type", choices=["app", "driver", "miniapp"], default="app")
    ap.add_argument("--dev-slot", type=int, default=0,
                    help="开发槽位表条目号（裸镜像的固定落点，与 config 一致）")
    ap.add_argument("--ram-size", type=auto_int, default=None,
                    help="标称模式下镜像 RW/ZI + 栈 的 RAM 字节数（2 的幂）")
    ap.add_argument("--heap-size", type=auto_int, default=0,
                    help="给 C 库留的堆字节数（0 = 不留，malloc 会在链接期报错）")
    ap.add_argument("--mode", choices=["dev", "nominal"], default=None,
                    help="覆盖环境变量 SVCRT_SCT_MODE")
    ap.add_argument("--output", help="输出路径（默认 build/<工程输出名>.sct）")
    args = ap.parse_args()

    uvprojx = os.path.abspath(args.project)
    if not os.path.isfile(uvprojx):
        # UV4 执行 BeforeMake 钩子时的工作目录就是 .uvprojx 所在目录，钩子里的
        # 相对路径按该目录解析。为兼容手工执行或从别处调用，这里再按文件名
        # 在当前目录及其父目录下各找一次。
        base = os.path.basename(args.project)
        found = None
        for cand in (os.path.join(os.getcwd(), base),
                     os.path.join(os.getcwd(), os.pardir, base)):
            if os.path.isfile(cand):
                found = cand
                break
        if found is None:
            raise SystemExit("找不到工程文件: %s" % uvprojx)
        uvprojx = os.path.abspath(found)

    targets = pack_app.read_project_targets(uvprojx)
    tname = args.target_name or targets[0][0]
    match = [t for t in targets if t[0] == tname]
    if not match:
        raise SystemExit("工程 %s 里没有 Target \"%s\"" % (uvprojx, tname))
    outname = match[0][2] or os.path.splitext(os.path.basename(uvprojx))[0]

    out = args.output or os.path.join(REPO_ROOT, "build", outname + ".sct")

    mode = args.mode or os.environ.get("SVCRT_SCT_MODE", "dev").strip().lower()
    if mode not in ("dev", "nominal"):
        raise SystemExit("SVCRT_SCT_MODE 只能是 dev 或 nominal（收到 %r）" % mode)

    cmd = [sys.executable, os.path.join(REPO_ROOT, "tools", "gen_scatter.py"),
           "--target", "image", "--type", args.type, "--output", out]
    if args.heap_size:
        cmd += ["--heap-size", str(args.heap_size)]
    if mode == "dev":
        cmd += ["--raw", "--dev-slot", str(args.dev_slot)]
    else:
        cmd += ["--nominal"]
        if args.ram_size is not None:
            cmd += ["--ram-size", str(args.ram_size)]
        for var, flag in (("SVCRT_ROM_DELTA", "--rom-delta"),
                          ("SVCRT_RAM_DELTA", "--ram-delta")):
            val = os.environ.get(var)
            if val:
                cmd += [flag, val]

    rc = subprocess.call(cmd)
    if rc != 0:
        raise SystemExit("gen_scatter 失败（退出码 %d）" % rc)
    return 0


if __name__ == "__main__":
    sys.exit(main())
