/**
* @file svcrt_net.c
* @brief Kernel side of the network service (SVC 0x1E): a request slot, a
*        service task and a socket table, on top of the lwIP built into the
*        kernel image.
*
*        Why it is shaped like this (and why it is not "call lwIP inside the
*        SVC handler"):
*          - an SVC handler cannot block. PendSV never preempts SVC, so a
*            task that waits inside the handler can never be resumed
*            (svcrt_hal.h, svcrt_sync_in_handler). lwIP's socket API always
*            waits on the tcpip thread's completion semaphore
*            (api_lib.c netconn_apimsg -> tcpip_send_msg_wait_sem), so a
*            direct call from the handler would fail with a bogus timeout;
*          - therefore the handler only takes the request (and answers
*            SVCRT_NET_EPENDING), and a normal kernel task performs the lwIP
*            call in thread mode where waiting is legal. The App side polls
*            the same call until the reply is there - same idea as the
*            semaphore/mutex "register + WOULDBLOCK" contract, with one
*            extra executor.
*
*        One request in flight, system wide, with a known owner. That keeps
*        the whole thing allocation free and easy to reason about; the price
*        (a second caller gets SVCRT_NET_EBUSY instead of being queued) is
*        deliberate. Queuing would need a reply slot per waiter, and a
*        half-done queue is worse than an honest refusal.
*
*        Every socket is switched to O_NONBLOCK as soon as it exists, so the
*        service task never sits inside lwIP and the slot frees immediately.
*        Blocking, timeouts and retries are the App's business.
*
* @note  Compiled for every kernel target; with SVCRT_USE_LWIP=0 the whole
*        thing degrades to "service not built in" and SVCRT_NET_ENOTSUP, so
*        the dispatcher in svcrt_task.c needs no conditional of its own.
* @author xw
*/
#include "svcrt_features.h"
#include "svcrt_def.h"
#include "svcrt_hal.h"
#include "svcrt_cfg.h"
#include "svcrt_task.h"
#include "svcrt_log.h"
#include "svcrt_kernel_check.h"
#include "svcrt_net.h"

#if (SVCRT_USE_LWIP == 1)

#include "svcrt_mq.h"
#include "lwip/sockets.h"
#include "lwip/def.h"

/* ============================================================
 * 1. state
 * ============================================================ */

/* One row per socket the kernel hands out. fd < 0 = free row.
 * owner is the task that created it; owner < 0 marks an orphan whose owner
 * went away - it is still open (only the service task may touch lwIP) but
 * no caller can reach it any more, and the service task closes it. */
typedef struct {
    int32 fd;
    int32 owner;
} svcrt_net_sock_t;

/* fd < 0 means "this row is free" (see svcrt_net_claim / svcrt_net_own), so
 * the table must NOT start out as all zeros: with the default zero fill every
 * row already looks taken, and the very first socket() comes back ENOBUFS
 * while the socket layer itself is perfectly healthy. -1 is the only correct
 * start value. */
static svcrt_net_sock_t g_net_sock[SVCRT_NET_MAX_SOCKETS] = {
    { -1, 0 }, { -1, 0 }, { -1, 0 }, { -1, 0 }
};

/* The initialiser above writes one pair per row. If the row count moves, this
 * stops compiling instead of silently leaving the extra rows at the all-zero
 * default - where they would look taken, which is the bug being guarded. */
typedef char svcrt_net_sock_rows_must_match_init[SVCRT_NET_MAX_SOCKETS == 4 ? 1 : -1];

#define SVCRT_NET_REQ_IDLE      (0u)
#define SVCRT_NET_REQ_PENDING   (1u)
#define SVCRT_NET_REQ_DONE      (2u)

#define SVCRT_NET_MSG_REQUEST   (1u)
#define SVCRT_NET_MSG_REAP      (2u)

static volatile uint32 g_net_req_state;
static int32  g_net_req_owner;
static volatile int32 g_net_req_result;
static uint32 g_net_req_arg[SVCRT_NET_ARG_WORDS];

static int32  g_net_mq = -1;
static uint32 g_net_task_stack[SVCRT_NET_TASK_STACK_WORDS];

/* ============================================================
 * 2. helpers
 * ============================================================ */

/* The row a handle names, provided it is live and belongs to owner. */
static svcrt_net_sock_t *svcrt_net_own(uint32 handle, int32 owner)
{
    if(owner < 0)
    {
        /* No owner means nobody may use it. An orphan is closed by the
         * service task; a request that outlived its caller must not reach it
         * through the ownerless mark. */
        return 0;
    }
    if((handle == 0u) || (handle > (uint32)SVCRT_NET_MAX_SOCKETS))
    {
        return 0;
    }
    if(g_net_sock[handle - 1u].fd < 0)
    {
        return 0;
    }
    if(g_net_sock[handle - 1u].owner != owner)
    {
        return 0;
    }
    return &g_net_sock[handle - 1u];
}

