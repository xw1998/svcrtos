/**
* @file svcrt_posix_types.h
* @brief Base POSIX types for the SVCrtOS compatibility layer.
* @details Kept in one place so every POSIX header agrees on the width of
*          ssize_t / off_t / time_t. The widths are fixed (int32 / uint32)
*          because SVCrtOS is a 32-bit only kernel and pretending otherwise
*          would only hide porting mistakes.
*
*          struct timespec: our definition may collide with a toolchain one,
*          because only the toolchain knows its own guard macro. If your
*          platform header already defines it, define SVCRT_POSIX_NO_TIMESPEC
*          before including this file. A collision is a compile error, never
*          a silent misread.
*
* @note  Dependency-free apart from svcrt_types.h.
* @author xw
*/
#ifndef __SVCRT_POSIX_TYPES_H__
#define __SVCRT_POSIX_TYPES_H__

#include "svcrt_types.h"

typedef int32  svcrt_ssize_t;
typedef int32  svcrt_off_t;
typedef int32  svcrt_pid_t;
typedef uint32 svcrt_mode_t;
typedef int32  svcrt_clockid_t;
typedef uint32 svcrt_time_t;
typedef int32  svcrt_useconds_t;

/** POSIX clock ids. Only MONOTONIC is backed by anything here: SVCrtOS has a
 *  boot relative tick, there is no wall clock, so REALTIME is aliased to it
 *  rather than reported as a different (and wrong) time base. */
#define SVCRT_CLOCK_REALTIME   0
#define SVCRT_CLOCK_MONOTONIC  1

#ifndef SVCRT_POSIX_NO_TIMESPEC
struct timespec
{
    svcrt_time_t tv_sec;
    int32        tv_nsec;
};
#endif

#endif /* __SVCRT_POSIX_TYPES_H__ */
