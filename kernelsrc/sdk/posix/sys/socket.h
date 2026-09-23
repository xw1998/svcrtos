/**
* @file sys/socket.h
* @brief POSIX sockets for SVCrtOS Apps.
* @details The App talks to the kernel's socket service (SVC 0x1E). The wire
*          contract is in svcrt_net_abi.h; what matters to a program is the
*          list below, because these are the places where "portable source"
*          meets a small RTOS:
*
*            - Only AF_INET with SOCK_STREAM or SOCK_DGRAM exist. socket()
*              refuses anything else instead of handing back a descriptor
*              that would fail later, somewhere less obvious.
*            - Descriptors come from the same table as open()/read()/write(),
*              so close(), read(), write(), fstat() and select() see sockets
*              too. One descriptor space, not two.
*            - A socket belongs to the thread that created it. The kernel
*              pins the handle to its owner, so using it from another thread
*              answers EINVAL rather than silently crossing the boundary.
*            - The kernel's socket calls never block (an SVC handler cannot
*              yield). Blocking is composed here, in the App, out of polls.
*              That is why SO_RCVTIMEO / SO_SNDTIMEO are honoured by this
*              layer even though the kernel never sees them.
*            - A blocking call with no timeout waits for ever, exactly as
*              POSIX says. Arm SO_RCVTIMEO / SO_SNDTIMEO when the program
*              must not hang, and use O_NONBLOCK (through fcntl) for the
*              calls that must not wait at all.
*            - Whether a TCP/IP stack exists at all is a *kernel* build
*              option (SVCRT_USE_LWIP), and an App image is not rebuilt per
*              kernel configuration. So this layer always compiles and
*              socket() answers EOPNOTSUPP on a kernel without sockets,
*              instead of the App failing to link.
*
* @note  Addresses are reduced to two scalars - a host order IPv4 address and
*        a host order port - before they cross the SVC boundary, so the
*        kernel and the App are each free to lay out their own sockaddr.
*        The layout here is Linux's on purpose: that is what the source of a
*        portable program already assumes.
* @author xw
*/
#ifndef __SVCRT_SYS_SOCKET_H__
#define __SVCRT_SYS_SOCKET_H__

#include "svcrt_posix_types.h"
#include "sys/time.h"

/* ---------------------------------------------------------------- types */
#ifndef SVCRT_POSIX_NO_STD_TYPES
typedef uint16 sa_family_t;
typedef uint32 socklen_t;
#endif

/* The generic address. Linux shape (no sa_len), for the reason in the file
 * comment: nothing here is ever handed to the kernel, so the layout is free
 * to be the one the source expects. */
struct sockaddr
{
    sa_family_t sa_family;
    char        sa_data[14];
};

/* -------------------------------------------------------------- domains */
#define AF_UNSPEC   0
#define AF_INET     2
#define PF_INET     AF_INET
/* Named so that a program which mentions it still compiles; socket() refuses
 * it rather than pretending an IPv6 stack is behind it. */
#define AF_INET6    10

/* ---------------------------------------------------------------- types */
#define SOCK_STREAM   1
#define SOCK_DGRAM    2
#define SOCK_RAW      3     /* named, refused by socket() */

/* ------------------------------------------------------- protocol level.
 * Linux's number, so a program that passes SOL_SOCKET keeps its meaning. */
#define SOL_SOCKET    1

/* ------------------------------------------------- socket level options.
 * Numbers are Linux's. Those with no support here are answered ENOPROTOOPT
 * rather than accepted and ignored: an App that believes its receive timeout
 * is armed, when it is not, is worse off than one that is told so. */
#define SO_REUSEADDR  2
#define SO_TYPE       3
#define SO_ERROR      4
#define SO_SNDBUF     7
#define SO_RCVBUF     8
#define SO_RCVTIMEO   20
#define SO_SNDTIMEO   21

/* -------------------------------------------------------- message flags.
 * Passed on to the kernel, which hands them to the stack. */
#define MSG_OOB       1
#define MSG_PEEK      2
#define MSG_DONTWAIT  0x40
#define MSG_WAITALL   0x100
#define MSG_NOSIGNAL  0x4000
#define MSG_MORE      0x8000

