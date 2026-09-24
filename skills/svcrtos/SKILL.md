---
name: svcrtos
description: SVCrtOS（Cortex-M 上的 SVC 特权隔离 RTOS）工程操作手册：编译/烧录内核、打包与安装 .svcapp 应用到设备、读写设备端布局配置区、使用串口 shell、以及经 mdk_agent_mcp 做在线调试。当任务涉及 SVCrtOS 工程、应用/驱动安装、分区与固定槽位、布局策略、或该目标板的真机验证时使用本 skill。
---

# SVCrtOS 工程操作手册

工程根目录 = 本文件所在目录往上两级（`<root>/skills/svcrtos/SKILL.md`）。
北向目标：**让 MCU 像手机一样安装应用与驱动**——不写死地址、安装时以最高密度往后挪、
卸载时回收空间；同时也支持"客户自己用 MDK 下载调试"的固定槽位模式。

先读这三份文档再动手（它们是权威描述，本文件是操作索引）：

| 文档 | 内容 |
|---|---|
| `docs/SVCrtOS应用安装与调试指南.md` | 从编译到安装的完整闭环与排错 |
| `docs/配置区与安装策略.md` | 布局/策略的 ABI、CLI、验收结论 |
| `docs/内核Shell控制台使用说明.md` | shell 命令表与语义 |

---

## 1. 硬约束（先看这一节，违反会返工）

- **不新建分支**：所有改动直接提交 `master`。
- **地址只有一个源头**：`config/svcrt_partition.h`。改了 `CHIP_FLASH_SIZE`，
  分区、池、槽位、`.sct` 全部自动重算。**不要**在别处复制粘贴地址。
- **Loader / App 工程禁止 include `svcrt_partition.h`**：布局在运行期经 SVC 0x18 取。
- **`.sct` 是构建产物**：由 `tools/gen_scatter.py` 生成，禁止手工编辑。
- **编码与行尾**：既有源文件是 GBK/UTF-8 + CRLF 混杂；**改已有文件必须走字节级补丁**
  （锚点唯一性 `count == 1`、保留原行尾、幂等），不要整文件转码——Keil 里的中文注释会乱码。
  新增文件用 UTF-8；新增注释一律英文 ASCII。
- **删除文件、`git push`、擦写设备 Flash 之前先问用户**。

---

## 2. 命令行闭环

工具链为 Keil MDK（AC5/AC6）。三条命令各有用途，**不要串在一条 `&&` 里**：

```bash
# 编译（重建，不下载）。UV4 用 Windows PATH 找 python，先注入 PATH
export PATH="/c/Users/<你>/AppData/Local/Programs/Python/Python310:$PATH"
"D:/Keil_v5/UV4/UV4.exe" -r "<proj>.uvprojx" -o "C:\\tmp\\k.log"   # ← -o 必须绝对路径

# 烧录（独立调用）
"D:/Keil_v5/UV4/UV4.exe" -f "<proj>.uvprojx" -o "C:\\tmp\\f.log"
```

工程文件：

| 目标 | 工程 |
|---|---|
| 内核（F427，板上在跑） | `example/stm32f427/kernel/SVCRTOS_TEST/MDK-ARM/SVCRTOS_TEST.uvprojx` |
| 内核（F401） | `example/stm32f401/kernel/SVCRTOS_TEST/MDK-ARM/SVCRTOS_TEST.uvprojx` |
| 应用示例 | `example/stm32f427/app_sdk/APP_DEMO/MDK-ARM/app_demo.uvprojx` |
| 驱动示例 | `example/stm32f427/driver_sdk/BLED_DRV/MDK-ARM/bled_drv.uvprojx` |

**退出码的坑**：UV4 返回 `1` 只表示"有 warning"（本工程基线就是 1 Warning：
`svcrt_context.S` 的 A1581W）。因此 `-r` 返回 1 会让 `&&` 后面的 `-f` 不执行——
必须分开调用，并读日志确认 `0 Error(s)`。

Python 脚本调用一律加 `env -u PYTHONHOME -u PYTHONPATH`（宿主环境变量泄漏会污染子进程）。

打包镜像（`.svcapp` = 256 B 头 + 重定位表 + 负载）：

```bash
py -3 tools/pack_app.py --project <app.uvprojx> --type app --name APP_X --out build/APP_X.svcapp
py -3 tools/pack_app.py --info build/APP_X.svcapp      # 看头（含 hw_compat）
py -3 tools/pack_app.py --verify build/APP_X.svcapp
```

