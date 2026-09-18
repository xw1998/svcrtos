# SVCrtOS 在 STM32F401RE 上的移植与 trace 验证记录

> 目的：把 SVCrtOS 从 F427 扩展到第二块板（Nucleo-F401RE），并用 `mdkdebug` 的 trace 工具族
> 在**只有 SWD 两线**的条件下，把「能做到什么 / 做不到什么」逐条落到板上实测。
> 结论先行：**内核已在 F401 上跑起来（调度、shell、分区表全部可用），20 个 trace 工具逐个跑过，
> 其中 4 项「看似权威的错答案」已修掉；SWO/ETM 在该板+该探针上确实不可用，工具如实报错而非编数据。**

---

## 1. 目标与范围

| 项 | 内容 |
|---|---|
| 板卡 | STM32 Nucleo-F401RE（STM32F401RETx） |
| 探针 | 板载 CMSIS-DAP（`BIN\CMSIS_AGDI.dll`），**无 TraceOpt 段** |
| 工具链 | Keil MDK 5.41 / UV4，UVSOCK 端口 4823；AC6（armclang） |
| 串口 | USART2 PA2/PA3 AF7 → 板载 VCP（枚举为 COM9，shell 115200） |
| 范围 | 内核 bring-up + shell + trace 工具族上板可用性 |
| 不在范围 | 应用（App）安装闭环、驱动动态加载 |

---

## 2. 移植内容

新增三处（**没有改动 F427 既有目录**）：

| 路径 | 作用 |
|---|---|
| `config/stm32f401/svcrt_partition.h` | 分区/内存布局的**唯一源头**（F401：Flash 512 K / RAM 96 K） |
| `board/stm32f401/` | 板级驱动：`drvuart`(USART2) / `drvled`(PC13) / `drvflash` / `svcrt_board.c` / `svcrt_board_config.h` |
| `example/stm32f401/kernel/SVCRTOS_TEST/` | 可编译工程：`Core/`（CubeMX 生成 + 接线段）、`MDK-ARM/`（uvprojx + startup） |

### 2.1 F401 上的实际分区（板上 `info` 命令实证，非纸面推算）

```
partition ABI v5，hw = 0x40100004
kernel  flash 0x08000000 + 128 K   ram 0x20002000 + 56 K
pool    flash 0x08020000 + 384 K（3 × 128 K 物理单元），unit 1 K，reserve 1
image   ram   0x20010000 + 32 K，block 1 K..8 K，16 slots
```

`.sct` 由 `tools/gen_scatter.py` 从 `config/stm32f401/svcrt_partition.h` 生成，**不手工编辑**。

### 2.2 构建

```bash
# 在 Keil 里打开 example/stm32f401/kernel/SVCRTOS_TEST/MDK-ARM/SVCRTOS_TEST.uvprojx
# 全量重建结果：0 Error / 1 Warning
```

---

## 3. 上板 bring-up

### 3.1 烧录与回读校验

`flash_download`（走 UV4 的 Flash 算法）返回：

```
Erase Done.  Programming Done.  Verify OK.
```

随后主机侧独立复核：解析 `.hex` 得到 1 个段（42156 B @ `0x08000000`），
**回读 Flash 与 HEX 逐字节比对，差异 0 B**。

> 注意：调试器的 `write_mem` 写 Flash 区**不落盘**（无 Flash 算法）；
> 真正落盘的路径是 `flash_download` / Keil 自己的下载。这两条不要混为一谈。

### 3.2 内核在跑的证据

复位 → 进调试 → 运行 4 s：

| 观测量 | 值 |
|---|---|
| `SystemCoreClock` | 0 → 84000000 |
| `g_led_blink_count` | 0 → 2 → 4 |
| `g_cnt_task_ticks` | 0 → 9 → 17 |
| `g_led_phase` | 0 → 1 |

即：时钟树 84 MHz 生效，2 kHz 节拍在跑，两个任务都在被调度。

