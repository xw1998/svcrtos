/**
* @file pthread.h
* @brief POSIX threads for SVCrtOS Apps.
* @details SVCrtOS has threads, a scheduler and synchronization, but no user
*          mode way to create a thread: a thread needs a TCB and a scheduler
*          slot, and only the kernel owns those. pthread_create therefore goes
*          through the thread service (SVC 0x1B) added for this purpose.
*
*          Two things a POSIX program may expect are absent on purpose,
*          because the kernel cannot honour them and pretending would be a
*          silent lie:
*            - no priority inheritance on mutexes (the kernel mutex does
*              inherit, but a POSIX program cannot observe or set it),
*            - no condition variables (they need a kernel wait queue with a
*              signal/broadcast primitive that does not exist yet).
*
* @author xw
*/
#ifndef __SVCRT_PTHREAD_H__
#define __SVCRT_PTHREAD_H__

#include "svcrt_posix_types.h"

typedef uint32 svcrt_pthread_t;
#define SVCRT_PTHREAD_NULL  ((svcrt_pthread_t)0)

/** Thread attributes. Anything left at 0 takes the built-in default, so a
 *  plain pthread_create(t, NULL, fn, arg) call needs no attributes at all. */
typedef struct
{
    void  *stackaddr;   /* 0 = take a stack from the SDK pool            */
    uint32 stacksize;   /* bytes, 0 = SVCRT_POSIX_STACK_WORDS * 4        */
    uint32 priority;    /* kernel priority, 0 = SVCRT_POSIX_DEFAULT_PRIO */
    uint32 period_ms;   /* 0 = event driven (blocks until it waits)      */
} svcrt_pthread_attr_t;

typedef svcrt_pthread_attr_t pthread_attr_t;
typedef svcrt_pthread_t      pthread_t;

typedef struct
{
    int32 handle;       /* kernel mutex handle, -1 = not created yet */
} svcrt_pthread_mutex_t;

typedef svcrt_pthread_mutex_t pthread_mutex_t;

#define PTHREAD_MUTEX_INITIALIZER  { -1 }

int  svcrt_posix_pthread_attr_init(svcrt_pthread_attr_t *attr);
int  svcrt_posix_pthread_create(svcrt_pthread_t *thread,
                                const svcrt_pthread_attr_t *attr,
                                void *(*start_routine)(void *), void *arg);
int  svcrt_posix_pthread_join(svcrt_pthread_t thread, void **retval);
void svcrt_posix_pthread_exit(void *retval);
svcrt_pthread_t svcrt_posix_pthread_self(void);

int  svcrt_posix_mutex_init(svcrt_pthread_mutex_t *m);
int  svcrt_posix_mutex_destroy(svcrt_pthread_mutex_t *m);
int  svcrt_posix_mutex_lock(svcrt_pthread_mutex_t *m);
int  svcrt_posix_mutex_trylock(svcrt_pthread_mutex_t *m);
int  svcrt_posix_mutex_unlock(svcrt_pthread_mutex_t *m);

#define pthread_attr_init(a)        svcrt_posix_pthread_attr_init(a)
#define pthread_create(t, a, f, g)  svcrt_posix_pthread_create((t), (a), (f), (g))
#define pthread_join(t, r)          svcrt_posix_pthread_join((t), (r))
#define pthread_exit(r)             svcrt_posix_pthread_exit(r)
#define pthread_self()              svcrt_posix_pthread_self()
#define pthread_mutex_init(m, a)    svcrt_posix_mutex_init(m)
#define pthread_mutex_destroy(m)    svcrt_posix_mutex_destroy(m)
#define pthread_mutex_lock(m)       svcrt_posix_mutex_lock(m)
#define pthread_mutex_trylock(m)    svcrt_posix_mutex_trylock(m)
#define pthread_mutex_unlock(m)     svcrt_posix_mutex_unlock(m)

#define pthread_cond_init(c, a)         svcrt_posix_cond_init(c)
#define pthread_cond_destroy(c)         svcrt_posix_cond_destroy(c)
#define pthread_cond_wait(c, m)         svcrt_posix_cond_wait((c), (m))
#define pthread_cond_timedwait(c, m, t) svcrt_posix_cond_timedwait((c), (m), (t))
#define pthread_cond_signal(c)          svcrt_posix_cond_signal(c)
#define pthread_cond_broadcast(c)       svcrt_posix_cond_broadcast(c)

/* Condition variable: a kernel object bound on first use, so
 * PTHREAD_COND_INITIALIZER works exactly like the mutex one. */
typedef struct
{
    int32 handle;       /* kernel cond handle, -1 = not created yet */
} svcrt_pthread_cond_t;

typedef svcrt_pthread_cond_t pthread_cond_t;

#define PTHREAD_COND_INITIALIZER  { -1 }

int  svcrt_posix_cond_init(svcrt_pthread_cond_t *c);
int  svcrt_posix_cond_destroy(svcrt_pthread_cond_t *c);
int  svcrt_posix_cond_wait(svcrt_pthread_cond_t *c, svcrt_pthread_mutex_t *m);
int  svcrt_posix_cond_timedwait(svcrt_pthread_cond_t *c, svcrt_pthread_mutex_t *m,
                                const struct timespec *abstime);
int  svcrt_posix_cond_signal(svcrt_pthread_cond_t *c);
int  svcrt_posix_cond_broadcast(svcrt_pthread_cond_t *c);

/* One row per POSIX thread slot, for the App side shell dump.  Plain data,
 * no printing: the App decides how to show it. */
typedef struct
{
    uint32 in_use;
    int32  task_id;          /* kernel task id, -1 once the thread left */
    int32  gate;             /* start gate handle                        */
    int32  done;             /* completion handle                        */
} svcrt_pth_dbg_t;

/* Copies SVCRT_POSIX_THREAD_MAX rows into out.  Returns 0 on success. */
int32 svcrt_posix_pth_dbg_slots(svcrt_pth_dbg_t *out);

extern volatile int32 svcrt_posix_pth_dbg_claim;   /* slot claimed by task id */
extern volatile int32 svcrt_posix_pth_dbg_boot;    /* claimed via boot_slot   */
extern volatile int32 svcrt_posix_pth_dbg_lost;    /* gave up: no slot to be  */
extern volatile int32 svcrt_posix_pth_dbg_post;    /* trampoline posted done  */
extern volatile int32 svcrt_posix_pth_dbg_exit;    /* pthread_exit() posted   */

#endif /* __SVCRT_PTHREAD_H__ */