离线打包（手上已有 A/B/C 三遍的 `.bin` 时）**必须给 `--bin-a/--bin-b/--bin-c`**
（`--bin-d` 可选但强烈建议，它就是打包期的正确性证明）；入口用 `--entry-offset`
或 **标称基址那一遍的** `--axf` 给。**只写 `--axf` 而不给三遍 bin 是跑不起来的**
——重定位表靠三遍差分求得，给不了就退化成“不报错但也没打包”。

---

## 3. 串口闭环（这是最容易踩坑的地方）

设备控制台：USART1（PA9/PA10），**115200 8N1**，主机侧通常是 `COM3`。

### 3.1 shell 命令表

`info` / `app` / `drv` / `task` / `sched` / `fault` / `install [slot]` / `log` / `pool` / `trace` / `cfg`

- `app [list|start <slot>|stop <slot>|uninstall <slot>]`，`drv` 同构；
- `task [<id> [x]]`：**任务号只有一套，从 1 开始，0 表示「没有任务」**——内核任务表行首、
  `app` / `drv` 列表的 `task` 列、`fault` 记录的任务号、`task <id>` 的参数全是这一个号；
  `task 0` 与越界号一律回 `task: no such task (ids run 1..<N>)`；
- `pool` 打印镜像池与**固定槽位**（固定槽模式下才有槽位行）；
- 控制台启动时先打印一段 ASCII 横幅（`ARK_SHELL_WELCOME`，在
  `kernelsrc/shell/ark_shell/ark_shell_config.h`；`ARK_SHELL_WELCOME_ART=0` 回通用文案），
  再给出提示符。**横幅只在启动时出现一次**，就绪判据用 `ark> ` 或静默，不要等横幅；
- `sched` 自检就绪集（256 位两级位图 + 每优先级链）与任务表的**逐条**一致性：
  一致时打印 `sched: consistent (bitmap/links == task table)`，不一致时逐条打印
  `#idx prio= status= on_ready= next= prev=`，并附 `ready=<就绪数> top=<最高优先级任务>`；
  改过调度器或手工注册过任务后，**第一件事是敲 `sched`**；
- `fault` / `log` 是排查安装与启动失败的第一站；
- `cfg show` 打印当前生效的布局与策略；`cfg load` 是**进入接收状态等 512 字节**，
  不是"重新读一遍"（手工敲会等到超时回 `timed out waiting for the record`）；
  `cfg clear` 擦除配置区、回编译期默认。

### 3.2 安装协议（`install` 窗口）

```
头 256 B + 重定位表(reloc_count × 4 B)   ← 一次突发，设备要先把表写进 Flash 才能 ACK
随后负载按 512 B 分块，每块等一个流控字节
    0x06 = 继续， 0x15 = 停止
```

- **流控字节不带原因码**。原因只在设备随后的打印行里
  （`[E][LOADER] frame rejected before payload: err=<code>` 或 `[E][INSTALL] rejected: err <code>, ...`）；
  `err` 码见 `kernelsrc/include/svcrt_loader.h`：`-3` 兼容号不符、`-4` 长度、`-5` CRC、`-13` 重定位表……
- 主机侧参考实现：`tools/send_image.py`（命令行）、`tools/svcrt_host_gui.py` 的"安装镜像"页（图形）。
- `install <n>` 里的 `n` 是**配置槽位表的下标**（仅固定槽模式有效），设备会先回一行
  `install: fixed slot n -> 0x________`，装完回 `install: ok, slot <分区表槽号>`。
- **固定槽模式同时存在两套编号，别混用**：配置槽序号（布局 JSON / GUI 布局页看到的 0、1、2）
  只是「第几个固定槽」；`install: ok, slot N`、`app` / `drv` 列表的 `id` 列、以及
  `app start <n>` / `app stop <n>` / `app uninstall <n>` 的参数，全部是**分区表槽号**。
- **App 与驱动走同一条通路**：驱动包就是 `pack_app.py --type driver` 产出的 `.svcapp`
  （包内 `type = 2`），装法、窗口、流控、校验与 App 完全相同，内核按类型找槽。auto 模式
  先 `install` 再发文件即可；fixed 模式必须装进配置里 `type = driver` 的槽，否则回 `err -7`。
  两套编号会不一致（实测：装在配置槽 2，分区表槽号是 1）。
  把配置槽序号直接拿去 `app uninstall`，会被 `slot_type` 校验挡下并回 `usage:`。
  **要确认落点就看 `base`**，不要拿两套序号互相对照。
