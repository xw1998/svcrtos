# -*- coding: utf-8 -*-
"""
修复 17：混合编码文件统一为 GBK（svcrt_mq.c / svcrt_mq.h）

事实：
  kernelsrc/src/svcrt_mq.c 与 kernelsrc/include/svcrt_mq.h 既不能整体按 GBK
  解码、也不能整体按 UTF-8 解码。逐行判定后是"大部分行 GBK，少数行 UTF-8"
  ——后续几轮补丁往这两个文件里加中文注释时用了 UTF-8，而文件原本是 GBK。

后果：
  Keil 按本地代码页（中文 Windows = GBK）读源码，被掺进去的 UTF-8 中文显示成
  乱码；如果哪天有人"顺手整体转成 UTF-8"，原有的 GBK 注释才会真的坏掉。

难点：
  有一部分行**两种编码都解得出**（UTF-8 的中文序列恰好也是合法 GBK 双字节），
  只按"能不能解码"判断会漏判。这里用词频打分消歧：
  先统计全仓"确定是 GBK 的行"里出现过的汉字集合 R 作为参照，
  歧义行分别按两种方式解码，取"落在 R 里的字符比例"更高的那一种；
  打平则保持 GBK（保守）。

修法：
  只把判定为 UTF-8 的行转成 GBK，其余行原字节不动，最后校验整文件可按 GBK 解开。
"""
import os
import shutil

ROOT = r'D:\工作\git_project\svcrtos_new'
BK = r'C:\Users\14905\OneDrive\Documents\lingxi-claw\20260911-13-47-19-445\backup_fix8'

TARGETS = ['kernelsrc/src/svcrt_mq.c', 'kernelsrc/include/svcrt_mq.h']
EXTS = ('.c', '.h', '.md', '.py', '.s', '.S', '.txt')
SKIP_DIRS = ('.git', 'MDK-ARM', '.gen_utf8')


def collect_reference():
    """全仓「确定是 GBK」的行里出现过的非 ASCII 字符集合，作为词频参照。"""
    ref = set()
    for dp, dn, fn in os.walk(ROOT):
        if any(s in dp for s in SKIP_DIRS):
            continue
        for f in fn:
            if not f.endswith(EXTS):
                continue
            p = os.path.join(dp, f)
            try:
                raw = open(p, 'rb').read()
                text = raw.decode('gbk')        # 整体是 GBK 的文件
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
    hit = sum(1 for c in chars if c in ref)
    return float(hit) / float(len(chars))


def fix_file(rel, ref, verbose=False):
    p = os.path.join(ROOT, rel.replace('/', os.sep))
    shutil.copy2(p, os.path.join(BK, rel.replace('/', '__')))
    raw = open(p, 'rb').read()
    out = []
    conv_lines = []
    for idx, ln in enumerate(raw.split(b'\n')):
        cr = ln.endswith(b'\r')
        x = ln[:-1] if cr else ln
        keep = True
        if x:
            try:
                x.decode('ascii')
            except UnicodeDecodeError:
                g_ok = u_ok = False
                try:
                    x.decode('gbk')
                    g_ok = True
                except UnicodeDecodeError:
                    pass
                try:
                    x.decode('utf-8')
                    u_ok = True
                except UnicodeDecodeError:
                    pass

                if not g_ok and u_ok:
                    keep = False                       # 只 UTF-8 能解
                elif g_ok and u_ok:
                    # 歧义行：按词频参照消歧
                    t_u = x.decode('utf-8')
                    t_g = x.decode('gbk')
                    if score(t_u, ref) > score(t_g, ref):
                        keep = False
                        if verbose:
                            conv_lines.append((idx + 1, 'both->utf8',
                                               score(t_u, ref), score(t_g, ref),
                                               t_u.strip()[:70]))
                    elif verbose:
                        conv_lines.append((idx + 1, 'both->gbk',
                                           score(t_u, ref), score(t_g, ref),
                                           t_g.strip()[:70]))
                elif g_ok and not u_ok:
                    keep = True
                else:
                    raise SystemExit('%s L%d 两种编码都解不开' % (rel, idx + 1))

        if keep:
            out.append(ln)
        else:
            out.append(x.decode('utf-8').encode('gbk') + (b'\r' if cr else b''))

    new = b'\n'.join(out)
    new.decode('gbk')                                  # 整文件 GBK 校验
    open(p, 'wb').write(new)
    print('%s: 转成 GBK 的行（含歧义行判定）' % rel)
    for lno, how, su, sg, txt in conv_lines:
        print('   L%-4d %-12s utf8分=%.2f gbk分=%.2f | %s' % (lno, how, su, sg, txt))
    return len(conv_lines)


def main():
    os.makedirs(BK, exist_ok=True)
    ref = collect_reference()
    print('词频参照集合：%d 个非 ASCII 字符\n' % len(ref))
    total = 0
    for rel in TARGETS:
        total += fix_file(rel, ref, verbose=True)
    print('\n合计处理 %d 行；两个文件现已可按 GBK 整体解码' % total)


if __name__ == '__main__':
    main()
