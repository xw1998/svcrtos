# VFS：Linux 风格路径命名空间

> 对应提交 `f77b1b7`（上板验证与挂载窗口修复见文末「验证状态」）。
> **验证分档写清楚**：编译通过 / 主机测试通过 / 上板验证过。

## 为什么要有这一层

在这之前，内核里只有两种「名字」：

| 名字 | 入口 | 能做什么 |
|---|---|---|
| 设备名 | `svcrt_dev_open("uart0")` | 开设备 |
| 卷内路径 | `svcrt_fs_open_write("/a.txt")` | 读写 littlefs 里的文件 |

两者互不相通：设备不是路径，路径也到不了设备。App 想「打开这个路径」时，
得先知道那到底是设备还是文件，然后走两套不同的 API。

这一版把 [ark_vfs](../../../ark_vfs) 挂到内核上，所有资源落进同一棵树：

```
/            易失便签区（ramfs，掉电即失，放临时数据）
/dev         设备注册表的镜像；/dev/uart0 可以直接按路径打开
/mnt/<名字>  持久卷（littlefs），由上层显式挂
```

路径语义按 Linux 来：`//` 合并、`.` 去掉、`..` 按前面的段消化，越过根的 `..`
报错（`ARK_E_LOOP`）而不是悄悄折回根。挂载点用最长前缀匹配，所以
`/mnt/nor` 会遮住 `/` 覆盖的那一段。

## 组成

| 文件 | 角色 |
|---|---|
| `kernelsrc/components/ark_vfs/` | vendored 组件（上游：ark_vfs 仓库，源 commit 见该目录的 `VENDOR.md`——只在那里记一次，避免每次同步后这里过期） |
| `kernelsrc/src/svcrt_vfs.c` | 门面：挂 `/` 与 `/dev`、挂/卸卷、读写删列 |
| `kernelsrc/src/svcrt_vfs_lfs.c` | littlefs 桥：把 `svcrt_fs` 接成 ark_vfs 的 fsdrv `"lfs"` |
| `kernelsrc/src/svcrt_dev.c` | 新增 `svcrt_dev_name_at()`：devfs 靠它枚举设备表 |
| `kernelsrc/include/svcrt_vfs.h` | 内核侧唯一的公开面（shell 与 App 只看它） |

**为什么内核不直接 include `ark_vfs.h`**：ark_vfs 是外部组件、版本独立演进。
内核只经 `svcrt_vfs.h` 这一层用它；将来 ark_vfs 换了 API，改的是这一层，
不是 shell、不是 App。

## 门面 API

```c
int32 svcrt_vfs_init(void);                     /* 幂等；挂 / 与 /dev */
uint8 svcrt_vfs_ready(void);
int32 svcrt_vfs_mount_volume(dev, off, size, target);   /* 挂持久卷 */
int32 svcrt_vfs_umount(target);
int32 svcrt_vfs_mount_info(idx, out);
int32 svcrt_vfs_mount_count(void);
int32 svcrt_vfs_read_file(path, buf, max, out_len);     /* 读整个文件 */
int32 svcrt_vfs_write_file(path, buf, len);             /* 写整个文件 */
int32 svcrt_vfs_open_read(path);                        /* 流式读三件套 */
int32 svcrt_vfs_read(fd, buf, len);
int32 svcrt_vfs_close(fd);
int32 svcrt_vfs_remove(path);
int32 svcrt_vfs_list(dir, cb, arg);
const char *svcrt_vfs_error_name(rc);
```

错误一律用 ark_vfs 的负错误码，符号名由 `svcrt_vfs_error_name()` 附加；
**报告时数字永远保留**，符号只是附加说明。

## 挂载点是「谁知道」的问题

内核知道**设备注册表**（所以 `/dev` 归它），不知道**哪块设备上的哪个偏移
放着文件系统**——那是板级布局知识。所以：

- `/` 给一块不依赖硬件的易失区（ramfs）；
- 持久卷留一个显式入口 `svcrt_vfs_mount_volume()`，由板级或 shell 下令。

挂载**不格式化**。卷没格式化就返回 `ARK_E_IO`，不会顺手擦掉别人的数据——
格式化是破坏性动作，得有人明确下令。

## 开机时序

`example/*/Core/Src/main.c` 在**启动任何镜像之前**拉起命名空间，理由是
命名空间的意义就在这里：App 应该能从第一行就 `open("/dev/uart0")`，
而不是等谁想起来去初始化它。失败不致命（控制台和镜像都还在），但会记日志，
错误名会告诉你是哪一层拒绝的。

## shell 命令