/* Claim a free row for a fresh lwIP descriptor. Returns the handle or a
 * negative SVCRT_NET_E*. */
static int32 svcrt_net_claim(int32 fd, int32 owner)
{
    uint32 i;

    for(i = 0u; i < SVCRT_NET_MAX_SOCKETS; i++)
    {
        if(g_net_sock[i].fd < 0)
        {
            g_net_sock[i].fd = fd;
            g_net_sock[i].owner = owner;
            return (int32)(i + 1u);
        }
    }
    return SVCRT_NET_ENOBUFS;
}

/* lwIP's errno -> our wire error. Never pass a raw lwIP errno across the SVC
 * boundary: the App side has its own errno numbering and no lwIP. */
static int32 svcrt_net_from_errno(int e)
{
    switch(e)
    {
    case EWOULDBLOCK:   return SVCRT_NET_EWOULDBLOCK;
    case EINPROGRESS:   return SVCRT_NET_EINPROGRESS;
    case EALREADY:      return SVCRT_NET_EALREADY;
    case EISCONN:       return SVCRT_NET_EISCONN;
    case ENOTCONN:      return SVCRT_NET_ENOTCONN;
    case ECONNREFUSED:  return SVCRT_NET_ECONNREFUSED;
    case ECONNRESET:    return SVCRT_NET_ECONNRESET;
    case ECONNABORTED:  return SVCRT_NET_ECONNABORTED;
    case EHOSTUNREACH:  return SVCRT_NET_EHOSTUNREACH;
    case EMSGSIZE:      return SVCRT_NET_EMSGSIZE;
    case ENOBUFS:       return SVCRT_NET_ENOBUFS;
    case EINVAL:        return SVCRT_NET_EINVAL;
    default:            return SVCRT_NET_EIO;
    }
}

static void svcrt_net_addr_in(struct sockaddr_in *sa, uint32 ip, uint32 port)
{
    sa->sin_len         = (u8_t)sizeof(struct sockaddr_in);
    sa->sin_family      = AF_INET;
    sa->sin_port        = lwip_htons((u16_t)port);
    sa->sin_addr.s_addr = lwip_htonl(ip);
}

static void svcrt_net_addr_out(const struct sockaddr_in *sa,
                               uint32 *p_ip, uint32 *p_port)
{
    if(p_ip != 0)
    {
        *p_ip = (uint32)lwip_ntohl(sa->sin_addr.s_addr);
    }
    if(p_port != 0)
    {
        *p_port = (uint32)lwip_ntohs(sa->sin_port);
    }
}

static void svcrt_net_reap(void)
{
    uint32 i;

    for(i = 0u; i < SVCRT_NET_MAX_SOCKETS; i++)
    {
        if((g_net_sock[i].fd >= 0) && (g_net_sock[i].owner < 0))
        {
            (void)lwip_close(g_net_sock[i].fd);
            g_net_sock[i].fd = -1;
            g_net_sock[i].owner = 0;
        }
    }
}

/* ============================================================
 * 3. the operations (thread mode, inside the service task)
 * ============================================================ */

static int32 svcrt_net_do_socket(int32 owner, uint32 type)
{
    int   fd;
    int32 handle;

    fd = lwip_socket(AF_INET,
                     (type == (uint32)SVCRT_NET_SOCK_DGRAM) ? SOCK_DGRAM
                                                            : SOCK_STREAM,
                     0);
    if(fd < 0)
    {
        return svcrt_net_from_errno(errno);
    }
    (void)lwip_fcntl(fd, F_SETFL, O_NONBLOCK);

    handle = svcrt_net_claim(fd, owner);
    if(handle < 0)
    {
        (void)lwip_close(fd);
        return handle;
    }
    return handle;
}

static int32 svcrt_net_do_accept(int32 owner, uint32 handle)
{
    svcrt_net_sock_t *s = svcrt_net_own(handle, owner);
    int   fd;
    int32 h;

    if(s == 0)
    {
        return SVCRT_NET_EINVAL;
    }
    fd = lwip_accept(s->fd, NULL, NULL);
    if(fd < 0)
    {
        return svcrt_net_from_errno(errno);
    }
    /* An accepted socket does not inherit the listening socket's flags. */
    (void)lwip_fcntl(fd, F_SETFL, O_NONBLOCK);

    h = svcrt_net_claim(fd, owner);
    if(h < 0)
    {
        (void)lwip_close(fd);
        return h;
    }
    return h;
}

