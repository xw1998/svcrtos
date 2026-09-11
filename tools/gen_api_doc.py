#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
SVCrtOS API 文档生成脚本
========================

背景：内核源码为 GBK 与 UTF-8 混用，而 *.md 文档为 UTF-8，直接运行 Doxygen
      会导致其中一部分注释乱码。本脚本先把源码统一转成 UTF-8 到系统临时目录，
      再据此生成文档，因此不会改动仓库里的任何源文件，也不会在仓库内留下中间产物。

功能：
  1. 把仓库源码（kernelsrc/、board/）与说明文档转换为 UTF-8 临时树（%TEMP%）；
  2. 若本机已安装 Doxygen，按其配置生成 HTML API 文档（docs/api/html）；
  3. 若未安装 Doxygen，使用内置解析器生成 Markdown API 参考
     （docs/api/SVCrtOS_API参考.md），保证无 Doxygen 环境也能出文档。

用法：
  python tools/gen_api_doc.py          # 有 Doxygen 用 Doxygen，否则回退 Markdown
  python tools/gen_api_doc.py --md     # 强制使用 Markdown 生成器
  python tools/gen_api_doc.py --both   # HTML 与 Markdown 都生成
"""

from __future__ import print_function

import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GEN = os.path.join(tempfile.gettempdir(), "svcrtos_api_gen")
OUT_DIR = os.path.join(ROOT, "docs", "api")
OUT_MD = os.path.join(OUT_DIR, "SVCrtOS_API参考.md")
TEMP_DOXYFILE = os.path.join(GEN, "Doxyfile.generated")

SRC_DIRS = ["kernelsrc/include", "kernelsrc/src", "kernelsrc/port",
            "kernelsrc/sdk", "kernelsrc/app", "board"]
SRC_DOCS = ["readme.md", "kernelsrc/user_manual.md", "kernelsrc/porting_manual.md"]
TEXT_EXT = (".h", ".c", ".s", ".S", ".md")


# --------------------------------------------------------------- 编码转换
def decode_text(raw):
    """逐行尝试 UTF-8 / GBK，兼容工程内两种编码混用"""
    out = []
    for line in raw.split(b"\n"):
        text = None
        for enc in ("utf-8", "gbk"):
            try:
                text = line.decode(enc)
                break
            except UnicodeDecodeError:
                continue
        out.append(text if text is not None else line.decode("utf-8", "replace"))
    return "\n".join(out)


def build_utf8_tree():
    count = 0
    for rel in SRC_DIRS:
        src = os.path.join(ROOT, rel)
        if not os.path.isdir(src):
            continue
        for dirpath, _dirs, files in os.walk(src):
            for fn in files:
                if not fn.endswith(TEXT_EXT):
                    continue
                sp = os.path.join(dirpath, fn)
                dp = os.path.join(GEN, os.path.relpath(sp, ROOT))
                parent = os.path.dirname(dp)
                if not os.path.isdir(parent):
                    os.makedirs(parent)
                with open(sp, "rb") as f:
                    text = decode_text(f.read())
                with open(dp, "w", encoding="utf-8") as f:
                    f.write(text)
                count += 1
    for rel in SRC_DOCS:
        sp = os.path.join(ROOT, rel)
        if not os.path.isfile(sp):
            continue
        dp = os.path.join(GEN, rel)
        if not os.path.isdir(os.path.dirname(dp)):
            os.makedirs(os.path.dirname(dp))
        with open(sp, "rb") as f:
            text = decode_text(f.read())
        with open(dp, "w", encoding="utf-8") as f:
            f.write(text)
        count += 1
    print("[1/2] 已转换 %d 个文件到 UTF-8 临时树: %s" % (count, GEN))


# --------------------------------------------------------------- Markdown 生成
def clean_comment(block):
    out = []
    for raw in block:
        s = raw.strip()
        if s.startswith("/**"):
            s = s[3:]
        elif s.startswith("*/"):
            s = s[2:]
        s = s.lstrip("*").strip()
        if not s or s.startswith("@file") or s.startswith("@defgroup") or s.startswith("@{"):
            continue
        if s.startswith("@brief"):
            out.append("**%s**" % s[6:].strip())
        elif s.startswith("@details"):
            out.append(s[8:].strip())
        elif s.startswith("@note"):
            out.append("> 说明：%s" % s[5:].strip())
        elif s.startswith("@param"):
            body = s[6:].strip()
            if " " in body:
                name, desc = body.split(" ", 1)
                out.append("- `%s`：%s" % (name, desc.strip()))
            else:
                out.append("- %s" % body)
        elif s.startswith("@return"):
            out.append("**返回**：%s" % s[7:].strip())
        elif s.startswith("@code") or s.startswith("@endcode") or s.startswith("@}"):
            continue
        elif s.startswith("@"):
            out.append("`%s`" % s)
        else:
            out.append(s)
    return out


def collect_decl(lines, i):
    decl = []
    while i < len(lines) and len(decl) < 6:
        s = lines[i].strip()
        if not s:
            if decl:
                break
            i += 1
            continue
        if s.startswith("/*"):
            break
        decl.append(s)
        if s.endswith(";") or s.endswith("{"):
            break
        i += 1
    return " ".join(decl)


def gen_markdown():
    if not os.path.isdir(OUT_DIR):
        os.makedirs(OUT_DIR)

    out = [u"# SVCrtOS API 参考", u"",
           u"> 由 `tools/gen_api_doc.py` 自动生成，请勿手工修改。",
           u"> 安装 [Doxygen](https://www.doxygen.nl/) 后重新运行脚本，可得到带交叉引用与调用关系的 HTML 版本。",
           u""]
    index, sections = [], []

    files = []
    for dirpath, _dirs, fnames in os.walk(os.path.join(GEN, "kernelsrc")):
        for fn in sorted(fnames):
            if fn.endswith((".h", ".c", ".S")):
                files.append(os.path.join(dirpath, fn))
    files.sort()

    for path in files:
        rel = os.path.relpath(path, GEN).replace("\\", "/")
        with open(path, "r", encoding="utf-8") as f:
            lines = f.read().split("\n")

        items, file_desc = [], []
        i = 0
        while i < len(lines):
            if lines[i].strip().startswith("/**"):
                block = []
                while i < len(lines):
                    block.append(lines[i])
                    if "*/" in lines[i]:
                        break
                    i += 1
                doc = clean_comment(block)
                decl = collect_decl(lines, i + 1)
                if decl and ("(" in decl or decl.startswith("#define") or decl.startswith("typedef")):
                    items.append((decl, doc))
                else:
                    file_desc.extend(doc)
            i += 1

        if not items:
            continue
        anchor = rel.replace("/", "").replace(".", "").lower()
        index.append("- [%s](#%s)（%d 项）" % (rel, anchor, len(items)))
        buf = [u"## %s" % rel, u""]
        if file_desc:
            buf.extend(file_desc[:4] + [u""])
        for decl, doc in items:
            buf.append(u"### `%s`" % decl[:70])
            buf.append(u"")
            buf.append(u"```c")
            buf.append(decl)
            buf.append(u"```")
            buf.append(u"")
            if doc:
                buf.extend(doc + [u""])
        sections.append("\n".join(buf))

    out.extend([u"## 目录", u""] + index + [u"", u"---", u""] + sections)
    with open(OUT_MD, "w", encoding="utf-8") as f:
        f.write("\n".join(out))
    print("[2/2] 已生成 Markdown API 参考: %s" % os.path.relpath(OUT_MD, ROOT))


# --------------------------------------------------------------- Doxygen
def gen_temp_doxyfile():
    """基于仓库根的 Doxyfile 生成临时配置（绝对路径 + UTF-8 输入）"""
    src = os.path.join(ROOT, "Doxyfile")
    lines = []
    if os.path.isfile(src):
        with open(src, "r", encoding="utf-8", errors="replace") as f:
            lines = f.read().split("\n")
    keys = ("INPUT ", "OUTPUT_DIRECTORY ", "USE_MDFILE_AS_MAINPAGE ", "INPUT_ENCODING ")
    lines = [l for l in lines if not l.startswith(keys)]
    lines += [
        "",
        "# ---- 以下由 gen_api_doc.py 自动追加（UTF-8 临时树）----",
        "INPUT                  = %s" % GEN.replace("\\", "/"),
        "OUTPUT_DIRECTORY       = %s" % OUT_DIR.replace("\\", "/"),
        "USE_MDFILE_AS_MAINPAGE = %s" % os.path.join(GEN, "readme.md").replace("\\", "/"),
        "INPUT_ENCODING         = UTF-8",
    ]
    with open(TEMP_DOXYFILE, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    return TEMP_DOXYFILE


def run_doxygen():
    exe = shutil.which("doxygen")
    if not exe:
        print("未检测到 Doxygen，改用内置 Markdown 生成器")
        return False
    cfg = gen_temp_doxyfile()
    r = subprocess.call([exe, cfg], cwd=GEN)
    if r == 0:
        print("[2/2] Doxygen 已生成 HTML: %s" % os.path.join("docs", "api", "html", "index.html"))
        return True
    print("Doxygen 返回错误码 %d，改用内置生成器" % r)
    return False


def main():
    os.chdir(ROOT)
    build_utf8_tree()
    html_ok = False
    if "--md" not in sys.argv:
        html_ok = run_doxygen()
    if not html_ok or "--both" in sys.argv:
        gen_markdown()
    print("完成。临时 UTF-8 树位于系统临时目录，可随时删除：%s" % GEN)


if __name__ == "__main__":
    main()