| 命令 | 说明 |
|---|---|
| `mount` | 列出挂载点 |
| `mount <target>` | 把**板载默认卷**（`SVCRT_FS_DEV_NAME/BASE/SIZE`）挂到 `<target>` |
| `mount <dev> <off> <size> <target>` | 挂 `<dev>` 上的一段窗口（`size 0` = 到设备末尾） |
| `umount <target>` | 卸一个挂载点 |
| `ls [path]` | 列目录，默认 `/` |
| `cat <path>` | 打印文件（不可打印字节转义成 `\xNN`） |
| `rm <path>` | 删文件 |

与既有 `fs` 命令的分工：`fs` 管**一个卷**（挂 / 格式化 / 卷内读写），
上面这组走的是**命名空间**。名字照 Linux 来——`ls` / `cat` / `mount` 已经是
人人都懂的意思。

### `size 0` 不等于「板子的那个卷」

`size 0` 的字面意思是**到设备末尾**。这是一块 16 MiB NOR，所以
`mount nor0 0 0 /mnt/nor` 说的是「从 0 到 16 MiB」；而这块板上的默认卷是
4 MiB（`SVCRT_FS_SIZE`）。两个窗口不是同一个，littlefs 会读盘上的超级块、
发现 `block_count` 对不上，回 `LFS_ERR_INVAL(-22)`。

想挂「板子上那个卷」就写 `mount /mnt/nor`，别用四个参数去凑一个窗口。
四个参数的形态是给**别的窗口**用的（分区、第二个区段），那时窗口宽度由你
自己负责写对；写不对不是静默降级，是当场报错，日志里会带上请求的窗口和
littlefs 的原始错误码：

```
[E][vfs:250] svcrt_fs_mount(nor0, off=0, size=16777216) failed: last_error=-22 (LFS_ERR_INVAL)
mount: FAILED (-11: backend error)
```

### 已挂着一个卷时，窗口要对得上

`svcrt_fs` 只有一个卷。如果它已经挂着一个卷（例如 App 开机时通过 SVC
把它拉起来过），而这次请求的 `dev/off/size` 和它**不是同一个窗口**，那就
**拒绝**（`ARK_E_EXIST`）而不是静默复用它：

```
[E][vfs:266] a different volume is already mounted: nor0 off=0 size=4194304 (asked for nor0 off=0 size=16777216)
mount: FAILED (-7: already exists)
```

窗口一致时复用（这是 App 与 shell 共享同一个卷的正常路径）。之所以要
卡这一道：静默复用会把 `dev/off/size` 整个丢掉，于是同一句 `mount` 在
「卷已挂」和「卷未挂」两种状态下给出**不同的几何**，而两种状态在命令行
上看起来一模一样——那是最难查的一类错答案。

## 功能开关

`kernelsrc/include/svcrt_features.h`：

| 开关 | 默认 | 作用 |
|---|---|---|
| `SVCRT_USE_VFS` | 1 | 门面 + ark_vfs 组件 + shell 那 5 条命令 |
| `SVCRT_USE_VFS_LFS` | 1 | littlefs 桥（`/mnt` 下的持久卷） |

依赖写死成 `#error`：`SVCRT_USE_VFS` 要 `SVCRT_USE_BLK`，
`SVCRT_USE_VFS_LFS` 要 `SVCRT_USE_FS`。

**关掉功能 ≠ 报故障**，两者必须分得开：

- 端口两个 `.c` 编成空翻译单元（`nm` 上零符号）；
- 门面所有入口返回 `SVCRT_VFS_ENOSYS`（值与 `ARK_E_NOSYS` 同，有静态断言盯着），
  而不是让调用方在链接期撞 undefined；
- `main.c` 那个拉起块整段被 `#if` 包住——否则关掉 VFS 的构建每次开机会打一条
  「svcrt_vfs_init failed: -12」，把「按需关闭」报成「启动失败」。

## 验证状态

**编译通过**（主机 `armclang -fsyntax-only` 零告警 + Keil 全量）：

| 板 | 结果 | Code |
|---|---|---|
| F427 | 0 Error / 1 Warning（唯一告警是基线 `svcrt_context.S` A1581W） | 92206 → **103070** |
| F401 | 0 Error / 1 Warning（同上） | 86222 → — |

裁剪组合也各自编过：`-DSVCRT_USE_VFS_LFS=0`、`-DSVCRT_USE_VFS=0 -DSVCRT_USE_VFS_LFS=0`。

**主机测试通过**（在 ark_vfs 仓库里）：核心 161 checks + SVCrtOS 端口 70 checks，
共 231，全绿，`-Wall -Wextra -Werror` 干净。

**上板验证过**（F427，USART1/COM3，固件 `Code=103070`）：