static int32 svcrt_net_do_bind(int32 owner, uint32 handle, uint32 ip, uint32 port)
{
    svcrt_net_sock_t *s = svcrt_net_own(handle, owner);
    struct sockaddr_in sa;

    if(s == 0)
    {
        return SVCRT_NET_EINVAL;
    }
    svcrt_net_addr_in(&sa, ip, port);
    if(lwip_bind(s->fd, (const struct sockaddr *)&sa,
                 (socklen_t)sizeof(sa)) < 0)
    {
        return svcrt_net_from_errno(errno);
    }
    return 0;
}

static int32 svcrt_net_do_listen(int32 owner, uint32 handle, uint32 backlog)
{
    svcrt_net_sock_t *s = svcrt_net_own(handle, owner);

    if(s == 0)
    {
        return SVCRT_NET_EINVAL;
    }
    if(lwip_listen(s->fd, (int)backlog) < 0)
    {
        return svcrt_net_from_errno(errno);
    }
    return 0;
}

static int32 svcrt_net_do_connect(int32 owner, uint32 handle, uint32 ip, uint32 port)
{
    svcrt_net_sock_t *s = svcrt_net_own(handle, owner);
    struct sockaddr_in sa;
    int32 r;

    if(s == 0)
    {
        return SVCRT_NET_EINVAL;
    }
    svcrt_net_addr_in(&sa, ip, port);
    if(lwip_connect(s->fd, (const struct sockaddr *)&sa,
                    (socklen_t)sizeof(sa)) < 0)
    {
        r = svcrt_net_from_errno(errno);
        /* Already connected is what the caller wanted: not an error. */
        return (r == SVCRT_NET_EISCONN) ? 0 : r;
    }
    return 0;
}

static int32 svcrt_net_do_send(int32 owner, uint32 handle, uint32 buf,
                               uint32 len, uint32 flags)
{
    svcrt_net_sock_t *s = svcrt_net_own(handle, owner);
    int32 r;

    if(s == 0)
    {
        return SVCRT_NET_EINVAL;
    }
    r = (int32)lwip_send(s->fd, (const void *)buf, (size_t)len, (int)flags);
    if(r < 0)
    {
        return svcrt_net_from_errno(errno);
    }
    return r;
}

static int32 svcrt_net_do_recv(int32 owner, uint32 handle, uint32 buf,
                               uint32 len, uint32 flags)
{
    svcrt_net_sock_t *s = svcrt_net_own(handle, owner);
    int32 r;

    if(s == 0)
    {
        return SVCRT_NET_EINVAL;
    }
    r = (int32)lwip_recv(s->fd, (void *)buf, (size_t)len, (int)flags);
    if(r < 0)
    {
        return svcrt_net_from_errno(errno);
    }
    return r;
}

static int32 svcrt_net_do_sendto(int32 owner, uint32 handle, uint32 buf,
                                 uint32 len, uint32 ip, uint32 port)
{
    svcrt_net_sock_t *s = svcrt_net_own(handle, owner);
    struct sockaddr_in sa;
    int32 r;

    if(s == 0)
    {
        return SVCRT_NET_EINVAL;
    }
    svcrt_net_addr_in(&sa, ip, port);
    r = (int32)lwip_sendto(s->fd, (const void *)buf, (size_t)len, 0,
                           (const struct sockaddr *)&sa,
                           (socklen_t)sizeof(sa));
    if(r < 0)
    {
        return svcrt_net_from_errno(errno);
    }
    return r;
}

static int32 svcrt_net_do_recvfrom(int32 owner, uint32 handle, uint32 buf,
                                   uint32 len, uint32 ip_out, uint32 port_out)
{
    svcrt_net_sock_t *s = svcrt_net_own(handle, owner);
    struct sockaddr_in from;
    socklen_t from_len = (socklen_t)sizeof(from);
    int32 r;

    if(s == 0)
    {
        return SVCRT_NET_EINVAL;
    }
    r = (int32)lwip_recvfrom(s->fd, (void *)buf, (size_t)len, 0,
                             (struct sockaddr *)&from, &from_len);
    if(r < 0)
    {
        return svcrt_net_from_errno(errno);
    }
    svcrt_net_addr_out(&from, (uint32 *)ip_out, (uint32 *)port_out);
    return r;
}

static int32 svcrt_net_do_close(int32 owner, uint32 handle)
{
    svcrt_net_sock_t *s = svcrt_net_own(handle, owner);
    int32 r;

    if(s == 0)
    {
        return SVCRT_NET_EINVAL;
    }
    r = (int32)lwip_close(s->fd);
    s->fd = -1;
    s->owner = 0;
    if(r < 0)
    {
        return svcrt_net_from_errno(errno);
    }
    return 0;
}