- **fixed 模式必须点名槽位**：裸 `install` 只有 auto 模式接受。fixed 模式下内核在看到
  镜像头之前不知道目标槽，会直接回 `usage: install <slot>   (fixed-slot mode needs an
  explicit slot)`，而不是"先开着窗口等"。
- **`install <slot>` 会先擦该槽（整扇区）再打印就绪行**：主机要等这一行出现才发。擦除
  期间 CPU 停住、串口收不进字节，早发一个字节都会丢，表现为中途 `err -4` 且无 `LOADER` 行。
- **fixed 模式覆盖安装会在开窗之前先擦目标槽**：`install <slot>` 走
  `svcrt_installer_run_once()`，开窗前按 `slot_hint` 预清目标槽（擦除占约 1 秒且期间收不进
  字节，必须发生在主机开始发之前）；没点名槽位（`slot_hint < 0`）时直接回参数错误，而不是
  擦到一半。
- **`SHELL_ENABLE=0` 的常驻安装任务同样闭环**：它没有命令行，目标槽改由配置给出——
  `config/svcrt_partition.h` 的 `INSTALLER_FIXED_SLOT`（配置槽序号，不是分区表槽号，默认
  `-1`）。设成 N 后，常驻任务在开窗前先做一次准备：该槽里若有正在运行的镜像，先
  `svcrt_loader_stop()` 停掉它（没有控制台，这一步等价于 `app stop <slot>`；不停就直接擦
  会擦掉正在取指的代码），再整槽擦一次并打印 `fixed slot N cleared, send the file now`。
  这个状态只建立一次：一个上电周期只接受一帧，第二帧被拒并回 `one image already installed
  this boot: reboot before sending another one`，随后 drain 掉该帧的尾巴（日志出现
  `drained N B of the rejected frame`）。保持默认 `-1` 时，fixed 模式下的常驻任务拒绝任何
  帧，而不是猜一个槽去擦。
- 覆盖安装一个正在运行的镜像会被拒（`-15 BUSY`）：先 `app stop <slot>`。
- **镜像头的 `hw_compat_id` 必须等于内核的 `SVCRT_HW_COMPAT_ID`**（F427 当前为 `0x42700005`），
  否则安装会在写负载之前被拒（`err -3`）。仓库里 `build/` 下的旧镜像可能是旧兼容号，
  先 `pack_app.py --info` 确认，必要时重新打包。

### 3.3 配置记录协议（`cfg load`）

```
magic "SCFG" 的记录 512 B，分 4 块 × 128 B 上传，逐块等流控字节
```

- 记录由 `tools/svcrt_layout.py` 从 JSON 生成（`template` / `check` / `build`），
  写入用 `tools/svcrt_cfg.py write`，读回 `cfg show`；
- **策略类字段（layout/reclaim/knobs）要重启才生效**，记录本身当场写入。

### 3.4 三条必须守住的约定

1. **shell 以 CR 断行**。发 `cfg load` 用 CRLF 时，残留的 LF 会占掉记录第 1 字节，
   设备收满 512 B 后报 `bad magic`——"配置写不进去"的头号原因；
2. **同一时刻只能有一个读者打开串口**：安装窗口 / 配置接收窗口期间，其他串口工具
   会与设备抢 FIFO。GUI 的做法是暂停读线程后再独占；
3. **不要在一条命令后马上发下一条**：设备跑完命令会重画 `ark> ` 提示符（启动时的那段
   ASCII 横幅只在开机出现一次），等提示符或等静默再发。

---

## 4. 在线调试通道（mdk_agent_mcp）

本工程接入了 `mdk_agent_mcp`（<https://gitee.com/xw19981010/mdk_agent_mcp.git>）注册的
MCP 调试工具，可不下载、不打断地读写目标：

| 工具族 | 用途 |
|---|---|
| `enter_debug` / `exit_debug` / `reset` | 进出调试、复位（`reset {run_after:true}` 需先进入调试） |
| `read_mem` / `write_mem` | 按地址读写（读 u32 序列，能手工解结构体） |
| `read_var` / `write_var` / `read_regs` | 变量、寄存器组 |
| `set_breakpoint` / `set_watchpoint` / `run_to_line` | 断点与运行控制 |
| `uvprojx_edit` / `launch_uvision` | 工程配置与 Keil 实例管理 |
| `trace_swd_status` / `trace_swd_reset` / `trace_swd_read` | 运行期 trace：状态 / 重新武装与切粒度 / 搬回并解码（`out_file=` 落盘） |
| `view_render` / `view_guide` | 把上面搬回的数据渲染成单文件 HTML（参数是 `data`/`data_file`/`out`，**没有 `out_file`**） |

