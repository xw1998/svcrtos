# SVCrtOS 文档目录

## 先读哪一份

| 我想…… | 看这份 |
|---|---|
| 把 App/驱动调试起来、装到板子上、处理崩溃 | **[SVCrtOS应用安装与调试指南.md](SVCrtOS应用安装与调试指南.md)**（操作手册，先读这份） |
| 搞懂安装策略、设备端配置区、槽位与 RAM 窗口 | [配置区与安装策略.md](配置区与安装策略.md)（配置记录格式 + 两种安装策略） |
| 把为 Linux/Windows 写的 C 程序搬到 SVCrtOS 上跑 | [POSIX与Windows兼容层说明.md](POSIX与Windows兼容层说明.md)（能力表 + 不支持项及原因） |
| 查调度策略、这一轮调度器改了什么、还剩什么 | [调度器说明.md](调度器说明.md)（位图+链的 O(1) 就绪集、三种调度触发点、实测代价账与未做项） |
| 看调度器在真机上长什么样（图） | 根目录 [`README.md`](../README.md) §调度表现（4.97 s trace 实测图，图存 `docs/img/`） |
| 用串口控制台看状态、启停 App、改配置 | [内核Shell控制台使用说明.md](内核Shell控制台使用说明.md) |
| 不想敲命令行，要图形界面 | `python tools/svcrt_host_gui.py`（连接 / 控制台 / 安装 / 布局配置四个页签） |
| 让 AI 代理直接驱动这块板子 | [`../skills/svcrtos/SKILL.md`](../skills/svcrtos/SKILL.md) 与《SVCrtOS应用安装与调试指南.md》§11 |
| 想在线把配置记录写进设备（不拆板、不用烧录器） | [配置区与安装策略.md](配置区与安装策略.md) §6.3（`tools/svcrt_cfg.py`）与 §6.5（真机验收结论） |
| 想把板子上跑的过程录成 trace 并画成网页 | [`../skills/svcrtos/SKILL.md`](../skills/svcrtos/SKILL.md) §4.1（武装 / 采集 / 切段 / 渲染） |
| 搞懂同步原语怎么写、令牌为什么不会丢、`syncinfo` 怎么读 | [同步原语与令牌守恒.md](同步原语与令牌守恒.md)（三条契约 + 返回值/唤醒原因 + 排障口径） |
| 加一个"放在文件系统里、想跑就跑"的小程序（不安装、不占槽位） | [小程序设计.md](小程序设计.md)（两块内存模型 + 装载六步 + 体积上限 + 已知限制） |
| 一个镜像反复崩溃会怎样、装镜像前内核怎么把关 | [崩溃恢复与镜像版本把关.md](崩溃恢复与镜像版本把关.md)（跨复位崩溃日记 + 版本三态判定 + 明确不做签名/回滚） |
| 查 API 签名与参数 | [api/SVCrtOS_API参考.md](api/SVCrtOS_API参考.md) |

## 目录结构

```
docs/
├── README.md                              # 本文件（文档索引 + API 文档生成说明）
├── SVCrtOS应用安装与调试指南.md             # 【操作手册】开发调试/串口安装/启动流程/故障处理/排错
├── 配置区与安装策略.md                      # 【设计说明】分区/池粒度/槽位与 RAM 窗口/配置记录格式/安装策略
├── POSIX与Windows兼容层说明.md              # 【设计说明】兼容层能力与边界、启动门闩、堆、真机自测
├── 调度器说明.md                            # 【设计说明】调度模型、O(1) 就绪集/延时链/时间片、三种触发点、实测与未做项
├── 同步原语与令牌守恒.md                    # 【设计说明】为何用户态等待只登记+轮询、派发方 peek(only_kernel)、三条契约、四个失败根因
├── 小程序设计.md                           # 【设计说明】文件系统里的第三种用户态应用：两块(代码/ RAM)内存模型、装载六步、体积上限、与 ELF 的对照
├── 崩溃恢复与镜像版本把关.md                 # 【设计说明】B 项崩溃终局与跨复位计数、C 项版本三态判定；明确不做签名与回滚
├── 内核Shell控制台使用说明.md               # 【操作手册】console 命令（info/app/drv/task/sched/fault/install/log/pool/trace/cfg）
├── img/                                   # 【实测图】根 README §调度表现 引用（必须入库，勿被忽略规则吞掉）
│   ├── sched-overview.png                 # 整体调度时间线 + 切换间隔（4.97 s 窗口）
│   ├── sched-switch-detail.png            # 单次切换细节 + App 任务被调度进来跑 72.7 µs
│   └── sched-stats.png                    # 间隔/PendSV 分布 + 优化 A/B（插桩关口径）
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
