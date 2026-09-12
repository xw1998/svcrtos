# -*- coding: utf-8 -*-
"""Add the new doc to docs/README.md (index table + directory tree)."""
import sys

README = r"D:/工作/git_project/svcrtos_new/docs/README.md"

t = open(README, 'rb').read().decode('utf-8')
eol = '\r\n' if '\r\n' in t else '\n'

a1 = "| 想知道哪些东西坏了、哪些没接线、验证到哪一层 | [死代码与未接线审计.md](死代码与未接线审计.md)（诚实记录，含每轮改动表） |"
a2 = ("| 装镜像时丢字节/校验失败，或准备打开 MPU 隔离 | "
      "[安装协议与上板标定清单.md](安装协议与上板标定清单.md)（ACK 流控协议 + 上板标定清单） |")

a3 = "├── 死代码与未接线审计.md                    # 【审计】已知缺陷/死代码/未接线清单 + 每轮改动与验证边界"
a4 = ("├── 死代码与未接线审计.md                    # 【审计】已知缺陷/死代码/未接线清单 + 每轮改动与验证边界" + eol +
      "├── 安装协议与上板标定清单.md                # 【协议/清单】.svcapp 传输 ACK 流控 + 打开 MPU 前的上板标定项")

t2 = t.replace(a1, a1 + eol + a2, 1)
assert t2 != t, 'table anchor not found'
t3 = t2.replace(a3, a4, 1)
assert t3 != t2, 'tree anchor not found'

open(README, 'wb').write(t3.encode('utf-8'))
print('OK  docs/README.md updated')
