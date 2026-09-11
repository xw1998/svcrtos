# SVCrtOS 文档目录

本目录存放自动生成的 API 文档，以及生成脚本的使用说明。

## 目录结构

```
docs/
├── README.md                          # 本文件
├── SVCrtOS变更说明_自旋锁与栈分析.md      # 自旋锁/调度器锁/栈用量/文档生成 的变更说明
└── api/
    ├── SVCrtOS_API参考.md               # 内置生成器输出的 Markdown API 参考（已生成）
    └── html/                          # Doxygen 输出的 HTML（需本机安装 Doxygen）
```

## 生成方式

```bash
# 默认：检测到 Doxygen 就生成 HTML，否则生成 Markdown
python tools/gen_api_doc.py

# 强制生成 Markdown（无需 Doxygen）
python tools/gen_api_doc.py --md

# 两者都要
python tools/gen_api_doc.py --both
```

## 为什么需要脚本而不是直接跑 Doxygen

内核源码（`kernelsrc/`）中的注释存在 **GBK 与 UTF-8 混用**（历史文件为 GBK，
新增模块为 UTF-8），而 `*.md` 文档为 UTF-8。Doxygen 的 `INPUT_ENCODING` 是全局
设置，直接扫描源码会让其中一部分中文注释变成乱码。

`tools/gen_api_doc.py` 的做法是：

1. 逐行尝试 UTF-8 → GBK 解码，把源码复制成一份 UTF-8 临时树（系统临时目录，不改动仓库）；
2. 基于根目录 `Doxyfile` 生成临时配置（输入指向 UTF-8 树、编码设为 UTF-8）；
3. 调用 Doxygen，或在内置 Markdown 生成器中解析注释并输出 API 参考。

## 安装 Doxygen（可选）

- Windows：`choco install doxygen.install` 或从 <https://www.doxygen.nl/download.html> 下载安装包
- 安装后确保 `doxygen` 在 PATH 中，重新运行 `python tools/gen_api_doc.py` 即可

## 注释规范

新增接口请沿用现有 Doxygen 风格，至少包含：

```c
/**
* @brief 一句话说明接口用途
* @param x 参数说明
* @return 返回值说明
* @note 注意事项（可选）
*/
```

公开 API（`kernelsrc/include/svcrt.h`）建议同时使用 `@defgroup` 归类，
以便生成的 HTML 按模块组织。
