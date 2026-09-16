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
| `app` | `app [list \| start <slot> \| stop <slot>]` | App 槽位表：状态 / auto / 任务号 / 崩溃计数 / 入口；启停单个槽 |
| `drv` | `drv [list \| start <slot> \| stop <slot>]` | 驱动槽位表，语义同 `app` |
| `task` | `task` | 内核任务表：优先级 / 状态 / 周期 / 等待时间 / 栈峰值 / 入口 |
| `fault` | `fault` | 故障环形记录：类型 + 任务号 + 时刻（含 `INSTALLFAIL`） |
| `install` | `install` | 打开一次性安装窗口，等待一个 `.svcapp` 镜像（主控先敲命令，再发文件） |
| 内置 | `version` / `clear` / `echo` / `reboot` | `reboot` 走平台钩子，直接写 `AIRCR.SYSRESETREQ` 复位整机 |

## 4. 典型操作

### 4.1 控制一个 App（不自启 + 随时退出）

```
ark> app list
slot  state     auto  task  crash  entry
 0    LOADED    no    0     0      0x08080201
ark> app start 0
app: started
ark> app stop 0
app: stopped, slot released
```

`stop` 会把任务从调度里摘掉并把槽位状态退回 `LOADED`（镜像仍留在 Flash），
所以可以反复 start / stop 而不必重新安装。

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