static int32 svcrt_net_do_setsockopt(int32 owner, uint32 handle,
                                     uint32 opt, uint32 value)
{
    svcrt_net_sock_t *s = svcrt_net_own(handle, owner);
    int v = (int)value;

    if(s == 0)
    {
        return SVCRT_NET_EINVAL;
    }
    switch(opt)
    {
    case SVCRT_NET_SO_REUSEADDR:
        if(lwip_setsockopt(s->fd, SOL_SOCKET, SO_REUSEADDR, &v,
                           (socklen_t)sizeof(v)) < 0)
        {
            return svcrt_net_from_errno(errno);
        }
        break;
    case SVCRT_NET_SO_RCVBUF:
        if(lwip_setsockopt(s->fd, SOL_SOCKET, SO_RCVBUF, &v,
                           (socklen_t)sizeof(v)) < 0)
        {
            return svcrt_net_from_errno(errno);
        }
        break;
    case SVCRT_NET_SO_SNDBUF:
        if(lwip_setsockopt(s->fd, SOL_SOCKET, SO_SNDBUF, &v,
                           (socklen_t)sizeof(v)) < 0)
        {
            return svcrt_net_from_errno(errno);
        }
        break;
    default:
        /* Refused, not ignored: silently accepting an option the kernel
         * cannot honour would make the App believe something that is not
         * true (e.g. that its receive timeout is armed). */
        return SVCRT_NET_ENOTSUP;
    }
    return 0;
}

static int32 svcrt_net_do_getsockopt(int32 owner, uint32 handle,
                                     uint32 opt, uint32 value_out)
{
    svcrt_net_sock_t *s = svcrt_net_own(handle, owner);
    int  v = 0;
    socklen_t v_len = (socklen_t)sizeof(v);

    if(s == 0)
    {
        return SVCRT_NET_EINVAL;
    }
    if(opt != (uint32)SVCRT_NET_SO_ERROR)
    {
        return SVCRT_NET_ENOTSUP;
    }
    if(lwip_getsockopt(s->fd, SOL_SOCKET, SO_ERROR, &v, &v_len) < 0)
    {
        return svcrt_net_from_errno(errno);
    }
    if(value_out != 0u)
    {
        *(uint32 *)value_out = (uint32)((v == 0) ? 0 : svcrt_net_from_errno(v));
    }
    return 0;
}

static int32 svcrt_net_do_name(int32 owner, uint32 handle, uint32 ip_out,
                               uint32 port_out, uint8 peer)
{
    svcrt_net_sock_t *s = svcrt_net_own(handle, owner);
    struct sockaddr_in sa;
    socklen_t sa_len = (socklen_t)sizeof(sa);
    int rc;

    if(s == 0)
    {
        return SVCRT_NET_EINVAL;
    }
    rc = (peer != 0u) ? lwip_getpeername(s->fd, (struct sockaddr *)&sa, &sa_len)
                      : lwip_getsockname(s->fd, (struct sockaddr *)&sa, &sa_len);
    if(rc < 0)
    {
        return svcrt_net_from_errno(errno);
    }
    svcrt_net_addr_out(&sa, (uint32 *)ip_out, (uint32 *)port_out);
    return 0;
}

static int32 svcrt_net_do_shutdown(int32 owner, uint32 handle, uint32 how)
{
    svcrt_net_sock_t *s = svcrt_net_own(handle, owner);

    if(s == 0)
    {
        return SVCRT_NET_EINVAL;
    }
    if(lwip_shutdown(s->fd, (int)how) < 0)
    {
        return svcrt_net_from_errno(errno);
    }
    return 0;
}

/* Non-blocking readiness probe for one socket. select() on the App side is a
 * loop of these, which keeps the kernel free of fd_set copies and lets the
 * caller use as many descriptors as it likes. */
static int32 svcrt_net_do_poll(int32 owner, uint32 handle, uint32 flags)
{
    svcrt_net_sock_t *s = svcrt_net_own(handle, owner);
    fd_set rd;
    fd_set wr;
    fd_set ex;
    struct timeval tv;
    uint32 out = 0u;
    int n;

    if(s == 0)
    {
        return SVCRT_NET_EINVAL;
    }

    FD_ZERO(&rd);
    FD_ZERO(&wr);
    FD_ZERO(&ex);
    if((flags & SVCRT_NET_POLL_IN) != 0u)
    {
        FD_SET(s->fd, &rd);
    }
    if((flags & SVCRT_NET_POLL_OUT) != 0u)
    {
        FD_SET(s->fd, &wr);
    }
    if((flags & SVCRT_NET_POLL_ERR) != 0u)
    {
        FD_SET(s->fd, &ex);
    }
    tv.tv_sec  = 0;
    tv.tv_usec = 0;

    n = lwip_select(s->fd + 1, &rd, &wr, &ex, &tv);
    if(n < 0)
    {
        return svcrt_net_from_errno(errno);
    }
    if(FD_ISSET(s->fd, &rd))
    {
        out |= SVCRT_NET_POLL_IN;
    }
    if(FD_ISSET(s->fd, &wr))
    {
        out |= SVCRT_NET_POLL_OUT;
    }
    if(FD_ISSET(s->fd, &ex))
    {
        out |= SVCRT_NET_POLL_ERR;
    }
    return (int32)out;
}

