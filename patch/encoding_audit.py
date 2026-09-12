# -*- coding: utf-8 -*-
"""
编码审计：找出"一个文件里混了两种中文编码"的源文件。

背景：项目约定「旧源文件保持 GBK（保证 Keil 里中文注释正常），新增模块用 UTF-8」。
实际执行中，往 GBK 文件里追加中文时若用了 UTF-8，就会产出混合编码文件：
整文件按 GBK 解不开、按 UTF-8 也解不开，Keil 里那段注释显示成乱码。

用法：
    python patch/encoding_audit.py            # 只报告
    python patch/encoding_audit.py --strict   # 有混编文件时以非零码退出（可用于 CI）

判据：
    先看整个文件：
      - 能整体按 GBK 解   -> 旧文件，正常（可能含少量 UTF-8 行，那才是问题）
      - 只能整体按 UTF-8 解 -> 新文件，符合"新增模块用 UTF-8"的约定，正常
      - 两种都解不开       -> 混编或损坏，**问题**
    再看行：一行只可能是
      1) 纯 ASCII                       —— 无所谓
      2) 仅 GBK 可解 / 仅 UTF-8 可解     —— 明确
      3) 两种都可解（UTF-8 的中文序列同时是合法 GBK 双字节）—— 歧义
    在"能整体按 GBK 解"的文件里出现"仅 UTF-8 可解"的行 -> 混编，**问题**。
    歧义行无法靠解码判断归属，这里用全仓"确定 GBK 行"的汉字集合做词频参照，
    给出倾向，供人工确认。
"""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXTS = ('.c', '.h', '.md', '.py', '.s', '.S', '.txt')
SKIP_DIRS = ('.git', 'MDK-ARM', '.gen_utf8', 'html', 'latex', 'xml')


def iter_files():
    for dp, dn, fn in os.walk(ROOT):
        if any(s in dp for s in SKIP_DIRS):
            continue
        for f in fn:
            if f.endswith(EXTS):
                yield os.path.join(dp, f)


def classify(raw):
    try:
        raw.decode('gbk')
        g_ok = True
    except UnicodeDecodeError:
        g_ok = False
    try:
        raw.decode('utf-8')
        u_ok = True
    except UnicodeDecodeError:
        u_ok = False
    return g_ok, u_ok


def line_kind(x):
    """返回 'ascii' / 'gbk' / 'utf8' / 'both' / 'bad'"""
    if not x:
        return 'ascii'
    try:
        x.decode('ascii')
        return 'ascii'
    except UnicodeDecodeError:
        pass
    g = u = False
    try:
        x.decode('gbk')
        g = True
    except UnicodeDecodeError:
        pass
    try:
        x.decode('utf-8')
        u = True
    except UnicodeDecodeError:
        pass
    if g and u:
        return 'both'
    if g:
        return 'gbk'
    if u:
        return 'utf8'
    return 'bad'


def build_reference():
    ref = set()
    for p in iter_files():
        try:
            text = open(p, 'rb').read().decode('gbk')
        except (UnicodeDecodeError, OSError):
            continue
        for ch in text:
            if ord(ch) > 0x7F:
                ref.add(ch)
    return ref


def score(text, ref):
    chars = [c for c in text if ord(c) > 0x7F]
    if not chars:
        return 0.0
    return float(sum(1 for c in chars if c in ref)) / float(len(chars))


def main():
    strict = '--strict' in sys.argv
    only = None
    for a in sys.argv[1:]:
        if a.startswith('--only='):
            only = a.split('=', 1)[1]
    ref = build_reference()
    bad = []
    n_gbk = n_utf8 = 0
    for p in iter_files():
        if only and only not in p:
            continue
        raw = open(p, 'rb').read()
        g_ok, u_ok = classify(raw)
        if g_ok and u_ok:
            continue                       # 纯 ASCII
        if u_ok and not g_ok:
            n_utf8 += 1                    # 整文件 UTF-8：符合新文件约定
            continue
        if g_ok and not u_ok:
            n_gbk += 1                     # 旧 GBK 文件：正常
        if not g_ok and not u_ok:
            bad.append((p, 'NEITHER', []))
            continue

        kinds = {}
        detail = []
        for i, ln in enumerate(raw.split(b'\n')):
            x = ln[:-1] if ln.endswith(b'\r') else ln
            k = line_kind(x)
            kinds[k] = kinds.get(k, 0) + 1
            if k in ('utf8', 'both'):
                t_u = x.decode('utf-8', 'replace')
                t_g = x.decode('gbk', 'replace')
                detail.append((i + 1, k, score(t_u, ref), score(t_g, ref),
                               t_u.strip()[:70]))
        if kinds.get('utf8'):
            bad.append((p, 'MIXED', detail))

    print('编码审计：GBK 整文件 %d 个，UTF-8 整文件 %d 个' % (n_gbk, n_utf8))
    if not bad:
        print('          未发现混合编码 / 无法解码的文件')
    for p, tag, detail in bad:
        print('[%s] %s' % (tag, os.path.relpath(p, ROOT)))
        for lno, k, su, sg, txt in detail:
            print('    L%-5d %-5s utf8分=%.2f gbk分=%.2f | %s'
                  % (lno, k, su, sg, txt))
    if strict and bad:
        sys.exit(1)


if __name__ == '__main__':
    main()
