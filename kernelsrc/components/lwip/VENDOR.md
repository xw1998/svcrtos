# lwIP（第三方源码，未改动）

本目录是 lwIP 官方发行版的**源码子集**，为 SVCrtOS 提供 TCP/IP 协议栈，
上面承载 socket / select 兼容面（B 面）。

**第三方代码保持原样**：`src/` 下的文件不放任何 `#if SVCRT_USE_LWIP`。
需要按构建裁剪的地方全部落在 `port/` 里（见 lwip_port.c 的整文件门控），
所以关掉开关时是「不编译 port、也不编译这些源文件」，而不是「源码带条件宏」。

| 项 | 值 |
|---|---|
| 来源 | gitee 镜像 `https://gitee.com/mirrors/lwip` |
| 版本 | `STABLE-2_1_3_RELEASE`（lwIP 2.1.3，2020-11 发行） |
| 获取方式 | `.../repository/archive/STABLE-2_1_3_RELEASE` 下载 zip（1,500,159 字节）解压后复制 |
| 许可 | 见 `COPYING`（BSD 三条款） |
| 复制日期 | 2026-09-23 |
| 源码文件数 | 192 |

复制范围：

- `src/core/*.c`：def、dns、init、inet_chksum、ip、mem、memp、netif、pbuf、
  raw、stats、sys、tcp、tcp_in、tcp_out、timeouts、udp
- `src/core/ipv4/*.c`：autoip、dhcp、etharp、icmp、igmp、ip4、ip4_addr、ip4_frag
- `src/api/*.c`：api_lib、api_msg、err、netbuf、netdb、sockets、tcpip
- `src/netif/ethernet.c`
- `src/include/` 整棵树（头文件，构建需要）
- `COPYING`

**不含**：`src/apps/`（httpd/mqtt/sntp 等示例应用）、`test/`、`doc/`、
`contrib/`、`FEATURES`/`CHANGELOG` 等文档。这些都是本次用不到的，
不复制可以少一层「看起来支持其实没接」的误导。

## 构建接入

- `src/**`：由 F427 内核工程 `SVCRTOS_TEST.uvprojx` 的 `lwIP` 组编入；
- `port/**`：同一工程组编入，include 路径 `..\..\..\..\..\kernelsrc\components\lwip\src\include`
  与 `..\..\..\..\..\kernelsrc\components\lwip\port`。

## 内核侧依赖

端口层（`port/sys_arch.c`）建在 SVCrtOS 内核原语上，用到：

- `svcrt_sem_*` / `svcrt_mutex_*` / `svcrt_mq_*`（信号量、互斥、消息队列）
- `svcrt_task_register()`（内核侧建任务；lwIP 的 tcpip 线程与收包线程都靠它起）
- `svcrt_get_time_ms()` / `svcrt_task_wait_period()`

容量相关：`sys_arch.c` 请求的邮箱深度受 `SVCRT_MQ_DEPTH` 限制，超出直接
返回 `ERR_MEM`；线程栈必须静态自管（内核 `svcrt_task_register` 不收堆栈来源），
每槽预置 2 KB 静态栈。

## 回环（当前唯一网卡）

本目录**没有** `src/netif/loopif.c`——2.1.3 把回环网卡内建进了
`src/core/netif.c`：`netif_init()` 会自动建 `lo`（127.0.0.1/8），
但**回环包的收端要外部周期性调用 `netif_poll_all()`**（`src/api/tcpip.c`
自己不会调）。这一步由 `port/lwip_port.c` 的周期任务承担。

真实以太网网卡尚未接入：仓库里没有这块 F427 板带 PHY 的证据，
所以当前只能验证 socket 语义，不能验证真实收发。
