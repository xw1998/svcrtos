# F427 运行期 trace 采集与可视化

**一句话**：在只接 SWD 两线（无 SWO）的条件下，用 `mdk_agent_mcp` 的
`trace_swd_*` 三件套把 SVCrtOS 的调度/中断事件录下来，再渲染成离线可开的单文件网页。

- 采集对象：F427 板（STM32F427VGTx，USART1 PA9/PA10 → COM3，115200 8N1），
  内核 + 外部 App（APP_DEMO）+ 外部驱动（BLED_DRV）三个镜像同时在跑。
- 采集工具：`mdk_agent_mcp` <https://gitee.com/xw19981010/mdk_agent_mcp.git>
- 记录时间：2026-09-19
- 实测产物：2466 事件 / 376.014 ms / 378 次上下文切换 / 8 个丢失断口

这份文档只写**已经上板验证过**的做法与数值；没验证过的写在最后一节。

---

## 1. 前提：trace 必须在设备上先"武装"

这是最容易白跑一趟的地方。

**SVCrtOS 的 trace 不在启动时初始化**。`svcrt_trace_init()` 只由设备 shell 的
`trace start` 命令调用。没执行过 `trace start` 之前：

- 控制块整片是 `0x00`；
- 采集工具会报 `swd-read-degenerate`（这是工具的正确行为，不是它坏了）。

所以采集的第一步永远是：

```
trace start        # 或 trace start <granularity>
```

武装之后 `trace` 命令会打印一行状态（ARMED / seq / cap / events / tokens / pending / lost），
拿它确认再开始录。

---

## 2. 采集链路（`mdk_agent_mcp`）

### 2.1 三件套

| 工具 | 作用 |
|---|---|
| `trace_swd_status` | 看控制块：是否 ARMED、seq、环容量、事件计数、背压丢事件数 |
| `trace_swd_reset` | 复位/重新武装，可切 `granularity`（`cycle` / `500us` / `1ms` / `2.5us` / `none`） |
| `trace_swd_read` | 把设备环里的事件搬回主机并解码；`out_file=` 直接落盘 |

### 2.2 一轮采集的标准动作

```
enter_debug                     # 进调试态（content-confirmed），顺带走 watchdog freeze
trace_swd_reset                 # seq_before 会 +1；applied 可能是 false 而 request_latched=true
                                #  —— 复位在目标下次跑起来时才生效
run_timeout(400)                # 让目标跑 400 ms（actual_run_ms 会回真实值）
trace_swd_read(out_file=...)    # 把这一段搬回来
```

要点：

1. **`stop` 之后跟的第一次 `read_mem` 可能读到全 0 脏帧**（UVSOCK 的已知行为），
   要重读复核；trace 读取同理，别拿第一次的结果下结论。
2. **`trace_swd_reset` 的 `applied=false` 不代表失败**，看 `request_latched=true`——
   它在目标恢复运行时落地。
3. **环满不覆盖，是背压丢事件**。所以 `meta.warnings` 里的
   `lost_events=N：目标因为宿主跟不上丢了事件` 是权威计数，
   一旦非 0 这条时间线就不是完整的，渲染出来的"断口"就是它。
4. 采集期间**别再进 halt 空转**：每多一段 halt 间隙，就多一次时间基不连续。

### 2.3 参数与坑

- `view_render` 的参数是 `data` / `data_file` / `max_events` / `names` / `out` /
  `subtitle` / `title` / `top` / `view`——**没有 `out_file`**（那是采集工具的）。
- 采集结果先 `out_file=` 落盘，再 `view_render(data_file=..., out=..., title=..., subtitle=...)`，
  不要试图把几 MB 的 JSON 直接塞进一次调用。
- MCP 服务 `idle_timeout=30s` 一重启就回落到 core 42 个工具，`trace_swd_*` 会消失。
  需要时先 `toolset(action="load", toolsets="all")` 把工具面装回来。

---

## 3. 事件语义（怎么把 id/arg 读成人话）

权威来源：`kernelsrc/include/svcrt_trace.h`。

### 3.1 事件类型（`type`）

| 值 | 名字 | 含义 |
|---|---|---|
| 0x01 | MARK | 用户标记 |
| 0x10 | SWITCH | 任务切换（载荷是 from→to） |
| 0x11 | WAIT | 任务进入等待（载荷是等待对象 id） |
| 0x12 | READY | 任务就绪 |
| 0x13 | CREATE | 任务创建 |
| 0x14 | EXIT | 任务退出 |
| 0x15 | TICK | 节拍 |
| 0x20 / 0x21 | ISR_ENTER / ISR_EXIT | 中断进入/退出 |
| 0x30 | SVC | SVC 调用 |
| 0xFF | EXT | 扩展 |

