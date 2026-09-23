# socket / select 兼容层（App 侧）

## 一句话

App 里可以直接写 Linux 风格的 socket 程序：`socket/bind/listen/accept/connect/send/recv/
sendto/recvfrom/select/getsockname/getpeername/shutdown/getsockopt/setsockopt/fcntl`
这些名字由 `kernelsrc/sdk/posix` 提供，经 **SVC `0x1E`** 打到内核的 socket 服务
（`kernelsrc/src/svcrt_net.c`），服务再转给内核里的 lwIP。

**结论分档（务必看清）**：当前状态是「**仅编译通过**」——内核侧 F427/F401 两个工程 0 Error，
App 侧 `SOCKET_DEMO` 也 0 Error / 0 Warning 并且 socket 代码确已链接进映像。
**尚未上板、尚未跑过 socket 语义自检**。协议栈只有 lwIP 回环网卡（127.0.0.1/8），
包不出芯片（见 [lwIP协议栈接入.md](lwIP协议栈接入.md)）。

## 1. 头文件与名字

| 头文件 | 提供 |
|---|---|
| `sys/socket.h` | `struct sockaddr`（Linux 布局）、`AF_INET` / `SOCK_STREAM` / `SOCK_DGRAM`、`SOL_SOCKET` / `SO_*`、`MSG_*`、`SHUT_*`、`fd_set` + `FD_ZERO/SET/CLR/ISSET`、15 个函数原型与 POSIX 别名宏 |
| `netinet/in.h` | `struct in_addr` / `struct sockaddr_in`（Linux 布局）、`INADDR_*`、`IPPROTO_*` |
| `arpa/inet.h` | `htons/ntohs/htonl/ntohl`、`inet_addr`、`inet_ntoa`、`inet_pton`、`inet_ntop` |
| `sys/time.h` | `struct timeval`（`select` 超时；定义 `SVCRT_POSIX_NO_TIMEVAL` 可避让工具链版本） |
| `fcntl.h` | `F_GETFL` / `F_SETFL` / `O_NONBLOCK` |
| `errno.h` | socket 相关 errno（`EWOULDBLOCK` / `ECONNREFUSED` / `EAFNOSUPPORT` ...） |
| `sys/stat.h` | `S_IFSOCK` / `S_ISSOCK` |

结构体一律用 **Linux 布局**（没有 BSD 的 `sa_len`）：地址跨界只传两个标量
（宿主序 ip + 宿主序 port），布局自由，选 Linux 布局能让为 Linux/Windows 写的源码零改动编译。
`SOL_SOCKET` 与 `SO_*` 也取 Linux 值。

## 2. 边界（现在只做这些）

- **只有 `AF_INET`**：`socket(AF_INET6, ...)` 明着回错误，不静默接受；
- 没有 DNS（`gethostbyname` / `getaddrinfo` 不在兼容面内），地址写 `inet_addr` 或 `inet_pton`；
- 没有 `poll()`，只有 `select()`；
- 同时存在的 socket 上限是 **4**（内核 lwIP `MEMP_NUM_NETCONN=4`）。

## 3. 内核只登记、用户态轮询

内核的 SVC 处理器**不能阻塞**（见 [同步原语与令牌守恒.md](同步原语与令牌守恒.md)），
所以 socket 面也守同一条线：

- App 发一条请求（完整参数块）进内核的请求槽，**同参数块**轮询到有结果为止；
- 内核请求槽按 **完整参数块** 匹配，不是按 owner + 子命令：否则一个放弃了请求的调用者
  改问别的（或换一个 socket 发同样的 `send`）会拿到上次的回复——那是"看似权威的错答案"；
- 死掉的调用者不再永久占住请求槽（`svcrt_net_release_task()` 把 owner 置 -1，
  下一个调用者丢弃这条孤儿回复）。

**阻塞语义在 App 侧兑现**：`send` / `recv` / `accept` / `connect` 的"等"由
`select`（内核 POLL）+ `svcrt_task_wait(1)` 组合实现；`SO_RCVTIMEO` / `SO_SNDTIMEO`
由 App 侧保存并在这一层生效（内核 socket 调用本身从不阻塞），**0/0 表示永久等待**（POSIX 语义）。
不认识的 `setsockopt` / `getsockopt` 一律回 `ENOPROTOOPT`，内核不认识的选项回 `ENOTSUP`——拒绝，不静默接受。