**共享分区表是最快的取证点**（F427，小端 u32 数组，`0x20000000` 起）：

```
偏移 52 = layout_mode    (0 = auto, 1 = fixed)
偏移 56 = layout_source  (0 = 编译期默认, 1 = 设备配置区)
偏移 68 = cfg_slot_count
偏移 72 = reclaim_mode   (0 = global, 1 = minimal, 2 = none)
```

当"命令输出说一套、实际行为另一套"时，直接读这里，比加打印重编译快得多。
（曾经就是靠它发现"整表播种覆盖了运行时布局字段"。）

注意事项：`UVSOCK` 下 `stop` 之后**第一次 `read_mem` 可能读到全 0 脏帧**，重读复核；
裸地址带 Thumb 位（奇数）会报 `error 57 illegal address`。

### 4.1 运行期 trace（只接 SWD 两线也能录）

三条**会直接决定成败**的：

1. **插桩上电即默认武装，不是「先敲 `trace start`」**。`board/stm32f427/svcrt_board.c`
   在板级初始化时调用 `svcrt_trace_init()`（内部 `g_tr_on = 1`），所以复位后立刻就在录——
   这是有意的（为了能复现「复位到启动」那一段）。两个后果要记住：
   - **要看干净的调度开销，先敲 `trace stop`**：插桩使每趟 PendSV 多花约 1760 周期
     （实测切换开销放大约 5 倍），不关掉会把被测的改动完全盖住；
   - **没人搬运时会持续丢事件**（实测 3 秒窗口丢 138068 条），不要用 `dump` 里的事件数
     当作真实发生数。
   反过来，`g_tr_on == 0` 时控制块整片 `0x00`，工具报 `swd-read-degenerate`——那是**正确判据**，
   不是工具坏了。
2. **多轮采集必然混段，渲染前必须切段**。每停一次再跑，目标就重开录制段（`seq` 递增），
   新旧段时间基不同。工具会在 `meta.notes` 里警告但**不替**你切；不切就直接渲染会得到
   “几十秒长的时间轴、数据全挤在最后几百毫秒”的**看着像真图**的误导图。
   做法：只留**最后一次 `sync` 之后**的事件。
3. **trace 里的任务编号是 0 基任务表下标（15 = idle），且跨复位会重排**。
   不要把它钉死成某个 App；认任务用 `task` 表的 `entry` 地址去比对。

另外：环满**丢事件不覆盖**，`lost_events` 才是真相；渲染页的时间单位以页面声明为准。

---

## 5. 常见故障对照

| 现象 | 先查什么 |
|---|---|
| `cfg load: record rejected, bad magic` | 主机是否用了 CRLF / 有没有第二个读者 / 设备是否刚被 flush 过接收 FIFO |
| `install` 被拒 `err -3` | 镜像 `hw_compat_id` 与内核不符 → `pack_app.py --info`，重新打包 |
| 安装后 `app` 里出现两个实例、输出逐字节交错 | 上一个实例没停：`app list` → `app stop <slot>`（自启镜像会自己回来） |
| `app uninstall 2` 回 `usage:`（明明槽位存在） | 拿配置槽序号当分区表槽号了：先 `app` 看 `base` 对上是哪一行，用那一行的 `id` |
| `install <slot>` 回 `err -7` 而槽里原来的镜像也没了 | fixed 模式点名的槽类型不符（驱动包只能进 `type = driver` 的槽）。**`install <slot>` 是先清槽、再收帧**（日志 `fixed slot N: clearing it before receive`），旧镜像已经停了、槽也擦了；设备会另起一行给出「configured type X, image type Y」。要装 driver 包，配置里得有一个 driver 槽 |
| 安装被拒后控制台冒出一行 `Command not found: ...` | 头/表必须背靠背发送，而兼容性只看 256 B 头就早退，残下的重定位表字节原先会落入 shell。安装器现在返回前会把这张表读完（日志 `drained N B of the rejected frame`）；若仍看到这一行，先确认设备侧有没有这条排空记录 |
| `install: failed or timed out` | 看设备侧 `INSTALL` / `LOADER` 日志行 + `fault`；不要相信主机侧的"发送完成" |
| `cfg show` 显示 fixed 但 `pool` 不列槽位 | 读 `0x20000000 + 52` 核对真正生效的 `layout_mode`，两个真相源要不一致就是 bug |
| 编译 0 Error 但行为不对 | 检查宏是否真的进了命令行（uvprojx 的 `<Define>` 有静默不生效的坑） |
| `trace start` 没敲就采集 → 报 `swd-read-degenerate` | 不是工具坏了：先在设备上 `trace start` 武装（§4.1） |
| trace 出来的时间轴长得离谱（几十秒）而数据只占最后一点 | 混段了：按最后一次 `sync` 切段再渲染（§4.1） |
| 只看到红/绿在闪、蓝灯不动 | 正常：红/绿是内核自带 1 Hz 反相任务，蓝是 App 心跳。蓝不动 = 池里没有正在跑的 App |
| 装了 BLED_DRV 但灯没变化 | 正常：`DrvMain` 只 `svcrt_task_wait(1000)`，它不翻转任何灯 |
| `fs put` 上传期间小程序状态行的 `over` 整段增长 | 正常：轮询那次文件读要排队等上传者手里的 littlefs（实测最长 100 ms），循环本身没被停住（迭代速率仍 ~90/s） |
| 交接协议里 `[v2] asking version 1.0.0 (owner 1, it=0)` 的 `it=0` | 正常：供方稳态不写文件，卷上那条 ACTIVE 记录是它启动时的快照，交接那一刻才刷新 |
| 同一上电周期内「装完就早停」，重启后同一镜像却跑得好好的 | 固定槽的 RAM 窗口必须与上电扫描同源：两者都按镜像头的 `ram_size` 向上取整，配置里登记的 `ram_size` 只是上限。若安装路径按配置值绑窗口，栈会比扫描路径高出差值那么多，`fault` 里出现的是一条 `STACKOVF` —— 症状看着像 App 自身的栈不够，其实是窗口来源不一致 |

