/**
* @file lwipopts.h
* @brief lwIP 在这块板上的配置：要什么、不要什么，以及为什么。
* @details 这是**内核侧**的配置（lwIP 编进内核映像，跟着 SVC 一起对外服务），
*          不是给 App 用的。三条口径：
*            - 只用 IPv4：IPv6 在这个目标上没有使用者，开着只是多占 RAM；
*            - 全功能 OS 模式（NO_SYS=0）：要 sockets API 就必须有 tcpip 线程，
*              没有第二条路（NO_SYS=1 只有 raw API）；
*            - 所有 mailbox 尺寸不得超过 SVCRT_MQ_DEPTH（内核消息队列深度 8），
*              sys_mbox_new() 会拒绝超出的请求，而不是悄悄截短。
*
*          内存：MEM_SIZE + PBUF_POOL 是本文件里唯二"按字节吃 RAM"的量，
*          合计约 8 KB + 8×~1.5 KB。板子 RAM 余量见 docs/内存与分区.md。
*
* @author xw
* @date 2026.09.23
*/
#ifndef LWIP_LWIPOPTS_H
#define LWIP_LWIPOPTS_H

/* ============================================================
 * 1. 运行模式：带 OS 的 sockets
 * ============================================================ */
#define NO_SYS                          0
#define SYS_LIGHTWEIGHT_PROT            1   /* sys_arch.h 用 PRIMASK 提供 */
#define LWIP_NETCONN                    1   /* netconn API：sockets 的底座 */
#define LWIP_SOCKET                     1
#define LWIP_SOCKET_SELECT              1
#define LWIP_SOCKET_POLL                0   /* 不提供 poll，只做 select */
#define LWIP_COMPAT_SOCKETS             0   /* 内核里不要 socket() 这些裸名字 */
#define LWIP_POSIX_SOCKETS_IO_NAMES     0
#define LWIP_TCPIP_CORE_LOCKING         0   /* 用 tcpip 线程的 mbox，不做全局锁 */

/* ============================================================
 * 2. 协议：IPv4 + TCP/UDP，其余一律关掉
 * ============================================================ */
#define LWIP_IPV4                       1
#define LWIP_IPV6                       0
#define LWIP_ARP                        1
#define LWIP_ETHERNET                   1
#define LWIP_ICMP                       1
#define LWIP_RAW                        1
#define LWIP_UDP                        1
#define LWIP_TCP                        1
#define LWIP_DNS                        0
#define LWIP_DHCP                       0
#define LWIP_AUTOIP                     0
#define LWIP_IGMP                       0
#define LWIP_ALTCP                      0
#define IP_REASSEMBLY                   0   /* 分片重组：内存换不来收益，关 */
#define IP_FRAG                         0
#define LWIP_TCP_KEEPALIVE              0
#define LWIP_TCP_TIMESTAMPS             0

/* ============================================================
 * 3. 回环：没有网卡也要能谈 socket
 * ============================================================ */
#define LWIP_NETIF_LOOPBACK             1
#define LWIP_HAVE_LOOPIF                1   /* netif.c 里自带 lo 网卡初始化 */
#define LWIP_LOOPIF_MULTICAST           0
#define LWIP_LOOPBACK_MAX_PBUFS         8

/* ============================================================
 * 4. 内存：定长池，不用 C 库堆
 * ============================================================ */
#define MEM_LIBC_MALLOC                 0
#define MEMP_MEM_MALLOC                 0
#define MEM_ALIGNMENT                   4
#define MEM_SIZE                        8192
#define MEMP_OVERFLOW_CHECK             0
#define MEMP_SANITY_CHECK               0
#define PBUF_POOL_SIZE                  8
#define PBUF_POOL_BUFSIZE               LWIP_MEM_ALIGN_SIZE(TCP_MSS + 40 + PBUF_LINK_ENCAPSULATION_HLEN)
#define PBUF_LINK_HLEN                  14
#define TCP_MSS                         1460
#define TCP_WND                         (2 * TCP_MSS)
#define TCP_SND_BUF                     (2 * TCP_MSS)
#define TCP_SND_QUEUELEN                ((4 * TCP_SND_BUF + (TCP_MSS - 1)) / TCP_MSS)
#define LWIP_LISTEN_BACKLOG             2
#define MEMP_NUM_TCP_SEG                16
#define MEMP_NUM_TCP_PCB                4
#define MEMP_NUM_TCP_PCB_LISTEN         2
#define MEMP_NUM_UDP_PCB                2
#define MEMP_NUM_REASSDATA              2
#define MEMP_NUM_NETBUF                 2
#define MEMP_NUM_NETCONN                4   /* = 同时存在的 socket 数上限。回环回显自检要同时占 3 个 netconn
                                               （监听 + 被 accept 的 + 客户端），留 1 个余量。
                                               每个 netconn 一个 mailbox：4 + tcpip 线程 1 = 5 <= SVCRT_MQ_NUM(8)。 */