/* ------------------------------------------------------------- shutdown */
#define SHUT_RD   0
#define SHUT_WR   1
#define SHUT_RDWR 2

/* ---------------------------------------------------------------- fd_set
 * One 32 bit word. Descriptor numbers in this App layer start at 3 and the
 * table holds 8, so nothing reachable from here comes close to the word
 * limit; nfds above FD_SETSIZE is refused by select() rather than masked
 * into a bit that means another descriptor. */
#ifndef SVCRT_POSIX_NO_FD_SET
#define FD_SETSIZE 32

typedef struct
{
    uint32 fds_bits[1];
} fd_set;

#define FD_ZERO(set)       ((set)->fds_bits[0] = 0u)
#define FD_SET(fd, set)    ((set)->fds_bits[0] |= (1u << (uint32)(fd)))
#define FD_CLR(fd, set)    ((set)->fds_bits[0] &= ~(1u << (uint32)(fd)))
#define FD_ISSET(fd, set)  ((((set)->fds_bits[0]) >> (uint32)(fd)) & 1u)
#endif

/* ------------------------------------------------------------------ API */
int svcrt_posix_socket(int domain, int type, int protocol);
int svcrt_posix_bind(int fd, const struct sockaddr *addr, socklen_t len);
int svcrt_posix_listen(int fd, int backlog);
int svcrt_posix_accept(int fd, struct sockaddr *addr, socklen_t *len);
int svcrt_posix_connect(int fd, const struct sockaddr *addr, socklen_t len);
svcrt_ssize_t svcrt_posix_send(int fd, const void *buf, uint32 len, int flags);
svcrt_ssize_t svcrt_posix_recv(int fd, void *buf, uint32 len, int flags);
svcrt_ssize_t svcrt_posix_sendto(int fd, const void *buf, uint32 len, int flags,
                                 const struct sockaddr *addr, socklen_t addrlen);
svcrt_ssize_t svcrt_posix_recvfrom(int fd, void *buf, uint32 len, int flags,
                                   struct sockaddr *addr, socklen_t *addrlen);
int svcrt_posix_setsockopt(int fd, int level, int optname,
                           const void *optval, socklen_t optlen);
int svcrt_posix_getsockopt(int fd, int level, int optname,
                           void *optval, socklen_t *optlen);
int svcrt_posix_getsockname(int fd, struct sockaddr *addr, socklen_t *len);
int svcrt_posix_getpeername(int fd, struct sockaddr *addr, socklen_t *len);
int svcrt_posix_shutdown(int fd, int how);
int svcrt_posix_select(int nfds, fd_set *rfds, fd_set *wfds, fd_set *efds,
                       struct timeval *timeout);

/* Aliases so plain POSIX source compiles unchanged. */
#define socket(d, t, p)             svcrt_posix_socket((d), (t), (p))
#define bind(fd, a, l)              svcrt_posix_bind((fd), (a), (l))
#define listen(fd, b)               svcrt_posix_listen((fd), (b))
#define accept(fd, a, l)            svcrt_posix_accept((fd), (a), (l))
#define connect(fd, a, l)           svcrt_posix_connect((fd), (a), (l))
#define send(fd, b, n, f)           svcrt_posix_send((fd), (b), (n), (f))
#define recv(fd, b, n, f)           svcrt_posix_recv((fd), (b), (n), (f))
#define sendto(fd, b, n, f, a, al)  svcrt_posix_sendto((fd), (b), (n), (f), (a), (al))
#define recvfrom(fd, b, n, f, a, al) svcrt_posix_recvfrom((fd), (b), (n), (f), (a), (al))
#define setsockopt(fd, lv, o, v, ol) svcrt_posix_setsockopt((fd), (lv), (o), (v), (ol))
#define getsockopt(fd, lv, o, v, ol) svcrt_posix_getsockopt((fd), (lv), (o), (v), (ol))
#define getsockname(fd, a, l)       svcrt_posix_getsockname((fd), (a), (l))
#define getpeername(fd, a, l)       svcrt_posix_getpeername((fd), (a), (l))
#define shutdown(fd, h)             svcrt_posix_shutdown((fd), (h))
#define select(n, r, w, e, t)       svcrt_posix_select((n), (r), (w), (e), (t))

#endif /* __SVCRT_SYS_SOCKET_H__ */
