# SVCrtOS 文档目录

## 先读哪一份

| 我想…… | 看这份 |
|---|---|
| 把 App/驱动调试起来、装到板子上、处理崩溃 | **[SVCrtOS应用安装与调试指南.md](SVCrtOS应用安装与调试指南.md)**（操作手册，先读这份） |
| 搞懂安装策略、设备端配置区、槽位与 RAM 窗口 | [配置区与安装策略.md](配置区与安装策略.md)（配置记录格式 + 两种安装策略） |
| 把为 Linux/Windows 写的 C 程序搬到 SVCrtOS 上跑 | [POSIX与Windows兼容层说明.md](POSIX与Windows兼容层说明.md)（能力表 + 不支持项及原因） |
| 查调度策略、这一轮调度器改了什么、还剩什么 | [调度器说明.md](调度器说明.md)（O(1) 快速路径的等价性论证 + 未做项） |
| 用串口控制台看状态、启停 App、改配置 | [内核Shell控制台使用说明.md](内核Shell控制台使用说明.md) |
| 搞懂分区/加载器/镜像格式为什么这么设计 | [Loader工程化落地说明.md](Loader工程化落地说明.md)（设计说明与变更记录） |
| 查自旋锁、调度器锁、栈用量分析的用法 | [SVCrtOS变更说明_自旋锁与栈分析.md](SVCrtOS变更说明_自旋锁与栈分析.md) |
| 查 API 签名与参数 | [api/SVCrtOS_API参考.md](api/SVCrtOS_API参考.md) |
| 想知道哪些东西坏了、哪些没接线、验证到哪一层 | [死代码与未接线审计.md](死代码与未接线审计.md)（诚实记录，含每轮改动表） |
| 装镜像时丢字节/校验失败，或准备打开 MPU 隔离 | [安装协议与上板标定清单.md](安装协议与上板标定清单.md)（ACK 流控协议 + 上板标定清单） |
| 在 F401 上跑起来、或想查 SWD 两线到底能 trace 到什么 | [F401移植与trace验证记录.md](F401移植与trace验证记录.md)（移植步骤 + 20 个 trace 工具上板实测） |

## 目录结构

```
docs/
├── README.md                              # 本文件（文档索引 + API 文档生成说明）
├── SVCrtOS应用安装与调试指南.md             # 【操作手册】开发调试/串口安装/启动流程/故障处理/排错
├── 配置区与安装策略.md                      # 【设计说明】分区/池粒度/槽位与 RAM 窗口/配置记录格式/安装策略
├── POSIX与Windows兼容层说明.md              # 【设计说明】兼容层能力与边界、启动门闩、堆、真机自测
├── 调度器说明.md                            # 【设计说明】调度模型、P4b 两项瘦身与等价性、剩余优化空间
├── 内核Shell控制台使用说明.md               # 【操作手册】console 命令（info/app/drv/task/fault/install/log/pool/trace/cfg）
├── Loader工程化落地说明.md                 # 【设计说明】分区配置、镜像格式、Loader、安装任务、安全分层
├── SVCrtOS变更说明_自旋锁与栈分析.md        # 【变更说明】自旋锁/调度器锁/栈用量/文档生成
├── 死代码与未接线审计.md                    # 【审计】已知缺陷/死代码/未接线清单 + 每轮改动与验证边界
├── 安装协议与上板标定清单.md                # 【协议/清单】.svcapp 传输 ACK 流控 + 打开 MPU 前的上板标定项
├── F401移植与trace验证记录.md               # 【验证记录】F401 移植 + trace 工具族上板结果与已知边界
└── api/
    ├── SVCrtOS_API参考.md                 # 内置生成器输出的 Markdown API 参考（已生成）
    └── html/                            # Doxygen 输出的 HTML（需本机安装 Doxygen）
```

下面几节介绍 API 文档的生成方式。

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