---

## 5.5 运行中升级（无缝升级例程）

想在**不改内核、不停控制回路**的前提下把一个正在跑的 App/小程序换掉，现有做法只有一条：
**控制逻辑以小程形态驻 RAM、镜像放外部 NOR**（片内 A/B 不行：`sched_lock` 下的擦除会让取指停住约一秒）。
例程：`example/stm32f427/app_sdk/SEAMLESS_V1`（被接管，1.0.0，周期 10 ms）与
`SEAMLESS_V2`（接管方，1.1.0，周期 5 ms），详述与实测见 `docs/无缝升级例程.md`。

交接通道是卷上的两条定长记录（magic + CRC32，写非原子所以要校验）：
`/sx_state.bin` 64 B（写者 = 当前控制者：owner/phase/iterations/pv/integ/gap…）与
`/sx_req.bin` 32 B（写者 = 新版本）。三条硬规则：**只有当前控制者写状态**；
**请求谁写谁撤**（放弃接管要删掉自己的请求，否则供方会照着残根释放、最后没人控制）；
**供方只认 `to_version > 自己版本` 的请求**。

上板实测（F427）：循环周期 10.7 ms / 5.9 ms（标称 10 / 5），交接耗时 27 ~ 407 ms，
迭代数与受控量逐字连续（`it=994 → resumed at it=994`），交接前后 `fault` 行数不变。
**装载停顿没有消除，只是被测出来**：装载 5.4 KB 小程序时循环最大空洞实测 416 ms
（装载 1 KB 的 mini_demo 为 193 ms）；写卷（`fs put`）本身没有把循环停住，
但会把轮询读拖长。

命令：`fs put` 送镜像 → `mini run <路径>`；镜像用
`pack_app.py --type miniapp --no-autostart` 打包（`--no-autostart` 不能省）。

---

## 6. 改完就按这个清单自检

1. 两块板内核各编译一次（`-r`），日志 `0 Error(s)`；
2. F427 烧录（`-f`，独立调用）；
3. `app list` 只有一个实例在跑；
4. 与布局/配置相关的改动：`svcrt_layout.py check` → 写入 → `cfg show` 读回 → **重启** → 再 `cfg show`；
5. 与安装相关的改动：真机装一次（看设备日志，不看主机侧"发送完成"）；
6. 结论分三档如实写：**仅编译通过 / 上板验证过 / 未验证**——不要用修饰词把"编译过"说成"验证过"。

图形界面上位机（人工操作用）：`py -3 tools/svcrt_host_gui.py`
（连接 / 控制台 / 安装镜像 / 布局配置 / 帮助 五页，协议与命令行脚本完全一致）。

想录一段运行过程给人看时：设备侧 `trace start` → 采集三件套 → 切段 →
`view_render` 出单文件网页（做法见 §4.1）。