/* ============================================================
 * 4. the service task
 * ============================================================ */

static int32 svcrt_net_exec(void)
{
    uint32 *p     = g_net_req_arg;
    int32   owner = g_net_req_owner;

    switch(p[0])
    {
    case SVCRT_NET_SUB_SOCKET:
        return svcrt_net_do_socket(owner, p[1]);

    case SVCRT_NET_SUB_BIND:
        return svcrt_net_do_bind(owner, p[1], p[2], p[3]);

    case SVCRT_NET_SUB_LISTEN:
        return svcrt_net_do_listen(owner, p[1], p[2]);

    case SVCRT_NET_SUB_ACCEPT:
        return svcrt_net_do_accept(owner, p[1]);

    case SVCRT_NET_SUB_CONNECT:
        return svcrt_net_do_connect(owner, p[1], p[2], p[3]);

    case SVCRT_NET_SUB_SEND:
        return svcrt_net_do_send(owner, p[1], p[2], p[3], p[4]);

    case SVCRT_NET_SUB_RECV:
        return svcrt_net_do_recv(owner, p[1], p[2], p[3], p[4]);

    case SVCRT_NET_SUB_SENDTO:
        return svcrt_net_do_sendto(owner, p[1], p[2], p[3], p[4], p[5]);

    case SVCRT_NET_SUB_RECVFROM:
        return svcrt_net_do_recvfrom(owner, p[1], p[2], p[3], p[4], p[5]);

    case SVCRT_NET_SUB_CLOSE:
        return svcrt_net_do_close(owner, p[1]);

    case SVCRT_NET_SUB_SETSOCKOPT:
        return svcrt_net_do_setsockopt(owner, p[1], p[2], p[3]);

    case SVCRT_NET_SUB_GETSOCKOPT:
        return svcrt_net_do_getsockopt(owner, p[1], p[2], p[3]);

    case SVCRT_NET_SUB_GETSOCKNAME:
        return svcrt_net_do_name(owner, p[1], p[2], p[3], 0u);

    case SVCRT_NET_SUB_GETPEERNAME:
        return svcrt_net_do_name(owner, p[1], p[2], p[3], 1u);

    case SVCRT_NET_SUB_SHUTDOWN:
        return svcrt_net_do_shutdown(owner, p[1], p[2]);

    case SVCRT_NET_SUB_POLL:
        return svcrt_net_do_poll(owner, p[1], p[2]);

    default:
        return SVCRT_NET_EINVAL;
    }
}

static void svcrt_net_task(void)
{
    uint32 msg;

    for(;;)
    {
        if(svcrt_mq_recv_internal(g_net_mq, &msg, 1, -1) < 0)
        {
            /* Only reachable if the handle stopped naming a live object.
             * Do not spin: svcrt_net_ready() reports "not up" meanwhile, so
             * callers get an honest refusal instead of a pegged CPU. */
            svcrt_task_wait_period_internal();
            continue;                       /* 队列被删了才会到这里 */
        }

        if(msg == SVCRT_NET_MSG_REQUEST)
        {
            g_net_req_result = svcrt_net_exec();
            /* Publish the result before the state: the waiter only looks at
             * the result once it sees DONE, and a single writer means the
             * order is all the safety this needs. */
            SVCRT_DMB();
            g_net_req_state = SVCRT_NET_REQ_DONE;
        }

        svcrt_net_reap();
    }
}

/* ============================================================
 * 5. the SVC entry point (never blocks)
 * ============================================================ */

uint8 svcrt_net_ready(void)
{
    /* Not "the handle is set": a handle keeps looking valid after the object
     * behind it is gone, and answering "up" then turns every request into a
     * bogus EBUSY. Ready only while the queue object is really there. */
    if(g_net_mq < 0)
    {
        return 0u;
    }
    return (svcrt_mq_is_alive(g_net_mq) != 0) ? 1u : 0u;
}

/* Everything the kernel is about to dereference gets checked here, in the
 * caller's own context (the window checks read the current task). */