`O_NONBLOCK` 存在 App 侧 fd 表里，用 `fcntl(fd, F_SETFL, ...)` 设置；
`select()` 对**非 socket** fd（路径 / 设备 / 裸句柄）一概报"就绪"——它们没有"还没好"的状态，
与 POSIX 对普通文件的答案一致。

## 4. SVC `0x1E` wire 契约

契约头是 `kernelsrc/include/svcrt_net_abi.h`（LF，纯 ASCII）。要点：

- 参数块 `SVCRT_NET_ARG_WORDS = 8` 个字；
- 17 个子命令 `0..16`：QUERY / SOCKET / BIND / LISTEN / ACCEPT / CONNECT / SEND / RECV /
  SENDTO / RECVFROM / CLOSE / SETSOCKOPT / GETSOCKOPT / GETSOCKNAME / GETPEERNAME / SHUTDOWN / POLL；
- 错误码 `-200 ~ -215`（`EPENDING` / `EBUSY` / `ENOTSUP` / `EINVAL` / `EWOULDBLOCK` /
  `EINPROGRESS` / `EALREADY` / `EISCONN` / `ENOTCONN` / `ECONNREFUSED` / `ECONNRESET` /
  `ECONNABORTED` / `EHOSTUNREACH` / `EMSGSIZE` / `ENOBUFS` / `EIO`）；
- `SVCRT_NET_MAX_SOCKETS = 4`；`SVCRT_NET_IP(a,b,c,d)` 组合地址。

`svcrt_net_svc` 在 `SVCRT_USE_LWIP != 1` 时是降级 stub：一律回 `SVCRT_NET_ENOTSUP`，
App 侧映射成 `EOPNOTSUPP`。也就是说**内核没编协议栈时，`socket()` 会明着失败**，
而不是给一个假句柄。

## 5. 限制与已知问题

- **fd 数值空间重叠**：`svcrt_posix_open()` 返回的 App fd（3 起）与 `svcrt_dev_open()`
  返回的裸内核句柄数值空间会重叠；同一个数值在 App 侧被当 fd 用时，表内判定优先，
  混用这两种句柄会误判。**不要把 `svcrt_dev_open` 的裸句柄交给 `read`/`write`/`close` 之外的 POSIX 接口。**
- 打开路径时传 `O_NONBLOCK` 会被内核路径标志拒绝（socket 侧不受影响）。
- 多任务共享 lwIP 的全局 `errno`（lwIP sockets 层的已知限制）：判断调用是否失败**看返回值**，
  不要跨任务依赖 `errno`。

## 6. SOCKET_DEMO：一份不改动的 socket 程序

`example/stm32f427/app_sdk/SOCKET_DEMO/Src/socket_demo.c` 是 B 面的试验件（A 面是 `FS_DEMO`，
进程/线程面是 `POSIX_DEMO`）：一份普通的 POSIX sockets 程序，同一个源码在 Linux 上
`cc socket_demo.c` 能编，在这里名字由兼容层回答。

它盖的事：`htons/ntohs` 往返、`inet_addr` / `inet_ntoa`；TCP 回显（socket / `SO_REUSEADDR` /
bind / listen / getsockname / connect / accept / getpeername / 双向 send-recv / `select` 可读 /
`select` 超时 / `shutdown(SHUT_WR)` 对端读 0 / `SO_ERROR`）；UDP（bind / `sendto` / `recvfrom`
含来源端口）；`socket(AF_INET6)` 必须报错。每行打 `ok` / `FAIL`，结尾打 `result : pass=N fail=M`。

## 7. 已验证 / 未验证

| 项 | 结论 | 证据 |
|---|---|---|
| 内核 socket 服务链接通过（F427） | **仅编译通过** | `Code=170258 RO-data=10094 RW-data=676 ZI-data=95584`，0 Error / 1 Warning（既有 `svcrt_context.S A1581W`） |
| 内核回归（F401） | **仅编译通过** | `Code=99354`，0 Error / 1 Warning；F401 未开 `SVCRT_USE_LWIP`，`svcrt_net.c` 走 stub 分支 |
| App 侧 socket 面链接通过 | **仅编译通过** | `SOCKET_DEMO` 0 Error / 0 Warning，`Code=10726`（比 `POSIX_DEMO` 的 7126 多约 3.6 KB，说明 socket 块确已链接进映像） |
| 路由门禁 | 已验证 | `tools/ci_gate.py` 7/7 |
| 上板、socket 语义自检 | **未验证** | 需要烧录（会擦写设备 Flash） |
| 真实以太网收发 | **未验证** | 这块板没有可用的 PHY 证据 |
