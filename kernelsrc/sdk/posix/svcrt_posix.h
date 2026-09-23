/**
* @file svcrt_posix.h
* @brief SVCrtOS POSIX compatibility layer - umbrella header.
* @details Lets an ordinary POSIX C program run as an SVCrtOS App with as few
*          source edits as possible. Everything maps onto SVC services the App
*          SDK already exposes, so nothing here knows an address, a partition
*          or a slot and the same source builds for any board.
*
*          Include this file instead of <pthread.h> / <semaphore.h> and the
*          rest; the individual headers stay usable on their own when a
*          project only needs one of them.
*
*          Tunables - define before including, or in the project options:
*            SVCRT_POSIX_THREAD_MAX   concurrent pthreads        (default 2)
*            SVCRT_POSIX_STACK_WORDS  stack words per pthread    (default 256)
*            SVCRT_POSIX_HEAP_SIZE    App side heap bytes        (default 1024)
*
*          The defaults are sized to fit the 8 KB RAM window a development
*          App slot gets out of the box: 2 * 1 KB of thread stack plus a 1 KB
*          arena leaves room for the App's own data and its main stack. Raise
*          them per project when the App slot has more RAM; the arenas are
*          static, so the cost is paid whether they are used or not.
*            SVCRT_POSIX_DEFAULT_PRIO default thread priority    (default 10)
*            SVCRT_POSIX_NO_TIMESPEC  platform already has struct timespec
*            SVCRT_POSIX_WRAP_STDLIB  also #define malloc/free/... over ours
*            SVCRT_POSIX_PTHREAD_KEY  thread-specific data (TLS)  (default 1)
*
* @note  Declarations only. svcrt_posix.c has to be added to the App project
*        once; the memory it costs is the two static arenas below.
* @author xw
*/
#ifndef __SVCRT_POSIX_H__
#define __SVCRT_POSIX_H__

#include "svcrt.h"

/* ------------------------------------------------------------- tunables */
#ifndef SVCRT_POSIX_THREAD_MAX
#define SVCRT_POSIX_THREAD_MAX    (2)
#endif
#ifndef SVCRT_POSIX_STACK_WORDS
#define SVCRT_POSIX_STACK_WORDS   (256u)
#endif
#ifndef SVCRT_POSIX_HEAP_SIZE
#define SVCRT_POSIX_HEAP_SIZE     (1024u)
#endif
#ifndef SVCRT_POSIX_DEFAULT_PRIO
#define SVCRT_POSIX_DEFAULT_PRIO  (10u)
#endif

/* Thread-specific data (pthread_key_*) and pthread_once.  Set to 0 to drop
 * both the declarations and the static pointer table svcrt_posix.c keeps for
 * them (SVCRT_POSIX_KEY_MAX x (SVCRT_POSIX_THREAD_MAX + 1) pointers). */
#ifndef SVCRT_POSIX_PTHREAD_KEY
#define SVCRT_POSIX_PTHREAD_KEY   1
#endif

#include "svcrt_posix_types.h"
#include "errno.h"
#include "fcntl.h"
#include "unistd.h"
#include "time.h"
#include "pthread.h"
#include "semaphore.h"
#include "mqueue.h"
#include "sys/stat.h"
#include "dirent.h"
#include "sys/socket.h"
#include "netinet/in.h"
#include "arpa/inet.h"


/* ----------------------------------------------------------- App memory.
 * A small first fit arena inside the App's own RAM. It is intentionally not
 * a kernel service: the kernel has no heap, the MPU already confines the App
 * to its own window, and an allocator that lives on the same side of the
 * privilege boundary as its callers is one less thing to get wrong.
 */
void *svcrt_posix_malloc(uint32 size);
void  svcrt_posix_free(void *ptr);
void *svcrt_posix_calloc(uint32 nmemb, uint32 size);
void *svcrt_posix_realloc(void *ptr, uint32 size);
uint32 svcrt_posix_heap_total(void);
uint32 svcrt_posix_heap_free_bytes(void);

/* Convenience helpers, same reason as the allocator: no libc dependency. */
void  *svcrt_posix_memset(void *dst, int value, uint32 len);
void  *svcrt_posix_memcpy(void *dst, const void *src, uint32 len);
uint32 svcrt_posix_strlen(const char *s);

#if defined(SVCRT_POSIX_WRAP_STDLIB)
#define malloc(n)      svcrt_posix_malloc(n)
#define free(p)        svcrt_posix_free(p)
#define calloc(n, s)   svcrt_posix_calloc((n), (s))
#define realloc(p, n)  svcrt_posix_realloc((p), (n))
#endif

#endif /* __SVCRT_POSIX_H__ */
