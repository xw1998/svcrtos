# 内核 Shell 控制台使用说明（ark-shell 移植）

> 源码：[`kernelsrc/shell/`](../../kernelsrc/shell/)（上游 [ark-shell](https://gitee.com/xw19981010/ark-shell.git) 原样引入 + 一个 SVCrtOS 平台适配层）。
> 配置：`config/svcrt_partition.h` 第四节「内核 Shell 控制台」。

## 1. 它解决什么问题

内核提供一个跑在**特权态**的控制台任务，用来在运行期直接观察和控制系统：

- 看内核/分区/任务表/故障记录的实时状态（不必连调试器）；
- **逐槽**启动、停止 App 与驱动——「像电脑程序一样要点一下才启动」以及「随时关掉」这两类需求都在这里落地；
- 需要时**顺手把镜像从串口装进去**（`install` 命令）。

「开机自启与否」**不是运行期开关**：它在打包时就写进镜像头的 `flags` 字段
（`tools/pack_app.py --autostart / --no-autostart`），开机扫描时由内核读取，
`app list` / `drv list` 的 `auto` 列就是它。Shell 只负责运行期的 start / stop。

## 2. 配置项（`config/svcrt_partition.h`）

| 宏 | 默认 | 说明 |
|----|------|------|
| `SHELL_ENABLE` | 1 | 0 = 不编译控制台任务（省下 ~4KB 内核 RAM + 任务槽） |
| `SHELL_DEV_NAME` | `"COM1"` | 控制台串口设备名（板级注册名） |
| `SHELL_DEV_ARG` | 115200 | 打开设备时的参数（波特率） |
| `SHELL_TASK_PRIORITY` | 14 | 控制台优先级，取最低，交互不抢业务 CPU |
| `SHELL_TASK_STACK_SIZE` | 3072 | 控制台任务栈（字节，含 ark-shell 行编辑缓冲） |
| `SHELL_TASK_PERIOD_MS` | 2 | 每轮轮询间隔 |
| `SHELL_INSTALL_TIMEOUT_MS` | 120000 | `install` 窗口等待镜像头的上限 |

> **串口归属**：`SHELL_ENABLE = 1` 时内核**不再注册常驻安装任务**，读权归控制台，
> 避免两个读者把同一个 UART FIFO 的字节流随机分掉。安装改由 `install` 命令
> 触发一次性窗口（复用同一个设备句柄）。`SHELL_ENABLE = 0` 时回到常驻安装任务。

## 3. 命令一览

| 命令 | 用法 | 说明 |
|------|------|------|
| `help` | `help` | 列出全部命令（含下面这些） |
| `info` | `info` | 内核版本时间、分区表（ABI/硬件签名/各段地址与大小）、任务占用、时基 |
| `app` | `app [list \| start <slot> \| stop <slot> \| uninstall <slot>]` | App 槽位表：type / 状态 / auto / 任务号 / 崩溃计数 / 基址 / 大小 / RAM 窗口 / 入口；启停或卸载单个槽 |
| `drv` | `drv [list \| start <slot> \| stop <slot> \| uninstall <slot>]` | 驱动槽位表，语义同 `app` |
| `task` | `task` | 内核任务表：优先级 / 状态 / 周期 / 等待时间 / 栈峰值 / 入口 |
| `fault` | `fault` | 故障环形记录：类型 + 任务号 + 时刻（含 `INSTALLFAIL`） |
| `install` | `install` | 打开一次性安装窗口，等待一个 `.svcapp` 镜像（主控先敲命令，再发文件） |
| `log` | `log [0..4]` | 不带参数读当前运行日志级别，带参数改（0=off…4=debug） |
| `pool` | `pool` | 镜像池空闲：`free` / `largest`，以及池内固定槽位类型占用 |
| `trace` | `trace [start \| stop \| reset \| dump \| mark <n>]` | 内核事件 trace 的开关、复位、导出与打标记 |
| `cfg` | `cfg [show \| load \| clear]` | 设备端布局配置：查看当前生效值 / 从配置区重新加载 / 清空配置区并回到编译期默认 |
| 内置 | `version` / `clear` / `echo` / `reboot` | `reboot` 走平台钩子，直接写 `AIRCR.SYSRESETREQ` 复位整机 |

## 4. 典型操作

### 4.1 控制一个 App（不自启 + 随时退出）

```
ark> app list
id  type  state     auto  task  crash  base        size    ram         entry
 0   app   RUNNING   yes   4     0      0x08060000  131072  0x20014000  0x08060001
ark> app stop 0
app: stopped (image kept in flash)
ark> app list
id  type  state     auto  task  crash  base        size    ram         entry
 0   app   RAW       yes   0     0      0x08060000  131072  0x20014000  0x08060001
ark> app start 0
app: started
```

列的含义：`ram` 是该槽位的 RAM 窗口基址，`entry` 的 bit0 是 Thumb 位。

`stop` 把任务从调度里摘掉、`task` 归零，镜像**仍留在 Flash**，可以反复 start / stop。
状态退回哪个值取决于槽里是什么：

- 安装的镜像 → `LOADED`（下次压实仍可被当作可搬移镜像）；
- 裸烧的开发镜像 → `RAW`（不可搬移、不参与回收，见 §5）。

### 4.2 开机自启（系统服务类）

```bash
# 打包时带上自启位（默认就是带的，显式写更清楚）
python tools/pack_app.py --axf build/APP_DEMO/app_demo.axf --type app \
       --out app_demo.svcapp --autostart
# 若希望它“要喊一声才启动”：
python tools/pack_app.py --axf build/xxx.axf --type app --out xxx.svcapp --no-autostart
```

`--info` 可以直接看清单里的自启状态：

```
$ python tools/pack_app.py --info app_demo.svcapp
flags : 0x00000001（开机自启）
```

### 4.3 串口安装（控制台窗口）

1. 串口终端连 `COM1`（115200，本板 PA9/PA10），回车看到 `ark> `；
2. 敲 `install`，控制台提示 `install: send the file now`；
3. 主控把 `.svcapp` 原样发出去（`tools/send_image.py` 等）；
4. 控制台打印 `install: ok, slot 0`；失败或超时会打印提示并在 `fault` 里留一条 `INSTALLFAIL`。

> 窗口期间控制台不处理输入（读权已交给安装器），串口是半双工，属正常现象。

### 4.4 设备端布局配置（`cfg`）

```
ark> cfg show
... 当前生效的策略、槽位表、运行期参数 ...
ark> cfg load        # 进入接收状态，等上位机把 512 字节记录发下来
ark> cfg clear       # 擦掉配置区，回到编译期默认
```

能改什么、格式是什么、上位机怎么写这份配置，见
[配置区与安装策略.md](配置区与安装策略.md)。这里只强调两点：

- `cfg load` 是**接收并写入**：它打印就绪行后等 4×128 字节，校验通过才写进 CONFIG
  扇区。手动敲它只会等到超时（`timed out waiting for the record`）——写入由
  `tools/svcrt_cfg.py write` 或图形界面驱动，协议约定见《配置区与安装策略.md》§6.3；
- `mode` / 槽位表 / 回收力度这类策略字段**默认在重启后才完全生效**（池扫描与镜像
  启动在启动早期已按当时的布局做完），写完请按提示重启；
- 配置区被擦除或全部无效时静默回退到编译期默认布局，不影响启动；
- 校验不过（CRC / 硬件签名 / 模式 / 槽位重叠等）时会保留原有配置并报错，不是带病生效。

### 4.5 镜像池空闲（`pool`）

```
ark> pool
pool: free 641592 B (largest run 510504 B), tasks 5/48
```

`free` 是 Flash 上为 `0xFF` 的字节数，含每个镜像槽位没被写到的尾巴；
`largest` 才是“现在能装下的最大镜像”。判断能不能装下要同时看这两个。

## 5. 与其它路径的关系

| 路径 | 说明 |
|------|------|
| 固定地址烧录裸镜像 | 开发期旁路，**保留**；裸镜像没有镜像头，自启与否退回 `APP_AUTO_START` / `DRIVER_AUTO_START` 全局开关 |
| MDK 下断点调试 | **保留**；调试挂停时整机冻结，此时控制台不响应是正常的 |
| `.svcapp` 串口安装 | 本文 4.3；逐槽自启标志随镜像走 |
| `.svcapp` 烧录器写入 | 见 `docs/SVCrtOS应用安装与调试指南.md` 路径 C |

## 6. 已知边界

1. **PC2 复用**：内核 `"LED3"`（蓝灯）与用户态驱动 `BLED_DRV` 注册的 `"BLED"` 是同一个物理灯，
   两者同时跑会互相覆盖显示，演示时只启用其中一个。
2. `reboot` 是整机复位（`AIRCR.SYSRESETREQ`），不是软重启任务。
3. 历史命令缓冲已从上游的 32 条降为 8 条（`ark_shell_config.h`），为内核 RAM 让路。
4. 未知字符/多字节输入按字节透传，中文输入不会被正确回显。

## 日志输出的时延约束（SVC 0x19）

日志发送走的是 SVC 异常处理内部，因此必须**很快返回**：一旦返回过慢，PendSV 无法完成切换，所有任务（含 shell）都会被饿死。

- 每个字节最多重试 `SVCRT_LOG_TX_SPIN_LIMIT`（当前 64）次；仍写不进板级发送 FIFO 就**丢弃该字节**继续下一个。
- 取值理由：单字节最坏 ~64 次重试（约数微秒量级），整行日志的处理器占用被限制在亚毫秒级；`200000` 这种量级会让一行日志在 handler 模式里停留数秒，是必须避免的。
- 症状对照：若发现"串口长时间无响应 + CPU 占用报 100% + 停在 `svcrt_log.c` 的发送循环"，先查这里的预算是否被放大。

## 卸载的语义与空间回收的粒度

`app uninstall <slot>` / `drv uninstall <slot>` 做两件事：把镜像在 Flash 上**作废**，然后**尽力**回收它占的空间。

**作废写在 `type` 字段上（清成 0），不是 `state`。** `state` 只能表达安装/搬移的提交点：安装时先写 UNCOMMITTED（=1），提交时改写为 VALID（=0），两步都是「1→0」的合法编程。镜像一旦提交，`state` 已经是 0，再想写回 1 需要把 0 置 1，而 Flash 编程只能把 1 清成 0——硬件会静默保持 0，卸载于是变成空操作，镜像在下次上电又会被扫回来（这就是「卸载后掉电复活」的成因）。`type = 0` 不是已知镜像类型，`check_header()` 会拒收、上电扫描会丢弃；`magic` 故意保留，好让回收沿用「本区间有带头内容」的判断。

**回收的粒度是池的物理单元（F427 上是 128K）。** 池内某个单元里只要还有一个活镜像，整个单元就擦不掉；而落点已经在池底的镜像不可能再往下搬，所以只要「单元里还有别人」，空间就一直被钉住。命令会如实区分这两种结果：

```
drv: uninstalled (image invalidated, its sector was erased)                        # 单元已空，扇区真的擦掉了
drv: uninstalled (image invalidated; space returns once its sector is free)        # 只是作废，空间还钉着
```

实测（`build/BLED_DRV` + `build/BLED_APP` 两个镜像，池内共 4 个镜像挤在 0 号单元）：停掉并卸载前三个时都只作废、`pool free` 不动；卸载最后一个时该单元再无活槽位，扇区被擦除，`pool free` 由 776244 B 回到 786432 B（该单元整片 0xFF）。

`pool` 给出的 `free` 是「Flash 上为 0xFF 的字节数」，因此包含每个镜像槽位里没被写到的尾巴（1KB 对齐扣掉实际长度的那部分）。它是**物理空闲**，不等于「下一次安装能拿到多少」；判断能不能装下要同时看 `largest`。
