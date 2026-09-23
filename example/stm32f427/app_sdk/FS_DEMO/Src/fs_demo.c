/**
* @file fs_demo.c
* @brief File-class POSIX on SVCrtOS: an ordinary C program that reaches the
*        VFS namespace through open/read/write/lseek/stat/opendir/readdir.
* @details Everything here is standard POSIX: the includes are <stdio.h>,
*          <string.h>, <errno.h>, <fcntl.h>, <unistd.h>, <sys/stat.h> and
*          <dirent.h>. There is no svcrt.h, no device name hard coded beyond
*          the path string "/dev/COM1" (the name this board registers its
*          console UART under), no address and no partition id.
*
*          Two things this file is meant to show:
*            1. a file and a device come in through the *same* open(): a name
*               that starts with '/' is a VFS path, anything else is a device.
*               Here both forms are exercised, and fstat tells them apart by
*               S_ISREG / S_ISCHR.
*            2. the namespace is reachable from an App with the plain C
*               library calls, so a desktop program that uses them ports over
*               by changing the include list, not the program structure.
*
*          The checks run once and print pass/fail. Every check states what it
*          compared; a failed check prints the number it actually saw, so the
*          report can be acted on instead of guessed at.
*
*          Written for the dev slot 3 window (8 KB heap): one thread, a few
*          hundred bytes of buffers.
* @author xw
* @date 2026.09.23
*/

#include <stdio.h>
#include <string.h>

/* Quoted on purpose. The toolchain ships its own <errno.h> because it is an
 * ISO C header, and an angle-bracket include finds that one first - it has no
 * ENOENT and would shadow the App SDK's copy. The POSIX headers below are not
 * shipped by the toolchain, so they stay in angle form. A desktop program can
 * keep "errno.h" in quotes too: it falls back to the system header there. */
#include "errno.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>

#define FILE_PATH   "/fsdemo.txt"
#define DEV_DIR     "/dev"
#define DEV_PATH    "/dev/COM1"    /* the console UART as this board registers it */
#define BAD_PATH    "/no/such/file"

static const char g_payload[] = "hello svcrtos vfs\n";   /* 18 bytes */

static unsigned g_pass;
static unsigned g_fail;

/**
* @brief Record one check. Prints the check name and, on failure, the observed
*        value so the line is evidence rather than a verdict.
*/
static void check(const char *what, int ok, int seen)
{
    if(ok != 0)
    {
        g_pass++;
        printf("  ok   %-28s\n", what);
    }
    else
    {
        g_fail++;
        printf("  FAIL %-28s (saw %d)\n", what, seen);
    }
}

/**
* @brief Create the file, write the payload, close it.
*/
static void t_write(void)
{
    int fd;
    int n;

    fd = open(FILE_PATH, O_CREAT | O_WRONLY | O_TRUNC);
    check("open O_CREAT|O_WRONLY", fd >= 0, fd);
    if(fd < 0)
    {
        return;
    }
    n = (int)write(fd, g_payload, (unsigned)(sizeof(g_payload) - 1u));
    check("write full payload", n == (int)(sizeof(g_payload) - 1u), n);
    check("close", close(fd) == 0, -1);
}

/**
* @brief Reopen it and compare the bytes that come back.
*/
static void t_read(void)
{
    char buf[64];
    int  fd;
    int  n;

    memset(buf, 0, sizeof(buf));
    fd = open(FILE_PATH, O_RDONLY);
    check("open O_RDONLY", fd >= 0, fd);
    if(fd < 0)
    {
        return;
    }
    n = (int)read(fd, buf, (unsigned)(sizeof(g_payload) - 1u));
    check("read byte count", n == (int)(sizeof(g_payload) - 1u), n);
    check("read content equal", memcmp(buf, g_payload,
          sizeof(g_payload) - 1u) == 0, -1);
    close(fd);
}

/**
* @brief lseek in both directions, then stat the descriptor and the path.
*/
static void t_seek_stat(void)
{
    char          buf[8];
    struct stat   st;
    int           fd;
    int           off;

    fd = open(FILE_PATH, O_RDONLY);
    check("reopen for seek", fd >= 0, fd);
    if(fd < 0)
    {
        return;
    }

    off = (int)lseek(fd, 6, SEEK_SET);
    check("lseek SEEK_SET 6", off == 6, off);
    memset(buf, 0, sizeof(buf));
    read(fd, buf, 5u);
    check("read after seek == \"svcrt\"", memcmp(buf, "svcrt", 5u) == 0, -1);

    off = (int)lseek(fd, 0, SEEK_END);
    check("lseek SEEK_END == size", off == (int)(sizeof(g_payload) - 1u), off);

    if(fstat(fd, &st) == 0)
    {
        check("fstat is regular", S_ISREG(st.st_mode), (int)st.st_mode);
        check("fstat size", st.st_size == (svcrt_off_t)(sizeof(g_payload) - 1u),
              (int)st.st_size);
    }
    else
    {
        check("fstat is regular", 0, -1);
    }
    close(fd);

    if(stat(FILE_PATH, &st) == 0)
    {
        check("stat is regular", S_ISREG(st.st_mode), (int)st.st_mode);
        check("stat size", st.st_size == (svcrt_off_t)(sizeof(g_payload) - 1u),
              (int)st.st_size);
    }
    else
    {
        check("stat is regular", 0, -1);
    }
}

