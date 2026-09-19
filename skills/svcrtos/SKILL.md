---
name: svcrtos
description: SVCrtOS（Cortex-M 上的 SVC 特权隔离 RTOS）工程操作手册：编译/烧录内核、打包与安装 .svcapp 应用到设备、读写设备端布局配置区、使用串口 shell、以及经 mdk_agent_mcp 做在线调试。当任务涉及 SVCrtOS 工程、应用/驱动安装、分区与固定槽位、布局策略、或该目标板的真机验证时使用本 skill。
---

# SVCrtOS 工程操作手册（给 AI 代理）

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

---

## 3. 串口闭环（这是最容易踩坑的地方）

设备控制台：USART1（PA9/PA10），**115200 8N1**，主机侧通常是 `COM3`。

### 3.1 shell 命令表

`info` / `app` / `drv` / `task` / `fault` / `install [slot]` / `log` / `pool` / `trace` / `cfg`

- `app [list|start <slot>|stop <slot>|uninstall <slot>]`，`drv` 同构；
- `pool` 打印镜像池与**固定槽位**（固定槽模式下才有槽位行）；
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
  两套编号会不一致（实测：装在配置槽 2，分区表槽号是 1）。
  把配置槽序号直接拿去 `app uninstall`，会被 `slot_type` 校验挡下并回 `usage:`。
  **要确认落点就看 `base`**，不要拿两套序号互相对照。
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
3. **不要在一条命令后马上发下一条**：设备会打印就绪横幅，等它（或等静默）再发。

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

---

## 5. 常见故障对照

| 现象 | 先查什么 |
|---|---|
| `cfg load: record rejected, bad magic` | 主机是否用了 CRLF / 有没有第二个读者 / 设备是否刚被 flush 过接收 FIFO |
| `install` 被拒 `err -3` | 镜像 `hw_compat_id` 与内核不符 → `pack_app.py --info`，重新打包 |
| 安装后 `app` 里出现两个实例、输出逐字节交错 | 上一个实例没停：`app list` → `app stop <slot>`（自启镜像会自己回来） |
| `app uninstall 2` 回 `usage:`（明明槽位存在） | 拿配置槽序号当分区表槽号了：先 `app` 看 `base` 对上是哪一行，用那一行的 `id` |
| 安装被拒后控制台冒出一行 `Command not found: ...` | 头/表必须背靠背发送，而兼容性只看 256 B 头就早退，残下的重定位表字节落入 shell。属固有副作用，不影响后续操作 |
| `install: failed or timed out` | 看设备侧 `INSTALL` / `LOADER` 日志行 + `fault`；不要相信主机侧的"发送完成" |
| `cfg show` 显示 fixed 但 `pool` 不列槽位 | 读 `0x20000000 + 52` 核对真正生效的 `layout_mode`，两个真相源要不一致就是 bug |
| 编译 0 Error 但行为不对 | 检查宏是否真的进了命令行（uvprojx 的 `<Define>` 有静默不生效的坑） |

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
