/**
* @file unistd.h
* @brief Minimal <unistd.h> for SVCrtOS Apps.
* @details Maps the POSIX names a portable C program actually uses onto the
*          App SDK: read/write/close act on a device handle, sleep/usleep map
*          onto the scheduler, and there is no process model at all (fork,
*          exec, getpid beyond a stub) because an App is a single address
*          space with threads, not a process tree.
* @author xw
*/
#ifndef __SVCRT_UNISTD_H__
#define __SVCRT_UNISTD_H__

#include "svcrt_posix_types.h"
#include "fcntl.h"

#define STDIN_FILENO    0
#define STDOUT_FILENO   1
#define STDERR_FILENO   2

svcrt_ssize_t svcrt_posix_read(int fd, void *buf, uint32 count);
svcrt_ssize_t svcrt_posix_write(int fd, const void *buf, uint32 count);
int           svcrt_posix_close(int fd);

/** Suspend the calling thread for whole seconds (scheduler based). */
uint32 svcrt_posix_sleep(uint32 seconds);
/** Suspend the calling thread for microseconds. SVCrtOS waits in ms, so the
 *  value is rounded UP: a sleep never returns early. */
int    svcrt_posix_usleep(svcrt_useconds_t usec);
/** Suspend until the given absolute time on the chosen clock. */
int    svcrt_posix_nanosleep(const struct timespec *req, struct timespec *rem);
/** Wall clock seconds since boot - SVCrtOS has no RTC. */
svcrt_time_t svcrt_posix_time(svcrt_time_t *tloc);
int    svcrt_posix_clock_gettime(svcrt_clockid_t clk, struct timespec *ts);

/* Aliases so plain POSIX source compiles unchanged. */
#define read(fd, buf, n)        svcrt_posix_read((fd), (buf), (n))
#define write(fd, buf, n)       svcrt_posix_write((fd), (buf), (n))
#define close(fd)               svcrt_posix_close(fd)
#define sleep(s)                svcrt_posix_sleep(s)
#define usleep(us)              svcrt_posix_usleep(us)
#define nanosleep(req, rem)     svcrt_posix_nanosleep((req), (rem))
#define time(t)                 svcrt_posix_time(t)
#define clock_gettime(c, ts)    svcrt_posix_clock_gettime((c), (ts))

#endif /* __SVCRT_UNISTD_H__ */