/**
* @brief Find the file by walking the root directory. This is the check that
*        proves the directory stream and the file share one namespace: the
*        name seen here is the name open() used.
*/
static void t_readdir(void)
{
    DIR           *d;
    struct dirent *e;
    int            found = 0;

    d = opendir("/");
    check("opendir /", d != NULL, 0);
    if(d == NULL)
    {
        return;
    }
    while((e = readdir(d)) != NULL)
    {
        if(strcmp(e->d_name, "fsdemo.txt") == 0)
        {
            found = 1;
            check("readdir entry is regular", e->d_type == DT_REG, (int)e->d_type);
        }
    }
    check("errno clean at end of dir", errno == 0, errno);
    check("readdir found the file", found, found);
    closedir(d);
}

/**
* @brief A device and a file open through the same call and are told apart by
*        fstat, which is the whole point of the "path is a name" design.
*        The device is enumerated first and then opened by its registered
*        name - on this board the console is "COM1", not "uart0".
*/
static void t_device(void)
{
    struct stat   st;
    DIR          *d;
    struct dirent *e;
    const char   *banner = "[fs_demo] device opened as a path\n";
    int           chars = 0;
    int           named = 0;
    int           fd;
    int           n;
    int           spin;
    int           written;
    unsigned      total;

    d = opendir(DEV_DIR);
    check("opendir /dev", d != NULL, 0);
    if(d != NULL)
    {
        while((e = readdir(d)) != NULL)
        {
            if(e->d_type == DT_CHR)
            {
                chars++;
            }
            if(strcmp(e->d_name, "COM1") == 0)
            {
                named = 1;
            }
        }
        closedir(d);
    }
    check("/dev has char devices", chars > 0, chars);
    check("/dev lists COM1", named, named);

    fd = open(DEV_PATH, O_WRONLY);
    check("open /dev/COM1 as path", fd >= 0, fd);
    if(fd < 0)
    {
        return;
    }
    if(fstat(fd, &st) == 0)
    {
        check("fstat is char device", S_ISCHR(st.st_mode), (int)st.st_mode);
    }
    else
    {
        check("fstat is char device", 0, -1);
    }
    total = (unsigned)strlen(banner);

    /* A character device may accept fewer bytes than asked: the driver's
     * transmit pipe is a fixed ring, and a write that lands while it is full
     * puts nothing in and returns 0. That is a short write, which POSIX
     * allows on a device, not an error - so the honest client loop is the one
     * the kernel's own console writers use: push a byte, let the transmit
     * interrupt make room, push the next. Bounded, so a dead line cannot
     * hang the App. */
    n = (int)write(fd, banner, total);
    check("single write is not an error", n >= 0, n);
    written = (n > 0) ? n : 0;
    for(spin = 0; (written < (int)total) && (spin < 200000); spin++)
    {
        n = (int)write(fd, banner + written, 1u);
        if(n < 0)
        {
            break;
        }
        written += n;
    }
    check("write to device path", written == (int)total, written);
    close(fd);
}

/**
* @brief Missing names must fail and say why; a namespace that answers a
*        fabricated result would be worse than one that fails.
*/
static void t_errors(void)
{
    struct stat st;
    DIR        *d;

    errno = 0;
    check("stat missing -> -1", stat(BAD_PATH, &st) == -1, -1);
    check("stat missing -> ENOENT", errno == ENOENT, errno);

    errno = 0;
    d = opendir("/no/such/dir");
    check("opendir missing -> NULL", d == NULL, 0);
    check("opendir missing -> ENOENT", errno == ENOENT, errno);
    if(d != NULL)
    {
        closedir(d);
    }
}

/**
* @brief Remove the file and confirm it is gone.
*/
static void t_unlink(void)
{
    struct stat st;

    check("unlink", unlink(FILE_PATH) == 0, -1);
    errno = 0;
    check("gone after unlink", stat(FILE_PATH, &st) == -1, -1);
    check("gone -> ENOENT", errno == ENOENT, errno);
}

int main(void)
{
    printf("\n");
    printf("fs_demo: file-class POSIX over the VFS namespace\n");

    t_write();
    t_read();
    t_seek_stat();
    t_readdir();
    t_device();
    t_errors();
    t_unlink();

    printf("fs_demo: pass=%u fail=%u\n", g_pass, g_fail);
    printf("fs_demo: entering heartbeat\n");

    for(;;)
    {
        sleep(5);
        printf("heartbeat\n");
    }
}