| 验的东西 | 设备原样回显 |
|---|---|
| 命名空间 | `mount` → `/dev`(devfs) + `/`(ramfs) + `/mnt/nor`(lfs, rw) |
| `/dev` 枚举 | `ls /dev` → LED / LED2 / LED3 / COM1 / BLED |
| 持久卷挂载 | `mount /mnt/nor` → `[I][vfs:291] volume nor0 mounted at /mnt/nor` |
| 卷内列目录 | `ls /mnt/nor` → 9 entries（含 `keep.txt` 15 B） |
| 卷内读文件 | `cat /mnt/nor/keep.txt` → `persistent-data` / `15 bytes` |
| 窗口过大被拒 | `mount nor0 0 0 /mnt/nor` → `-11: backend error`，日志带 `size=16777216` |
| 已挂载时窗口不符 | `mount nor0 0 0 /mnt/nor` → `-7: already exists`，日志写明两边窗口 |
| 错误路径 | `ls\|cat\|rm /mnt/nor/nosuch` → `-1: no such file or mount` |
| 目录当文件读 | `cat /` → `-5: is a directory` |
| 字符设备读 | `cat /dev/LED` → `-11: backend error`（驱动拒绝，不是 EOF） |
| 卸载 | `umount /mnt/nor` → 成功，`mount` 回到两行 |

**未上板**：端口的锁只验过嵌套配平，没验过真并发——那需要两块任务在板上抢。

## 已知限制（说清楚做不到，不是静默降级）

1. `svcrt_fs` 一个卷同时只允许一个写流。所以 `/mnt` 下第二个写 open 拿到
   `ARK_E_BUSY`，有写流在跑时读也 `BUSY`——读和写共享同一份文件缓存。
2. 一个内核只挂一个持久卷（`g_vol_target` 单槽），这是 `svcrt_fs` 的限制，
   不是这一层加的。
3. `svcrt_vfs_open_read()` 只放只读打开。写清一色走 `svcrt_vfs_write_file()`；
   多一个半成品标志集只会让人以为那几种组合都试过了。
4. 只读挂载（`ro`）字段已经在 `mount_info` 里，但门面还没有「只读挂载」入口。
5. `mount <dev> <off> <size> <target>` 的窗口由调用者负责写对：窗口比盘上
   的卷大（常见于 `size 0`）会被 littlefs 拒绝，而不是被缩到实际卷大小。

## 上游文档（ark_vfs 仓库）

组件本身的设计与用法不在本文件里，看上游三篇（仓库已开源：
`https://gitee.com/xw19981010/ark_vfs`，本地 `D:/工作/git_project/ark_vfs`）：

| 文档 | 内容 |
|---|---|
| `docs/architecture.md` | 分层与各层能拿到什么、静态表与实测占用、路径与挂载解析、fd 表、递归锁只在边缘加、错误模型、核心刻意不做的事 |
| `docs/fsdrv.md` | fsdrv 逐项契约、能留 `NULL` 的部分、返回错误的六条规则、完整可编译的 `romfs` 示例、backend 契约、交活前检查清单 |
| `docs/porting.md` | limits/backend/hooks 三步 + 挂载、四种工具链编译配方、实测体积（核心+ramfs 约 14.5 KiB flash / 5.4 KiB .bss）、移植完成的判据分档 |

本文里的 `ARK_E_*`、`ark_vfs_fsdrv_t`、`ark_vfs_hooks_t` 等名字都指上游的公开面，
内核侧只经 `svcrt_vfs.h` 用它。

## App 侧：文件类 POSIX 到这一层的映射

已做（SVC 0x1C sub 11..18）：App 写普通 POSIX，路径以 `/` 开头就走进来。
`open/read/write/lseek/close/stat/fstat/opendir/readdir/closedir/unlink` →
`svcrt_path_*`，容量有限（fd 8 格、目录流 2 格）且表空时如实报 `EMFILE`/`ENOMEM`。
能力表与字段口径见 [POSIX与Windows兼容层说明.md](POSIX与Windows兼容层说明.md) §4.7，
试验件 `example/stm32f427/app_sdk/FS_DEMO/`（**上板验证过**：F427 固定槽 0 覆盖安装后
跑文件类 POSIX 自检，逐项 `ok`，结尾 `result : pass=32 fail=0`）。

## 未做

- App 侧：socket / select **不走这一层**——它经 SVC `0x1E` 打到内核的 socket 服务
  （lwIP 2.1.3，起的是回环网卡），见 [socket与select兼容层.md](socket与select兼容层.md)
  与 [lwIP协议栈接入](lwIP协议栈接入.md)
- 组件升级：`tools/sync_ark_vfs.py --apply` 后跑一次全量编译即可