中断源 id：`SVCRT_TR_IRQ_PENDSV = 14`、`SYSTICK = 15`、`SVC = 11`。
任务 id 里的 `0x0F`（15）是 **IDLE**。

### 3.2 插桩点在哪

| 位置 | 插了什么 |
|---|---|
| `kernelsrc/port/arm/cortex-m4/svcrt_context.S` | `PendSV_Handler` 入口记 `isr(14, ENTER)`，切换完成后记 `isr(14, EXIT)`；`endpend`（进了 PendSV 但没切换）也记一次 EXIT |
| `kernelsrc/src/svcrt_task.c`（371 / 928 行附近） | SVC 入口/出口 记 `isr(11, ENTER/EXIT)` |
| `kernelsrc/src/svcrt_task.c`（1062 行附近） | 切换时记 `switch(from, to)` |

**F427 板的 `SysTick_Handler` 没有 trace 插桩**（只有 `board/stm32f401/svcrt_board.c` 有）。
所以 F427 的时间轴靠 PendSV 事件与 CPU 周期时间戳还原，看不到 500 µs 节拍点。

### 3.3 `from` / `to` 是 0 基任务表下标

`svcrt_task.c` 传的是 `(cur > 0) ? cur - 1 : 15` 与 `(new > 0) ? new - 1 : 15`，
也就是：

- trace 里的 **15 = idle**；
- 其余数字是 **0 基的任务表下标**，与 shell `task` 命令输出的 `id` 列同一套编号；
- 与分区表 / `app list` 的槽号（`app start <n>` 那个 `n`）**不是一回事**。

**而且这套编号跨复位会重排**（任务按创建顺序占位）。实测：采集那次是
`app = task4 / drv = task5`，本轮的 `task` 表却是 `drv = task4 / app = task5`。
所以**绝不要把 trace 里的数字钉死成某个 App**——要认任务，去 `task` 表比
`entry` 地址落在哪个镜像/内核段。

---

## 4. 把原始数据渲染成网页

```
trace_swd_read(out_file="trace_r01.json")          # 原始落盘
# 按最后一次 sync 切段（见 §6 第 2 条），只留同一条时间基的窗口
view_render(data_file="trace_window.json",
            out="svcrtos_trace.html",
            title="SVCrtOS F427 运行期 trace",
            subtitle="<说明这一段是什么>")
```

产物是**单文件、离线可开**的 HTML（本轮 79.1 KB），含时间线轨道、
上下文切换序列、丢失断口等视图。用普通浏览器直接打开即可，不需要装任何东西。

---

## 5. 实测结果（2026-09-19）

采集配置：F427 + APP_DEMO + BLED_DRV 同时运行；`trace start` 已武装；
粒度 = 最小粒度（一个 dt 单位 = 一个 CPU 周期，96 MHz → 10.4 ns）。

单段窗口（最后一次 sync 之后、时间基一致的连续段）：

| 指标 | 值 |
|---|---|
| 事件数 | 2466 |
| 时长 | 376.014 ms |
| 类型分布 | `isr` 1890 / `sched` 378 / `event` 189 / `sync` 1 / `gap` 8 |
| 上下文切换 | 378 次：`(15→5)` 188、`(5→15)` 188、`(15→4)` 1、`(4→5)` 1 |
| 等待事件 | 对象 5 共 188 次、对象 4 共 1 次 |
| 中断 | `isr 14`（PendSV）enter 942 / exit 942；`isr 11`（SVC）enter 3 / exit 3 |
| 背压丢事件 | 会话累计 407（`meta.warnings` 报出），窗口内留下 8 个断口 |
| 传输开销 | 3.32 字节/事件（会话口径 3.33） |

可以读出的事实：

- 这是一个**以 idle 为背景、被一个高频任务周期性唤醒**的系统：
  376 ms 里 `idle → idx5 → idle` 这一对走了 188 个来回，折算 500 Hz
  （对应 shell 里 `task` 显示的 `period 4`，即 4 个 tick = 2 ms）。
- `idx5` 的 `entry` 落在内核 Flash 段（`0x0800AF99`，prio 14），**是内核任务**，
  不对应任何一个外部镜像。
- `idx4` 在 376 ms 里只出现 1 次，属外部模块任务；数字编号跨复位会重排，
  这一条**只在本次会话内成立**。