### 3.3 shell 交互（COM9 @ 115200）

`help` 返回 13 条命令；`info` / `task` / `pool` / `drv` / `app` 输出与分区头完全吻合：

- `task`：4 个任务在用（上限 48）
- `pool`：free 256 K
- `drv` / `app`：空（未装任何驱动/应用，符合预期）

---

## 4. trace 工具族上板结果（20 / 20 逐个跑过）

### 4.1 可用的

| 工具 | 板上结果 |
|---|---|
| `trace_guide` | 各 topic（`swd_limits` / `overview` / `rtt` / `swo`）均有正文 |
| `trace_status` | 正常 |
| `trace_dwt_counters` | `CTRL=0x40000001`、`CYCCNT` 持续递增（如 `0xFB402913`）→ **DWT 可用** |
| `trace_pcsample` | 198 样本 / 6 个不同 PC；主热点 `svcrt_port_wfi` 95.8%，其次 `svcrt_timer_tick_handler` 等 |
| `trace_profile` | 35 样本，落在 `0x8007E52` 等 |
| `trace_scope_start/read/stop` | 可用；返回真实轮询率（≈6 Hz）与丢点计数 |
| `trace_record` | 可用；高频函数 `svcrt_kernel_tick_handler` 能录到事件（`gap_cyc` ≈ 41977 ≈ 0.5 ms） |
| `trace_events` / `trace_clear` | 正常（空缓冲如实返回 0） |
| `trace_instrument` | 拷出 9 个插桩文件（`mdk_trace.c/.h`、RTT 变体、CMake/make 片段等） |
| `trace_rtt_read/write/detach` | 未 attach 时如实报「先 trace_rtt_attach」 |

### 4.2 板上明确不可用的（工具如实拒绝，不编数据）

| 工具 | 返回 | 原因 |
|---|---|---|
| `trace_swo_start` | `ok:false`，`swo-needs-openocd` | SWO 的 TPIU 配置与流转存依赖 OpenOCD；该板探针为 CMSIS-DAP 且**无 SWO 引脚接线** |
| `trace_etm_probe` | `supported:false` | 指令级 trace 需要并行 trace 口（ETM/TPIU），SWD 两线做不到 |
| `trace_eventrec` | `eventrec-symbol-missing`（本工程无 `EventRecorderInfo`） | 目标未链接 Event Recorder 组件 |
| `trace_rtt_find/attach` | 未给 `elf`/`ranges` 时明确报「无法定位控制块」 | 目标固件未含 RTT 控制块 |

### 4.3 已知的量级限制（写进工具返回，供选型参考）

- **`trace_record` 经 UVSOCK 的事件吞吐很低（实测 ≈0.7 事件/s 量级）**，只适合低频函数；
  盯 2000 ms 周期的任务函数时，短窗口天然录不到东西——此时工具会返回 `no_hit` 说明原因，
  **不会**把「没命中」说成「函数没被调用」。
- `trace_scope` 的采样率是**主机侧轮询率**，不代表目标执行周期。
- `flash_download` 之后，已打开的调试会话会失效，需要重新 `enter_debug`。

---

## 5. 本轮在板上撞出并修掉的 4 项「看似权威的错答案」

| # | 现象（板上原始表现） | 根因 | 修法 |
|---|---|---|---|
| 1 | `trace_scope_start(vars="g_cnt_task_ticks")` 报「地址不是数字」 | 文档承诺「只给名字就用 elf 查地址与大小」，`_parse_vars` 没真去查 | 裸名回落 ELF 查符号；查不到就如实列 `invalid` |
| 2 | 采样工具不传 `elf` 时 PC 退化成裸地址 | 只在显式传 `elf` 时才做地址→函数名，忽略会话里已 `set_symbol_file` 的 `.axf` | 新增 `_session_axf()` 回落，显式参数仍优先 |
| 3 | 目标停机时 `trace_record` 静默录到 0 事件仍 `ok` | `resume` 回显成功但目标没真跑 | `resume` 后复核运行态；明确仍在停止则报错并撤断点 |
| 4 | 零事件时断言「读不到 DWT_CYCCNT（该内核没有）」 | 零事件时压根没读过 CYCCNT，把「没测」说成了「没有」 | 分两种情形；零事件改说「无法判定」并指向 `trace_dwt_counters` |

