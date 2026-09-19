/**
* @file fcntl.h
* @brief open()/close() flags for SVCrtOS Apps.
* @details An SVCrtOS "file descriptor" is a device handle from
*          svcrt_dev_open(). There is no filesystem, so the flags that only
*          make sense with one (O_CREAT, O_TRUNC, ...) are absent on purpose:
*          a program that needs them should fail at compile time rather than
*          open something unexpected at run time.
* @author xw
*/
#ifndef __SVCRT_FCNTL_H__
#define __SVCRT_FCNTL_H__

#include "svcrt_posix_types.h"

#define O_RDONLY    0x0000
#define O_WRONLY    0x0001
#define O_RDWR      0x0002
#define O_ACCMODE   0x0003
#define O_NONBLOCK  0x0004
#define O_APPEND    0x0008

int  svcrt_posix_open(const char *name, int flags, ...);
int  svcrt_posix_close(int fd);

/* Two arguments only: there is no O_CREAT here, so no mode argument is ever
 * meaningful and accepting one would suggest otherwise. */
#define open(name, flags)  svcrt_posix_open((name), (flags))

#endif /* __SVCRT_FCNTL_H__ */