- PendSV 入口 942 次但真正切换只有 378 次：多数 PendSV 是空转
  （`endpend` 路径）。这只是一个线索，**不构成缺陷结论**。

---

## 6. 三条必须记住的硬结论

1. **先武装再录**：没执行 `trace start` 之前，控制块整片 0，工具报
   `swd-read-degenerate` 是正确判据而不是故障。
2. **多轮采集必然混段，渲染前必须切段**：每停一次再跑，目标就会重开录制段
   （`seq` 递增），新旧段**不在同一条时间基上**。工具会在 `meta.notes` 里明确警告，
   但**不会替你切**。不切段直接渲染，会得到"34.265 s 长的时间轴，数据全挤在最后 0.4 s"
   这种看着像真图的误导图。做法：只保留**最后一次 `sync` 之后**的事件。
3. **任务编号不能跨复位硬编码**：trace 的 id/arg 是 0 基任务表下标，
   任务表下标随创建顺序变化。要确认"这是谁"，用 `task` 表的 `entry` 地址去比对。

配套的两条操作习惯：

- 环满**丢事件不覆盖**，`lost_events` 才是真相；有断口就在图上标出来，不要平滑掉。
- halt 会拉长墙钟但不会产生事件，时间轴只信事件自带的周期戳。

---

## 7. 已知工具链问题（`mdk_agent_mcp` 侧）

本轮实测暴露、**尚未在工具侧修复**的 5 条，记录在此以免误导后人：

| # | 问题 | 影响 |
|---|---|---|
| 1 | 工具面不跨进程保留（服务空闲重启回落 core 42） | `trace_swd_*` 会突然"不存在"；需 `toolset(load, all)` 或把 `MDKDEBUG_TOOLSETS=all` 写进连接器命令 |
| 2 | `serial_expect` 的超时提示里写死 `port="COM9"` | 本机实际是 COM3，照提示走会失败 |
| 3 | `trace_swd_read` 直接返回混段数据 | 调用方必须自己按 `sync` 切段（§6 第 2 条） |
| 4 | 渲染页徽章在数据为空时显示"事件 0" | 与"渲染失败"不可区分，容易把空图读成"什么都没发生" |
| 5 | `view_guide` 未提"F427 上 trace 需要先 `trace start` 武装" | 第一次用必然白跑一趟 |

（问题 2、3 的根因在工具侧；问题 1 建议用环境变量写死。）

---

## 8. LED 现象对照

板载三色灯与内核/镜像的对应关系（`board/stm32f427/svcrt_board.c` + 内核 `main.c`）：

| 灯 | 注册名 | 引脚 | 谁在驱动 |
|---|---|---|---|
| 红 | `LED` | PC0（`GPIO_PIN_0`） | 内核自带的 1 Hz 闪烁任务 |
| 绿 | `LED2` | PC1（`GPIO_PIN_1`） | 内核自带的 1 Hz 闪烁任务（与红**反相**，视觉像红黄交替） |
| 蓝 | `LED3` | PC2（`GPIO_PIN_2`） | APP_DEMO 心跳（约 4 次/秒） |

所以：**只看到红/绿在闪、蓝不动 = 镜像池里没有正在运行的 App**，
内核本身是好的。APP_DEMO 装回并 RUNNING 之后蓝灯就会恢复。

外部驱动 BLED_DRV 的 `DrvMain` 是 `while (1) { svcrt_task_wait(1000); }`，
**它自己不动灯**，只是注册设备名 `BLED` 并占用 GPIOC PC2——所以
"装了驱动但灯没变"是预期行为，别当成驱动没起来。

---

## 9. 未验证 / 未做

- **只在 F427 上验证过**。F401 的 trace 情况见
  [F401移植与trace验证记录.md](F401移植与trace验证记录.md)。
- 未做 SWO（板上没接 SWO 线），本文所有结论都建立在 SWD 两线的录-读方案上。
- `[POOL:415] free 639624 B (largest run 639044 B)` 与 shell `info` 的
  `free 624K`（= 638976 B）差 648 字节，**两种口径的差异未查清**，此处并列记录，
  不作为缺陷宣称。
- `APP_TEST shell.register ... FAIL (-2)` 只在首次启动出现、复位复跑即通过，
  怀疑与上一个 App 生命周期残留的 shell 命令注册未清理有关，**未定论**。

### 7.1 第八轮补测新增（2026-09-19）

前四条都是"会把人带偏的错答案"，用之前先知道：