#define MEMP_NUM_TCPIP_MSG_API          8
#define MEMP_NUM_TCPIP_MSG_INPKT        8
#define MEMP_NUM_SYS_TIMEOUT            8

/* ============================================================
 * 5. 线程与 mailbox：尺寸受内核对象数约束，见 lwip_port.h 的说明
 * ============================================================ */
#define TCPIP_THREAD_NAME               "lwip"
#define TCPIP_THREAD_STACKSIZE          1024
#define TCPIP_THREAD_PRIO               12
#define TCPIP_MBOX_SIZE                 8   /* == SVCRT_MQ_DEPTH，不能更大 */
#define DEFAULT_THREAD_STACKSIZE        1024
#define DEFAULT_THREAD_PRIO             16
#define DEFAULT_UDP_RECVMBOX_SIZE       8
#define DEFAULT_TCP_RECVMBOX_SIZE       8
#define DEFAULT_ACCEPTMBOX_SIZE         8

/* ============================================================
 * 6. socket 选项与杂项
 * ============================================================ */
#define LWIP_SO_RCVTIMEO                1
#define LWIP_SO_SNDTIMEO                1
#define LWIP_SO_RCVBUF                  1
#define LWIP_SO_REUSE                   1
#define SO_REUSE                        1
#define LWIP_TIMEVAL_PRIVATE            1   /* 用 lwIP 自己的 timeval，别和 App 侧撞 */
#define LWIP_NETIF_HOSTNAME             0
#define LWIP_NETIF_STATUS_CALLBACK      0
#define LWIP_NETIF_LINK_CALLBACK        0
#define LWIP_NETIF_REMOVE_CALLBACK      0
#define LWIP_NETIF_API                  0
#define LWIP_NETIF_TX_SINGLE_PBUF       0
#define LWIP_CHECKSUM_CTRL_PER_NETIF    0
#define LWIP_CHKSUM_ALGORITHM           3
#define LWIP_STATS                      0
#define LWIP_STATS_DISPLAY              0
#define LWIP_DEBUG                      0
/* 注意：不要在这里定义 `LWIP_ERROR`。
 * lwIP 自己就有一个同名函数式宏 `LWIP_ERROR(msg, expr, handler)`，
 * 它和 `LWIP_PLATFORM_ERROR` 一起被 `#ifndef LWIP_ERROR` 罩住了。
 * 在这里写 `#define LWIP_ERROR 1`（当成开关），
 * 会把真正的宏定义挡在门外，然后每一处
 * 用到它的地方都报 #109。 */
#define LWIP_ASSERT_CORE_LOCKED()       do { } while(0)
#define LWIP_DISABLE_TCP_SANITY_CHECKS  1

/* 回环包的投递方式。置 1（多线程）：netif_loop_output() 把 pbuf 挂上 loop
 * 链表后，用 tcpip_try_callback(netif_poll) 交给 tcpip 线程自己喂回协议栈——
 * 不需要任何人周期调 netif_poll()。置 0（轮询）才要求应用在自己的循环里
 * 调 netif_poll_all()。本构建是 OS + tcpip 线程，所以是 1；而且这里显式写死，
 * 不依赖 opt.h 那个 `= (!NO_SYS)` 的默认值推导——这个值决定 port 侧要不要
 * 再起一个轮询任务，让它跟着 NO_SYS 隐式翻转不是好事。 */
#define LWIP_NETIF_LOOPBACK_MULTITHREADING 1

/* 内核里没有半主机重定向：printf 会去敲调试器。所有 lwIP 的诊断出口
 * 一律走内核日志（svcrt_log），断言走一个可下断点的钩子。 */
void svcrt_lwip_assert_hook(const char *msg, int line, const char *file);
#define LWIP_PLATFORM_ASSERT(x)  svcrt_lwip_assert_hook((x), __LINE__, __FILE__)
#define LWIP_PLATFORM_DIAG(x)    do { } while(0)

#endif /* LWIP_LWIPOPTS_H */
