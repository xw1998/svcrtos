# lwIP 协议栈接入（socket / select 面的底座）

## 一句话

内核里编进了 lwIP 2.1.3（`kernelsrc/components/lwip`），由 `SVCRT_USE_LWIP` 门控；
现在起了一张**回环**网卡（127.0.0.1/8），还没有真实以太网收发。

**结论分档（务必看清）**：当前状态是「**上板验证过**」——F427 内核工程 0 Error，
F401 内核工程回归 0 Error；F427 上装 `SOCKET_DEMO`（固定槽 0 覆盖安装）跑过 socket
语义自检，`result : pass=36 fail=0`（逐项证据见
[socket与select兼容层.md](socket与select兼容层.md) §7）。

## 1. 为什么是 lwIP，为什么先做回环

- App 兼容面的 B 面（socket / select）不能没有 TCP/IP 栈，仓库里原本没有；
- 全功能 OS 模式是唯一有 sockets API 的路（`NO_SYS=1` 只有 raw API）；
- 这块 F427 板**没有以太网 PHY 的证据**：仓库里只有 CubeMX 留下的 ETH/PHY 宏，
  没有任何 ETH 初始化，内核工程也没编 `stm32f4xx_hal_eth.c`。
  所以先验的是**协议栈与 socket 语义**（连接/收发/关闭/超时走同一条 TCP 状态机），
  真实收发等有网卡的硬件再接。

## 2. 目录与来源

| 路径 | 内容 |
|---|---|
| `kernelsrc/components/lwip/src/` | lwIP 官方源码子集（192 文件，未改动，见同目录 `VENDOR.md`） |
| `kernelsrc/components/lwip/port/` | 本板端口层：`lwipopts.h`、`arch/cc.h`、`arch/sys_arch.h`、`sys_arch.c`、`lwip_port.[ch]` |

第三方源码里**不放任何 `#if SVCRT_USE_LWIP`**（改第三方代码的代价比收益大）；
按构建裁剪的地方全在 `port/`，关掉开关时是不编译 port 层。

## 3. 构建接入（F427 内核工程）

- `SVCRTOS/lwIP`：`src/**` 下 33 个 `.c`；`SVCRTOS/lwIP Port`：`port/**` 下 2 个 `.c`；
- include 路径：`kernelsrc\components\lwip\src\include`、`kernelsrc\components\lwip\port`；
- 工程宏：`SVCRT_USE_LWIP=1`（`svcrt_features.h` 里默认是 **0**，不想要就撤掉这个宏）。

## 4. 端口层的三条口径

1. **超时语义要换算**：lwIP 说 `timeout==0` 是"永久等待"，SVCrtOS 说 0 是"不等待"、
   负值才是永久。所有入口过 `lwip_to_svcrt_timeout()`，谁也别自己写死参数。
2. **失败不装成功**：内核对象创建失败、句柄非法、超时以外的错误码，一律回 lwIP 认的
   失败值，并往内核日志留一条带错误码的记录。邮箱请求的深度超过
   `SVCRT_MQ_DEPTH`（8）直接回 `ERR_MEM`，**不悄悄截短**；线程栈要不到就直接
   `SYS_THREAD_NULL`。
3. **内核侧只用 `*_internal`**：`svcrt_sem_create()` 这些裸名字是 **App SDK 的 SVC 封装**
   （`kernelsrc/sdk/app_sdk/svcrt_oslib.c`），内核里链接不到。内核侧调的是
   `svcrt_sem_create_internal()` / `svcrt_mtx_*_internal()` / `svcrt_mq_*_internal()` /
   `svcrt_task_wait_period_internal()` / `svcrt_kernel_get_time()`。

另外两处必须知道的事实：

- `sys_now()` 走 `svcrt_kernel_get_time()`（内核里由 tick 换算出的毫秒）；
- `errno` 是**全局一个变量**（`LWIP_PROVIDE_ERRNO=1` 时由端口提供定义）。多任务共享
  `errno` 是 lwIP sockets 层自己的已知限制，我们没做每任务副本——判断 socket 调用
  是否失败**看返回值**，不要跨任务依赖 `errno`。

## 5. 回环是"多线程模式"，不需要外部轮询

`LWIP_NETIF_LOOPBACK_MULTITHREADING` 在 `lwipopts.h` 里**显式写死 1**：

