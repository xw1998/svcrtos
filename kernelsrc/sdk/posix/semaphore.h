/**
* @file semaphore.h
* @brief POSIX unnamed semaphores for SVCrtOS Apps.
* @details Backed by the kernel semaphore service (SVC 0x15). sem_t holds the
*          kernel handle, so sem_init / sem_destroy are the only lifecycle
*          calls and no static storage is needed.
* @author xw
*/
#ifndef __SVCRT_SEMAPHORE_H__
#define __SVCRT_SEMAPHORE_H__

#include "svcrt_posix_types.h"

#define SEM_FAILED  ((svcrt_sem_t *)0)
#define SEM_VALUE_MAX (0x7fffffff)

/** Opaque to the caller: only svcrt_posix.c knows what is inside. */
typedef struct
{
    int32 handle;
} svcrt_sem_t;

/* The POSIX spelling, so `sem_t s;` compiles as written. */
typedef svcrt_sem_t sem_t;

int svcrt_posix_sem_init(svcrt_sem_t *sem, int pshared, uint32 value);
int svcrt_posix_sem_destroy(svcrt_sem_t *sem);
int svcrt_posix_sem_wait(svcrt_sem_t *sem);
int svcrt_posix_sem_trywait(svcrt_sem_t *sem);
int svcrt_posix_sem_timedwait(svcrt_sem_t *sem, uint32 timeout_ms);
int svcrt_posix_sem_post(svcrt_sem_t *sem);
int svcrt_posix_sem_getvalue(svcrt_sem_t *sem, int *value);

#define sem_init(s, pshared, v)     svcrt_posix_sem_init((s), (pshared), (v))
#define sem_destroy(s)              svcrt_posix_sem_destroy(s)
#define sem_wait(s)                 svcrt_posix_sem_wait(s)
#define sem_trywait(s)              svcrt_posix_sem_trywait(s)
#define sem_timedwait(s, ms)        svcrt_posix_sem_timedwait((s), (ms))
#define sem_post(s)                 svcrt_posix_sem_post(s)
#define sem_getvalue(s, v)          svcrt_posix_sem_getvalue((s), (v))

#endif /* __SVCRT_SEMAPHORE_H__ */