修复落在 `mdkdebug/trace.py`、`mdkdebug/rtrace.py`（mdk_agent 仓库），
配套 mock 测试 `tests/test_batch54.py`（25 项，含不破坏 `test_batch49` H11(r2)「不要编造」措辞的回归）。

### 板上复验结果

| 项 | 复验方式 | 结果 |
|---|---|---|
| #1 | `trace_scope_start(vars="g_cnt_task_ticks")` 不传 elf | 解析出 `addr=536879120 (0x20002B10)`，不再报错 |
| #2 | `trace_pcsample` 不传 elf | 直接给出函数名（`svcrt_port_wfi` 95.83% 等） |
| #3 | 该分支需「resume 回显成功但实际没跑起来」，本轮真机**未复现**该状态 | mock 已覆盖；真机待后续遇到再验 |
| #4 | 400 ms 窗口录 2000 ms 周期函数 → 零事件 | `cyccnt_note` 改为「本轮没有任何命中，无法判定 CYCCNT 是否可用」；同时 `trace_dwt_counters` 证明 `CYCCNT` 正常递增，旧文案确实自相矛盾 |

---

## 6. 未决 / 待排查

1. **`trace_record` 收尾清除断点曾报一次误报**：某次录制返回
   `breakpoints_left: ["0x800499C"]` 并提示「手工清掉」，但紧接着 `list_breakpoints`
   显示 Keil 真实断点表 `real_total = 0`——**断点其实已经撤掉了，是报告成了失败**。
   之后用相同序列（先 `stop`，再连续两次 `trace_record`）**未能复现**；
   直接调 `KeilBackend.set_bp/clear_bp` 也一切正常。
   已排除「陈旧控制台错误串扰」这一猜测（制造一条报错命令后再录，清除仍成功）。
   属「把成功报成失败」，危害可接受但需单独立项排查；**在下结论前不要臆断原因**。
2. 探针无 SWO、无 ETM：若要在 F401 上做函数级无侵入 trace，需要换带 SWO 接线的调试器
   （或用 `trace_instrument` 的 RTT/Event Recorder 插桩路线）。
3. `board/stm32f401` 与 `example/stm32f401` 的编译产物（`.o/.axf/.hex/.map` 等）已被
   `.gitignore` 覆盖，不入库。

---

## 7. 复现步骤

```bash
# 1) 打开工程（保持只有一个 Keil 实例）
#    example/stm32f401/kernel/SVCRTOS_TEST/MDK-ARM/SVCRTOS_TEST.uvprojx
# 2) 通过 mdkdebug 收敛 Keil 并定位符号
#    close_uvision(keep="all") → launch_uvision(project=..., uvsock_port=4823)
#    set_symbol_file(path=".../SVCRTOS_TEST.axf")   # 8860 条行号条目
# 3) 上板
#    flash_download(...)  → "Erase/Programming/Verify OK"
#    reset → enter_debug → run
# 4) trace 矩阵
#    trace_dwt_counters / trace_pcsample / trace_profile /
#    trace_scope_start(vars="g_cnt_task_ticks", duration_s=0.8) → trace_scope_stop /
#    trace_record(funcs="svcrt_kernel_tick_handler", max_ms=3000)
# 5) shell
#    serial_list_ports → monitor_start(COM9, 115200) → serial_write("info")
```

---

*记录时间：2026-09-18。板上结论均来自实测；未复现的现象已在第 6 节如实标注，不替设备下结论。*
