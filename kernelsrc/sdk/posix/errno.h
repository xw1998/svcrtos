/**
* @file errno.h
* @brief POSIX errno for SVCrtOS Apps.
* @details errno is per thread. SVCrtOS reaches the kernel through SVC, but
*          errno itself is pure userspace state - no kernel capability is
*          involved - so it is stored here, in the App's own RAM, in one cell
*          per thread. The thread is identified with the kernel task id from
*          SVC 0x1B (sub command "self"), which is what makes the cell really
*          per thread without any kernel side table.
*
*          Cost: one SVC per errno access. That is deliberate - correctness
*          first on a small RTOS; cache it in a local if you are in a hot loop.
*
* @author xw
*/
#ifndef __SVCRT_ERRNO_H__
#define __SVCRT_ERRNO_H__

#include "svcrt_posix_types.h"

#define EPERM        1
#define ENOENT       2
#define ESRCH        3
#define EINTR        4
#define EIO          5
#define ENXIO        6
#define E2BIG        7
#define ENOEXEC      8
#define EBADF        9
#define ECHILD      10
#define EAGAIN      11
#define ENOMEM      12
#define EACCES      13
#define EFAULT      14
#define EBUSY       16
#define EEXIST      17
#define EXDEV       18
#define ENODEV      19
#define ENOTDIR     20
#define EISDIR      21
#define EINVAL      22
#define ENFILE      23
#define EMFILE      24
#define ENOTTY      25
#define EFBIG       27
#define ENOSPC      28
#define ESPIPE      29
#define EROFS       30
#define EMLINK      31
#define EPIPE       32
#define EDOM        33
#define ERANGE      34
#define EDEADLK     35
#define ENAMETOOLONG 36
#define ENOSYS      38
#define ENOTEMPTY   39
#define ELOOP       40
#define EOVERFLOW   75
#define ETIMEDOUT  110
#define EINPROGRESS 115
#define EOPNOTSUPP   95

/* ---- sockets ---------------------------------------------------------- */

/* Two names, one condition: a non-blocking socket that cannot proceed now. */
#define EWOULDBLOCK     EAGAIN
#define ENOTSOCK        88
#define EMSGSIZE        90
#define EPROTOTYPE      91
#define ENOPROTOOPT     92
#define EPROTONOSUPPORT 93
#define ESOCKTNOSUPPORT 94
#define EAFNOSUPPORT    97
#define EADDRINUSE      98
#define EADDRNOTAVAIL   99
#define ENETDOWN       100
#define ENETUNREACH    101
#define ENETRESET      102
#define ECONNABORTED   103
#define ECONNRESET     104
#define ENOBUFS        105
#define EISCONN        106
#define ENOTCONN       107
#define ECONNREFUSED   111
#define EHOSTUNREACH   113
#define EALREADY       114

/** Address of the calling thread's errno cell; never NULL. */
int32 *svcrt_posix_errno_location(void);

#ifndef errno
#define errno (*svcrt_posix_errno_location())
#endif

#endif /* __SVCRT_ERRNO_H__ */