static int32 svcrt_net_validate(const uint32 *p)
{
    switch(p[0])
    {
    case SVCRT_NET_SUB_SOCKET:
        if((p[1] != (uint32)SVCRT_NET_SOCK_STREAM) &&
           (p[1] != (uint32)SVCRT_NET_SOCK_DGRAM))
        {
            return SVCRT_NET_EINVAL;
        }
        return 0;

    case SVCRT_NET_SUB_BIND:
    case SVCRT_NET_SUB_CONNECT:
        return (svcrt_net_own(p[1], svcrt_current_task_id) != 0)
               ? 0 : SVCRT_NET_EINVAL;

    case SVCRT_NET_SUB_LISTEN:
        if(svcrt_net_own(p[1], svcrt_current_task_id) == 0)
        {
            return SVCRT_NET_EINVAL;
        }
        return (p[2] <= 255u) ? 0 : SVCRT_NET_EINVAL;

    case SVCRT_NET_SUB_ACCEPT:
    case SVCRT_NET_SUB_CLOSE:
        return (svcrt_net_own(p[1], svcrt_current_task_id) != 0)
               ? 0 : SVCRT_NET_EINVAL;

    case SVCRT_NET_SUB_SEND:
        if((svcrt_net_own(p[1], svcrt_current_task_id) == 0) ||
           (p[3] > 0xFFFFu))
        {
            return SVCRT_NET_EINVAL;
        }
        if((p[3] != 0u) &&
           (svcrt_kernel_user_ro_ok((const void *)p[2], p[3]) == 0u))
        {
            return SVCRT_NET_EINVAL;
        }
        return 0;

    case SVCRT_NET_SUB_RECV:
        if((svcrt_net_own(p[1], svcrt_current_task_id) == 0) ||
           (p[3] == 0u) || (p[3] > 0xFFFFu))
        {
            return SVCRT_NET_EINVAL;
        }
        if(svcrt_kernel_dev_buf_ok((const void *)p[2], (int32)p[3]) == 0u)
        {
            return SVCRT_NET_EINVAL;
        }
        return 0;

    case SVCRT_NET_SUB_SENDTO:
        if((svcrt_net_own(p[1], svcrt_current_task_id) == 0) ||
           (p[3] > 0xFFFFu))
        {
            return SVCRT_NET_EINVAL;
        }
        if((p[3] != 0u) &&
           (svcrt_kernel_user_ro_ok((const void *)p[2], p[3]) == 0u))
        {
            return SVCRT_NET_EINVAL;
        }
        return 0;

    case SVCRT_NET_SUB_RECVFROM:
        if((svcrt_net_own(p[1], svcrt_current_task_id) == 0) ||
           (p[3] == 0u) || (p[3] > 0xFFFFu))
        {
            return SVCRT_NET_EINVAL;
        }
        if(svcrt_kernel_dev_buf_ok((const void *)p[2], (int32)p[3]) == 0u)
        {
            return SVCRT_NET_EINVAL;
        }
        if(((p[4] != 0u) &&
            (svcrt_kernel_dev_buf_ok((const void *)p[4], 4) == 0u)) ||
           ((p[5] != 0u) &&
            (svcrt_kernel_dev_buf_ok((const void *)p[5], 4) == 0u)))
        {
            return SVCRT_NET_EINVAL;
        }
        return 0;

    case SVCRT_NET_SUB_SETSOCKOPT:
        if(svcrt_net_own(p[1], svcrt_current_task_id) == 0)
        {
            return SVCRT_NET_EINVAL;
        }
        return 0;

    case SVCRT_NET_SUB_GETSOCKOPT:
        if(svcrt_net_own(p[1], svcrt_current_task_id) == 0)
        {
            return SVCRT_NET_EINVAL;
        }
        if((p[3] != 0u) &&
           (svcrt_kernel_dev_buf_ok((const void *)p[3], 4) == 0u))
        {
            return SVCRT_NET_EINVAL;
        }
        return 0;

    case SVCRT_NET_SUB_GETSOCKNAME:
    case SVCRT_NET_SUB_GETPEERNAME:
        if(svcrt_net_own(p[1], svcrt_current_task_id) == 0)
        {
            return SVCRT_NET_EINVAL;
        }
        if(((p[2] != 0u) &&
            (svcrt_kernel_dev_buf_ok((const void *)p[2], 4) == 0u)) ||
           ((p[3] != 0u) &&
            (svcrt_kernel_dev_buf_ok((const void *)p[3], 4) == 0u)))
        {
            return SVCRT_NET_EINVAL;
        }
        return 0;

    case SVCRT_NET_SUB_SHUTDOWN:
        if(svcrt_net_own(p[1], svcrt_current_task_id) == 0)
        {
            return SVCRT_NET_EINVAL;
        }
        return (p[2] <= (uint32)SVCRT_NET_SHUT_RDWR) ? 0 : SVCRT_NET_EINVAL;

    case SVCRT_NET_SUB_POLL:
        if(svcrt_net_own(p[1], svcrt_current_task_id) == 0)
        {
            return SVCRT_NET_EINVAL;
        }
        if((p[2] == 0u) ||
           ((p[2] & ~(uint32)(SVCRT_NET_POLL_IN | SVCRT_NET_POLL_OUT |
                              SVCRT_NET_POLL_ERR)) != 0u))
        {
            return SVCRT_NET_EINVAL;
        }
        return 0;

    default:
        return SVCRT_NET_EINVAL;
    }
}