1. **`read_peripheral` 在目标运行中返回伪影值，而且不给任何提示。**
   目标运行中连续读 `GPIOC`，拿到 `MODER=0x017C2EB0`、`OTYPER=0x017C354E`、`OSPEEDR=0x00000000`、
   `PUPDR=0x017C4207`、`IDR=0x017C485B`、`ODR=0x017C4E9E`、`BSRR=0x017C54F2`、`LCKR=0x017C5BC1`、
   `AFRL=0x017C6215`——这些值**随寄存器地址单调递增**（步长 0x69E），一眼就是地址伪影，不是寄存器内容。
   `stop` 之后同一调用立刻返回完全自洽的值（`MODER=0x15`、`OTYPER=0`、`PUPDR=0x15`、`ODR=0x00000006`）。
   对比 `read_registers`：它在目标运行时会明确返回 `pc_confidence:"low"` / `halt_verified:false` /
   `target_running:true`，把通用寄存器列进 `unavailable`，并给出"读到的 PC 是上一次 halt 的残留值"警告——
   `read_peripheral` 缺这道保险。**结论：读外设寄存器前先 `stop`；运行中的 `read_peripheral` 结果一律不可信。**

2. **`uvprojx_read` 的 `include_path` 取错了元素。**
   本工程 `.uvprojx` 里 `<IncludePath>` 共出现 18 次：第 40 行那次属于 `TargetCommonOption`（环境路径字段，本来就空），
   真正的编译器搜索路径在第 343 行 `<Cads><VariousControls>` 里，共 **11 条**
   （`kernelsrc/components/mdk_trace`、`../Core/Inc`、四个 HAL/CMSIS Inc、`kernelsrc/include`、
   `board/stm32f427`、`config`、`kernelsrc/shell`、`kernelsrc/shell/ark_shell`）。
   工具返回了前者，于是 `include_path: ""`。照它排查会得出"这工程没有包含路径"的错误结论。
   同一个调用里的 `define` / `misc_controls` 是对的（`-DMDK_TRACE_SWD_EXTERNAL_PLATFORM -DMDK_TRACE_USE_CONFIG_FILE`）。

3. **`toolchain_detect_project` 对 mdk 工程返回 `ok:false`，但同时给出 `kind:"mdk"` 与 evidence。**
   mdk 是它自己文档里列明的合法识别结果之一，`ok:false` 很容易被读成"识别失败"。看 `kind`，别看 `ok`。

4. **`toolchain_size` 在本机装了 `arm-none-eabi-size` 的情况下仍退化为 pyelftools**
   （只给 `flash: 90732`，没有 RAM 细分），它自带 note 说明了退化，属诚实降级；
   但"发现 size"的路径比 `toolchain_list` 弱——后者能列出 arm-none-eabi 全套工具。
   同一份 `.axf` 用 `toolchain_elf_info` 读是对的（EM_ARM / entry `0x080001AD` / 5 个 alloc section / 2304 符号）。

### 7.2 build 组实测（通过）

- `build_project` 在 Keil **处于调试态、设备正在跑**的情况下仍能正常构建：
  `exit_code 0`（0 Error / 0 Warning），`Program Size: Code=48774 RO-data=3638 RW-data=280 ZI-data=38040`，
  构建前后 `keil` 自检均为 "Keil 与 UVSOCK 均就绪"，`ensure_debug_channel: true`。
  Before-Build 钩子 `py -3 tools/gen_scatter.py --target kernel` 正常触发。
- `parse_build_errors` 正确解析 AC5 的 `file(line)` 格式（`svcrt_context.S(0): warning: A1581W`，
  `compiler:"AC5"` / `format:"file(line)"`），并给出把 message 交给 `explain_build_error` 的提示。
- `explain_build_error` 对未收录的 A1581W 返回 `matched:false` / `confidence:"unknown"` + 通用处置清单，
  **不猜**——这是正确行为。
- `parse_map` 正常解析 564 KB 的 `.map`，按段给出 exec/load 地址与 object。

### 7.3 仍未测（需要独占探针或会擦写 Flash）

`ocd_start` / `ocd_probe` / `ocd_read_mem` / `ocd_reg` / `ocd_control` 等所有 `ocd_*`：
OpenOCD 与 Keil 不能同时占用同一根 DAPLink，Keil 正持着探针，故本轮只测了无侵入的 `ocd_cfg_list`（329 个 cfg）。
`flash_download` / `build_and_flash` / `flash_debug` / `ocd_flash` 会擦写设备 Flash，属不可逆操作，未执行。
