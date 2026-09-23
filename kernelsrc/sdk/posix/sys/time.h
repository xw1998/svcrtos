/**
* @file sys/time.h
* @brief struct timeval for SVCrtOS Apps - the select() timeout.
* @details SVCrtOS has no wall clock: the kernel keeps one boot relative
*          millisecond tick. So there is no gettimeofday() here, because a
*          "time of day" would have to be invented and a program would then
*          believe it. clock_gettime() in <time.h> reports that same tick,
*          and that is the honest call. This header exists so <sys/socket.h>
*          can give select() the POSIX timeout type.
* @note  Define SVCRT_POSIX_NO_TIMEVAL when the toolchain headers already
*        define struct timeval; a collision is a compile error, never a
*        silent misread.
* @author xw
*/
#ifndef __SVCRT_SYS_TIME_H__
#define __SVCRT_SYS_TIME_H__

#include "svcrt_posix_types.h"

#ifndef SVCRT_POSIX_NO_TIMEVAL
struct timeval
{
    svcrt_time_t     tv_sec;    /**< whole seconds                */
    svcrt_useconds_t tv_usec;   /**< microseconds, 0..999999      */
};
#endif

#endif /* __SVCRT_SYS_TIME_H__ */
