/**
* @file fcntl.h
* @brief open()/close() flags for SVCrtOS Apps.
* @details Two kinds of "file" can be opened through open(), and the caller
*          writes the same code for both:
*            - a name that starts with '/' is an absolute path in the VFS
*              namespace ("/dev/uart0", "/mnt/nor/a.txt", "/tmp") and is
*              served by the file system;
*            - any other name is a device the driver registered with the
*              kernel ("uart0"), exactly as before.
*          The O_* values are the POSIX ones and are pinned to the kernel's
*          SVCRT_PATH_O_* by a static assert on the kernel side, so a program
*          that compares against O_CREAT keeps its meaning. Creation,
*          truncation and append are only meaningful for a path; asking a
*          device for them is refused rather than quietly ignored.
* @author xw
*/
#ifndef __SVCRT_FCNTL_H__
#define __SVCRT_FCNTL_H__

#include "svcrt_posix_types.h"

#define O_RDONLY    0x0000
#define O_WRONLY    0x0001
#define O_RDWR      0x0002
#define O_ACCMODE   0x0003
#define O_CREAT     0x0004
#define O_TRUNC     0x0008
#define O_APPEND    0x0010

/* fcntl() commands. Only the two an App can actually act on: SVCrtOS has no
 * file owner, no advisory locks and no descriptor duplication, so the rest
 * are absent rather than accepted and quietly ignored. */
#define F_GETFL     3
#define F_SETFL     4
#define O_DIRECTORY 0x0020
/* Not one of the kernel's path flags - this one never leaves the App. It
 * is the fcntl() status flag that makes a socket not wait, and it lives
 * above 0x0020 so it can never be confused with a kernel flag. */
#define O_NONBLOCK  0x0080

int  svcrt_posix_open(const char *name, int flags, ...);
int  svcrt_posix_close(int fd);

/* The real open() takes an optional third argument (the mode, only used with
 * O_CREAT). There are no permission bits here, so the argument is accepted
 * and ignored - accepting it lets portable source compile unchanged instead
 * of failing on the extra parameter. */
#define open(...)  svcrt_posix_open(__VA_ARGS__)/** Get or set the descriptor status flags. For a socket the only flag that
 *  means anything is O_NONBLOCK, and it is remembered in the App's own fd
 *  table - the kernel's socket calls never block in the first place, so
 *  "non-blocking" is a promise this layer keeps, not one it forwards. */
int  svcrt_posix_fcntl(int fd, int cmd, ...);
#define fcntl(...)  svcrt_posix_fcntl(__VA_ARGS__)



#endif /* __SVCRT_FCNTL_H__ */
