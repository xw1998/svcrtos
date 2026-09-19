/**
* @file time.h
* @brief Minimal <time.h> for SVCrtOS Apps.
* @details Only the calls that make sense without a wall clock: the kernel
*          keeps a boot relative millisecond tick, so time() returns whole
*          seconds since boot and clock_gettime() reports that same base.
*          There is no calendar, so no localtime / strftime here - a program
*          needing those should bring its own.
* @note  If your toolchain's <time.h> is pulled in as well, define
*        SVCRT_POSIX_NO_TIMESPEC so struct timespec is not defined twice.
* @author xw
*/
#ifndef __SVCRT_TIME_H__
#define __SVCRT_TIME_H__

#include "svcrt_posix_types.h"
#include "unistd.h"

#define CLOCK_REALTIME   SVCRT_CLOCK_REALTIME
#define CLOCK_MONOTONIC  SVCRT_CLOCK_MONOTONIC

svcrt_time_t svcrt_posix_time(svcrt_time_t *tloc);
int          svcrt_posix_clock_gettime(svcrt_clockid_t clk, struct timespec *ts);
/* Kept for source compatibility; SVCrtOS has nothing to yield to but the
 * scheduler, so this is a plain thread wait of 0 ms. */
int          svcrt_posix_sched_yield(void);

#define time(t)              svcrt_posix_time(t)
#define clock_gettime(c, ts) svcrt_posix_clock_gettime((c), (ts))
#define sched_yield()        svcrt_posix_sched_yield()

#endif /* __SVCRT_TIME_H__ */