- lo 网卡由 `lwip_init()` → `netif_init()` 自动建好（地址 127.0.0.1/8，input 接 `tcpip_input`）；
- 回环发送 `netif_loop_output()` 把 pbuf 挂上 loop 链表后，用
  `tcpip_try_callback(netif_poll)` **交给 tcpip 线程自己喂回协议栈**；
- 因此**不需要**任何人在主循环里周期调 `netif_poll_all()`。

> 这一条是纠错记录：多线程模式下 `netif_poll_all()` 连声明都没有
> （`netif.h` 里它在 `#if !LWIP_NETIF_LOOPBACK_MULTITHREADING` 里面），
> 照抄"回环必须自己轮询"的说法会编译不过。只有轮询模式（`=0`）才要自己调。

`port/lwip_port.c` 因此只做一件事：在一个内核任务的第一轮里调一次 `tcpip_init()`。
**不能在 `main()` 里调**——它会阻塞等 tcpip 线程把初始化做完（内部信号量），
而 `main()` 跑在调度器启动之前，等不到任何任务。任务之后按 1 s 周期躺着
（内核没有"结束自己"的语义），将来接真实网卡时收包搬运落在这个循环里。

## 6. 代价：内核区 128K → 256K

lwIP 让内核代码从 `Code=113214` 涨到 `Code=145222`（+ 约 32 KB）。
128 KB 的内核区装不下，链接报：

```
Error: L6407E: Sections of aggregate size 0x5de0 bytes could not fit into .ANY selector(s).
```

MPU 区域要求 2 的幂（`svcrt_partition.h` 的 `svcrt_mpu_window_check`），
128K 的下一档只能是 256K，所以：

| 分区 | 改动前 | 改动后 |
|---|---|---|
| KERNEL | 128 KB（`0x08000000~0x0801FFFF`） | **256 KB**（`0x08000000~0x0803FFFF`） |
| CONFIG | 128 KB（`0x08020000`） | 128 KB（`0x08040000`） |
| IMAGE_POOL | 768 KB（`0x08040000`，6 扇区） | **640 KB**（`0x08060000`，5 扇区） |
| 池可分配（扣 1 扇区压实余量） | 640 KB | 512 KB |

**连带影响**：设备端配置区（`config` 分区）与池基址整体上移，板上原来写的布局配置
与已安装镜像**都对不上**，需要重新写配置 / 重装镜像。不想要这个代价就把工程宏
`SVCRT_USE_LWIP` 撤掉，并把 `KERNEL_SIZE` 用 `-D` 覆盖回 128K。

## 7. 已验证 / 未验证

| 项 | 结论 | 证据 |
|---|---|---|
| F427 内核工程链接通过 | **仅编译通过** | `Code=170258 RO-data=10094 RW-data=676 ZI-data=95584`，0 Error / 1 Warning（`svcrt_context.S A1581W`，既有）；这是**含 socket 服务（SVC 0x1E）**的当前构建 |
| `.sct` 随配置头重算 | 已验证 | `build/kernel.sct` 生成 `LR_KERNEL 0x08000000 0x00040000` |
| F401 内核工程回归 | 仅编译通过 | `Code=100102`，0 Error / 1 Warning。F401 配置里 `SVCRT_USE_LWIP` 未开，`svcrt_net.c` 走 stub 分支 |
| 路由门禁 | 已验证 | `tools/ci_gate.py` 7/7 |
| 上板、socket 语义自检 | **上板验证过** | F427 + `SOCKET_DEMO`（固定槽 0 覆盖安装）：`[LWIP:45] tcpip up, loopif 127.0.0.1` → `[NET:1028] socket service up (SVC 0x1E)` → App 自检，`result : pass=36 fail=0`；逐项见 [socket与select兼容层.md](socket与select兼容层.md) §7 |
| 真实以太网收发 | **未验证** | 这块板没有可用的 PHY 证据 |

## 8. 容量与限制（记住这几条，接 App 面时会撞上）

- 线程只有 **2 个槽位**（tcpip 线程 + 预留给将来网卡驱动线程），每槽 **2 KB 静态栈**——
  内核只接受调用者自己的栈，所以栈是 `sys_arch.c` 里的静态数组；
- 各类邮箱深度默认 8，上限是 `SVCRT_MQ_DEPTH`（8）；
- `MEM_SIZE=8 KB`、`PBUF_POOL_SIZE=8`、`MEMP_NUM_TCP_SEG=16`、`MEMP_NUM_TCP_PCB=4`、
  `MEMP_NUM_NETCONN=2`——这些是当前内核 RAM 余量下的取值，接真实网卡时要重新核算。