/* Is this call the same request as the one in the slot? The whole argument
 * block is compared, not just the sub command: two different sockets in two
 * send() calls are two different requests, and handing one the other's reply
 * would be a wrong answer that looks like a right one. */
static uint8 svcrt_net_same_args(const uint32 *p)
{
    uint32 i;

    for(i = 0u; i < SVCRT_NET_ARG_WORDS; i++)
    {
        if(p[i] != g_net_req_arg[i])
        {
            return 0u;
        }
    }
    return 1u;
}

int32 svcrt_net_svc(void *p_svc_ctx)
{
    uint32 *p;
    uint32  op;
    uint32  msg;
    uint32  i;
    int32   r;

    p = (uint32 *)SVCRT_SVC_ARG(p_svc_ctx, 0);
    if(svcrt_kernel_svc_args_ok(p, (uint32)(SVCRT_NET_ARG_WORDS * 4u)) == 0u)
    {
        return SVCRT_NET_EINVAL;
    }

    op = p[0];

    if(op == SVCRT_NET_SUB_QUERY)
    {
        return (svcrt_net_ready() != 0u) ? 1 : 0;
    }

    if(op > SVCRT_NET_SUB_POLL)
    {
        return SVCRT_NET_EINVAL;
    }

    svcrt_sched_lock_internal();

    /* ---- the reply of a finished request ---- */
    if(g_net_req_state == SVCRT_NET_REQ_DONE)
    {
        if(g_net_req_owner < 0)
        {
            /* The owner died before collecting this. Nobody can use it now,
             * and leaving it here would lock the slot for good. */
            g_net_req_state = SVCRT_NET_REQ_IDLE;
        }
        else if(g_net_req_owner != svcrt_current_task_id)
        {
            svcrt_sched_unlock_internal();
            return SVCRT_NET_EBUSY;
        }
        else if(svcrt_net_same_args(p) != 0u)
        {
            r = g_net_req_result;
            g_net_req_state = SVCRT_NET_REQ_IDLE;
            svcrt_sched_unlock_internal();
            return r;
        }
        else
        {
            /* The caller gave up on that request - its budget ran out and it
             * has moved on. That reply answers a question nobody is asking
             * any more, so it is dropped rather than handed to this call. */
            g_net_req_state = SVCRT_NET_REQ_IDLE;
        }
    }

    /* ---- our own request, still being worked on ---- */
    else if(g_net_req_state == SVCRT_NET_REQ_PENDING)
    {
        if((g_net_req_owner != svcrt_current_task_id) ||
           (svcrt_net_same_args(p) == 0u))
        {
            /* In flight, and either not ours to answer or ours but abandoned.
             * A request already handed to the service task cannot be
             * cancelled, so "busy" is the honest answer - never somebody
             * else's reply, and never a reply to a call the caller no longer
             * makes. */
            svcrt_sched_unlock_internal();
            return SVCRT_NET_EBUSY;
        }
        svcrt_sched_unlock_internal();
        return SVCRT_NET_EPENDING;
    }

    /* ---- idle: take a new one ---- */
    if(svcrt_net_ready() == 0u)
    {
        svcrt_sched_unlock_internal();
        return SVCRT_NET_ENOTSUP;
    }

    r = svcrt_net_validate(p);
    if(r != 0)
    {
        svcrt_sched_unlock_internal();
        return r;
    }

    for(i = 0u; i < SVCRT_NET_ARG_WORDS; i++)
    {
        g_net_req_arg[i] = p[i];
    }
    g_net_req_owner = svcrt_current_task_id;

    /* PENDING goes up BEFORE the message: the service task may run the
     * moment the message lands, and if it did that before this store, the
     * result it publishes would be overwritten by PENDING and the caller
     * would wait for a reply that already came. The arguments are complete
     * by now, so the early state change costs nothing. */
    g_net_req_state = SVCRT_NET_REQ_PENDING;

    msg = SVCRT_NET_MSG_REQUEST;
    if(svcrt_mq_send_internal(g_net_mq, &msg, 1, 0) < 0)
    {
        g_net_req_state = SVCRT_NET_REQ_IDLE;
        svcrt_sched_unlock_internal();
        return SVCRT_NET_EBUSY;
    }
    svcrt_sched_unlock_internal();

    return SVCRT_NET_EPENDING;
}

