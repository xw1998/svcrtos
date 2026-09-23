/**
* @file svcrt_net_abi.h
* @brief SVC 0x1E (network service) wire contract.
* @details What the App side and the kernel side must agree on, in one file
*          so the two can never drift: the sub command numbers, the argument
*          layout of each, the socket option numbers, the poll bits and the
*          error codes. Nothing here is an address, a register or a chip
*          detail - it is a pure protocol.
*
*          Argument block: a uint32 p[8] that lives in the caller's own RAM.
*          The kernel checks the block (svcrt_kernel_svc_args_ok) and every
*          buffer inside it before it dereferences anything.
*
*          One request at a time, system wide, and one owner:
*            - a call that starts a request answers SVCRT_NET_EPENDING;
*            - the caller then re-issues the SAME call (same sub command,
*              same handle) until it gets something else.  The re-issue is
*              how it polls; the kernel itself never waits inside an SVC
*              handler, because PendSV cannot preempt SVC;
*            - a different task (or a different sub command) arriving while
*              a request is in flight gets SVCRT_NET_EBUSY, never the pending
*              code - otherwise it would wait for a reply that is not its own.
*
*          Addresses travel as two scalars - a host order IPv4 address and a
*          host order port - never as a struct.  Only the App side knows
*          struct sockaddr; the boundary stays free of layout agreements.
*
* @note  Shared by kernel and SDK on purpose. It must stay plain C and free
*        of any platform include.
* @author xw
*/
#ifndef __SVCRT_NET_ABI_H__
#define __SVCRT_NET_ABI_H__

/* ---------------------------------------------------------------- layout */
#define SVCRT_NET_ARG_WORDS        (8u)                 /* p[0..7] */

/* ------------------------------------------------------------- sub cmds
 * 0  query      ()                          -> 1 = service up, 0 = not built
 * 1  socket     (p1=type)                   -> handle
 * 2  bind       (p1=handle, p2=ip, p3=port) -> 0
 * 3  listen     (p1=handle, p2=backlog)     -> 0
 * 4  accept     (p1=handle)                 -> new handle
 * 5  connect    (p1=handle, p2=ip, p3=port) -> 0 / EINPROGRESS / ...
 * 6  send       (p1=handle, p2=buf, p3=len, p4=flags) -> sent
 * 7  recv       (p1=handle, p2=buf, p3=len, p4=flags) -> got
 * 8  sendto     (p1=handle, p2=buf, p3=len, p4=ip, p5=port) -> sent
 * 9  recvfrom   (p1=handle, p2=buf, p3=len, p4=ip out, p5=port out) -> got
 * 10 close      (p1=handle)                 -> 0
 * 11 setsockopt (p1=handle, p2=opt, p3=value) -> 0
 * 12 getsockopt (p1=handle, p2=opt, p3=value out) -> 0
 * 13 getsockname(p1=handle, p2=ip out, p3=port out) -> 0
 * 14 getpeername(p1=handle, p2=ip out, p3=port out) -> 0
 * 15 shutdown   (p1=handle, p2=how)         -> 0
 * 16 poll       (p1=handle, p2=flags)       -> ready bits (0 = nothing yet)
 */
#define SVCRT_NET_SUB_QUERY        (0u)
#define SVCRT_NET_SUB_SOCKET       (1u)
#define SVCRT_NET_SUB_BIND         (2u)
#define SVCRT_NET_SUB_LISTEN       (3u)
#define SVCRT_NET_SUB_ACCEPT       (4u)
#define SVCRT_NET_SUB_CONNECT      (5u)
#define SVCRT_NET_SUB_SEND         (6u)
#define SVCRT_NET_SUB_RECV         (7u)
#define SVCRT_NET_SUB_SENDTO       (8u)
#define SVCRT_NET_SUB_RECVFROM     (9u)
#define SVCRT_NET_SUB_CLOSE        (10u)
#define SVCRT_NET_SUB_SETSOCKOPT   (11u)
#define SVCRT_NET_SUB_GETSOCKOPT   (12u)
#define SVCRT_NET_SUB_GETSOCKNAME  (13u)
#define SVCRT_NET_SUB_GETPEERNAME  (14u)
#define SVCRT_NET_SUB_SHUTDOWN     (15u)
#define SVCRT_NET_SUB_POLL         (16u)

/* ------------------------------------------------------------ families */
#define SVCRT_NET_AF_INET          (2)
#define SVCRT_NET_SOCK_STREAM      (1)
#define SVCRT_NET_SOCK_DGRAM       (2)

/* ------------------------------------------------------------- options */
#define SVCRT_NET_SO_REUSEADDR     (1)
#define SVCRT_NET_SO_RCVBUF        (2)
#define SVCRT_NET_SO_SNDBUF        (3)
#define SVCRT_NET_SO_ERROR         (4)

/* ------------------------------------------------------------ shutdown */
#define SVCRT_NET_SHUT_RD          (0)
#define SVCRT_NET_SHUT_WR          (1)
#define SVCRT_NET_SHUT_RDWR        (2)

/* ---------------------------------------------------------------- poll
 * In: which events the caller asks about.  Out: which of them are true. */
#define SVCRT_NET_POLL_IN          (1u)
#define SVCRT_NET_POLL_OUT         (2u)
#define SVCRT_NET_POLL_ERR         (4u)

/* -------------------------------------------------------------- errors
 * Negative, and disjoint from SVCRT_SYNC_ERR_* (-1..-4). The App layer
 * translates each into the matching errno; the kernel never returns a raw
 * lwIP errno. */
#define SVCRT_NET_EPENDING         (-200)  /* request taken, poll again */
#define SVCRT_NET_EBUSY            (-201)  /* someone else's request in flight */
#define SVCRT_NET_ENOTSUP          (-202)
#define SVCRT_NET_EINVAL           (-203)
#define SVCRT_NET_EWOULDBLOCK      (-204)
#define SVCRT_NET_EINPROGRESS      (-205)
#define SVCRT_NET_EALREADY         (-206)
#define SVCRT_NET_EISCONN          (-207)
#define SVCRT_NET_ENOTCONN         (-208)
#define SVCRT_NET_ECONNREFUSED     (-209)
#define SVCRT_NET_ECONNRESET       (-210)
#define SVCRT_NET_ECONNABORTED     (-211)
#define SVCRT_NET_EHOSTUNREACH     (-212)
#define SVCRT_NET_EMSGSIZE         (-213)
#define SVCRT_NET_ENOBUFS          (-214)
#define SVCRT_NET_EIO              (-215)

/* System wide socket ceiling: memory bounded by MEMP_NUM_NETCONN in
 * lwipopts.h, not by this number. */
#define SVCRT_NET_MAX_SOCKETS      (4)

/* Host order IPv4 address from four octets, e.g. SVCRT_NET_IP(127,0,0,1). */
#define SVCRT_NET_IP(a, b, c, d)   ((((unsigned long)(a)) << 24) | \
                                    (((unsigned long)(b)) << 16) | \
                                    (((unsigned long)(c)) << 8)  | \
                                    ((unsigned long)(d)))

#endif /* __SVCRT_NET_ABI_H__ */
