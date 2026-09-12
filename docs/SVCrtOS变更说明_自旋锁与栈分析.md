# SVCrtOS 变更说明：自旋锁、调度器锁、栈用量分析与 API 文档

日期：2026-09-11
工程路径：`D:\工作\git_project\svcrtos_new`
备份位置：`C:\Users\14905\OneDrive\Documents\lingxi-claw\20260911-13-47-19-445\backup_svcrtos_20260911_155826`（改动前的 kernelsrc + readme）

---

## 1. 本次新增能力

| 能力 | 入口 | 配置开关 | 说明 |
|------|------|---------|------|
| 自旋锁（内核/驱动侧） | `svcrt_spin.h` | `SVCRT_USE_SPINLOCK`（默认 1） | 原子 CAS 实现，支持嵌套、trylock、关中断变体；为 RISC-V / LoongArch / SMP 预留 |
| 调度器锁（用户态临界区） | `svcrt_sched_lock/unlock/count()` | `SVCRT_USE_SCHED_LOCK`（默认 1） | 禁止任务切换、不关中断，等价 RT-Thread `rt_enter_critical` |
| 任务栈峰值用量分析 | `svcrt_task_stack_info(task_id, out3)` | `SVCRT_USE_STACK_USAGE`（默认 1） | 栈填充图案 + 上下文切换最低栈指针，双手段取大值 |
| API 文档生成 | `tools/gen_api_doc.py` + `Doxyfile` | — | 有 Doxygen 出 HTML，无 Doxygen 出 Markdown |

## 2. 改动文件清单

### 新增
- `kernelsrc/include/svcrt_spin.h` — 自旋锁与临界区 API（header-only，无需改工程文件）
- `Doxyfile`、`tools/gen_api_doc.py`、`docs/README.md`、`docs/api/SVCrtOS_API参考.md`

### 修改
- `kernelsrc/include/svcrt_hal.h`：新增端口层原子接口声明（`svcrt_port_atomic_cas` / `svcrt_port_cpu_id` / `svcrt_port_spin_hint`），并统一引入 `svcrt_spin.h`
- `kernelsrc/port/arm/cortex-m{3,4}/svcrt_port.c`：用 `LDREX/STREX` 实现原子 CAS、CPU ID、自旋提示
- `kernelsrc/include/svcrt_config.h`：新增三个配置宏与 `SVCRT_STACK_FILL_PATTERN`
- `kernelsrc/include/svcrt_task.h`：TCB 新增 `stack_peak_low`；新增调度器锁与栈查询内部接口声明
- `kernelsrc/include/svcrt_cfg.h` / `src/svcrt_cfg.c`：任务栈填充与峰值基准初始化
- `kernelsrc/src/svcrt_task.c`：调度器锁实现与全路径落实（tick、切换决策、递交高优先级、阻塞接口防护）、栈用量查询、SVC 0x11 子命令 7~10
- `kernelsrc/include/svcrt_fault.h`：新增故障类型 `SVCRT_FAULT_SCHEDLOCK`
- `kernelsrc/include/svcrt.h`：全部公开 API 补齐 Doxygen 注释并分组，新增 4 个 API 声明
- `kernelsrc/app/oslib.c`、`kernelsrc/sdk/app_sdk/svcrt_oslib.c`：新增 4 个用户态 SVC 封装
- `readme.md`、`kernelsrc/user_manual.md`：新增特性说明、配置项、第 11 章使用指南

> 未改动任何 `.uvprojx`：所有新增内容要么是头文件，要么落在工程已包含的源文件中。

> **后续修订（加载器架构改造）**：文中提到的 `kernelsrc/app/oslib.c` 已作为重复副本删除，
> 用户态 SVC 封装现在只有 `kernelsrc/sdk/app_sdk/svcrt_oslib.c` 一份；
> 本文其余内容作为历史变更记录保留。

## 3. 编码处理说明

内核旧文件为 GBK，近几轮新增模块为 UTF-8。为避免你 Keil 编辑器里的中文注释变乱码，
本次**没有对源文件整体转码**：对旧文件按 GBK 字节级插入新内容，新文件用 UTF-8。
文档生成脚本会在系统临时目录生成 UTF-8 副本，不会污染仓库。

## 4. 验证情况

- 内核全部源文件（`kernelsrc/src/*.c`、`port/arm/cortex-m4/svcrt_port.c`）：
  Arm Compiler 6 语法检查通过（`-Wall` 无告警），MPU 开/关两种配置均通过
- 用户态封装（`kernelsrc/sdk/app_sdk/svcrt_oslib.c`，当时的重复副本 `kernelsrc/app/oslib.c` 现已删除）：
  Arm Compiler 5 编译通过
- 文档生成：`python tools/gen_api_doc.py` 已跑通，产出 94 个条目的 Markdown API 参考

## 5. 建议的验证步骤

1. 用 Keil 全量重编译 `SVCRTOS_TEST` 工程（应与之前一样直接通过）
2. 在任务中调用 `svcrt_task_stack_info()` 打印各任务峰值用量，确认数据合理
3. 构造自旋锁与中断共享数据的用例，验证 `irqsave` 变体行为
4. 在临界区内故意调用一次 `svcrt_task_wait()`，确认故障记录中出现 `SVCRT_FAULT_SCHEDLOCK`

## 6. 已知边界

- 自旋锁目前为单核语义（`svcrt_port_cpu_id()` 恒返回 0）；将来做 SMP 时需把 port 层原子接口与 CPU ID 实现补全，内核与锁代码无需改动
- 调度器锁采用"禁止抢占"语义：临界区内调用阻塞接口会被忽略并记录故障，这一点在手册与头文件中均已注明
- `svcrt_task_stack_info` 不覆盖空闲栈与中断栈（它们不在任务表中）