/* 1 = a live service task (entry == svcrt_net_task) sits in the task table.
 * Only the rebuild path asks, so a second service task is never stacked on
 * top of a running one. */
static uint8 svcrt_net_service_alive(void)
{
    int32 i;

    for(i = 0; i < svcrt_task_count; i++)
    {
        if((svcrt_task_table[i].entry == svcrt_net_task) &&
           (svcrt_task_table[i].status != SVCRT_TASK_INVALID))
        {
            return 1u;
        }
    }
    return 0u;
}

int32 svcrt_net_start(void)
{
    int32 h;
    int32 rc;

    if((g_net_mq >= 0) && (svcrt_mq_is_alive(g_net_mq) != 0))
    {
        return 0;                       /* idempotent */
    }
    if(g_net_mq >= 0)
    {
        /* The object behind our handle is gone. Rebuild instead of holding a
         * dead handle: the service task re-reads g_net_mq on every loop, so
         * a fresh queue is picked up without touching the task. */
        SVCRT_LOGW("NET", "request queue gone; rebuilding");
        g_net_mq = -1;
    }

    /* Kernel owned on purpose: this queue has to outlive every task, so it
     * must not carry the id of whichever task happens to be current while
     * lwIP starts. Task teardown only collects queues whose creator id
     * matches the dead task, so id 0 leaves this one alone. */
    h = svcrt_mq_create_kernel_internal("net_req");
    if(h < 0)
    {
        SVCRT_LOGE("NET", "request queue create failed");
        return h;
    }
    g_net_mq = h;

    if(svcrt_net_service_alive() != 0u)
    {
        return 0;               /* task already up: it re-reads g_net_mq */
    }

    rc = svcrt_task_register(svcrt_net_task,
                             g_net_task_stack,
                             (uint32)sizeof(g_net_task_stack),
                             (uint8)SVCRT_NET_TASK_PRIO,
                             1000u);
    /* svcrt_task_register() returns the new task id (>= 1) on success and a
     * negative value on failure - testing "!= 0" reads every success as a
     * failure, deletes the queue just built and reports the service down
     * while the task is in fact running. Task id and error must be told
     * apart by sign. */
    if(rc < 0)
    {
        (void)svcrt_mq_delete_internal(g_net_mq);
        g_net_mq = -1;
        SVCRT_LOGE("NET", "service task register failed");
        return rc;
    }

    SVCRT_LOGI("NET", "socket service up (SVC 0x1E)");
    return 0;
}

void svcrt_net_release_task(int32 task_id)
{
    uint32 i;
    uint32 msg;

    if(task_id <= 0)
    {
        return;
    }

    /* Only mark: lwIP may not be touched from here (this runs from the task
     * teardown path, possibly with interrupts already masked, and an lwIP
     * call would want to wait). The service task does the close.
     * No scheduler lock either: the caller already sits in a critical
     * section and each write is a single aligned word, so a concurrent read
     * in the SVC handler either sees the old owner (and refuses) or the
     * orphan marker (and refuses) - never anything half done. */
    for(i = 0u; i < SVCRT_NET_MAX_SOCKETS; i++)
    {
        if((g_net_sock[i].fd >= 0) && (g_net_sock[i].owner == task_id))
        {
            g_net_sock[i].owner = -1;
        }
    }

    /* The request slot is pinned to its owner too. Make it ownerless: the
     * reply, if one still comes, is not collectable any more, and the next
     * caller discards it instead of being handed another call's answer. */
    if(g_net_req_owner == task_id)
    {
        g_net_req_owner = -1;
    }

    if(g_net_mq >= 0)
    {
        msg = SVCRT_NET_MSG_REAP;
        (void)svcrt_mq_send_internal(g_net_mq, &msg, 1, 0);
    }
}

#else   /* SVCRT_USE_LWIP != 1 */

int32 svcrt_net_start(void)
{
    return 0;
}

uint8 svcrt_net_ready(void)
{
    return 0u;
}

int32 svcrt_net_svc(void *p_svc_ctx)
{
    (void)p_svc_ctx;
    /* Honest refusal: this kernel was built without a TCP/IP stack, so the
     * App gets "not supported" instead of an empty success. */
    return SVCRT_NET_ENOTSUP;
}

void svcrt_net_release_task(int32 task_id)
{
    (void)task_id;
}

#endif  /* SVCRT_USE_LWIP */
