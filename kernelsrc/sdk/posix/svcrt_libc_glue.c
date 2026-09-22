/**
* @file svcrt_libc_glue.c
* @brief Wire the toolchain C library's bottom end to the kernel: printf works.
* @details The compatibility layer in this directory gives a program the POSIX
*          names. This file is the other half: it makes the names it does *not*
*          re-implement - the C library's own `printf`, `puts`, `strcpy`,
*          `malloc` - usable inside an App.
*
*          What it does:
*            - disables semihosting, so the library never tries to reach a
*              debugger that is not there;
*            - routes stdout/stderr to the kernel console device through
*              svcrt_dev_write();
*            - unbuffers stdout/stderr, because nothing else on the board is
*              going to flush them and a printf without a newline would sit in
*              a buffer forever.
*
*          What it deliberately does NOT do:
*            - stdin. There is no line discipline, no echo and no owner of the
*              console to hand it to, so `fgetc`/`scanf` report end of input.
*              A program that wants characters uses svcrt_dev_read().
*            - files. `fopen` links, but the open hook answers "no such
*              stream" for anything that is not the terminal, so `fopen`
*              returns NULL instead of handing back a plausible empty file.
*              Files are reachable through the VFS (see docs/VFS路径命名空间.md).
*            - the heap. `malloc` is answered by the toolchain, whose heap is
*              whatever the scatter file gives it (ARM_LIB_HEAP, produced by
*              tools/gen_scatter.py --heap-size). This file does not silently
*              divert it to the App arena; that is what
*              SVCRT_POSIX_WRAP_STDLIB is for, and it has to be asked for.
*
* @author xw
* @date 2026.09.22
*/

#include <stdio.h>

#include "svcrt.h"

/* ------------------------------------------------------------------
 * Build switches
 * ------------------------------------------------------------------ */

/* Which device the C library's output goes to, and with what parameter. */
#ifndef SVCRT_GLUE_CONSOLE_DEV
#define SVCRT_GLUE_CONSOLE_DEV     "COM1"
#endif
#ifndef SVCRT_GLUE_CONSOLE_PARAM
#define SVCRT_GLUE_CONSOLE_PARAM   (115200u)
#endif

/* AC5 (armcc) and AC6 (armclang) expose different retarget hooks. Both are
 * supported so the same App source compiles under either, which is the whole
 * point of this file. */
#if defined(__ARMCC_VERSION) && (__ARMCC_VERSION >= 6000000)
#define SVCRT_GLUE_AC6   1
#else
#define SVCRT_GLUE_AC6   0
#endif

/* Keep the library away from semihosting. Without this the first printf may
 * hang waiting for a debugger, and on a board with none it ends in a fault
 * far away from the call that caused it. */
#if SVCRT_GLUE_AC6
__asm(".global __use_no_semihosting\n\t");
#else
#pragma import(__use_no_semihosting)
#endif

/* ------------------------------------------------------------------
 * Console
 * ------------------------------------------------------------------ */

static int32 g_glue_out    = -1;    /* console handle, or -1 when not open */
static uint8 g_glue_tried  = 0u;    /* open attempted (success or not)      */

/**
* @brief Open the console once, on first use.
* @note  A failure is remembered on purpose: retrying on every byte would turn
*        one missing device into thousands of SVC calls, and the answer will
*        not change by itself.
*/
static void glue_open(void)
{
    if(g_glue_tried != 0u)
    {
        return;
    }
    g_glue_tried = 1u;
    g_glue_out   = svcrt_dev_open((char *)SVCRT_GLUE_CONSOLE_DEV,
                                  (uint32)SVCRT_GLUE_CONSOLE_PARAM);
}

/**
* @brief Prepare stdio. Optional - the first output call does it too.
*/
void svcrt_stdio_init(void)
{
    glue_open();

    /* Nothing on the board flushes stdout for us, and a line-buffered stream
     * would hold "done." until a newline arrives that may never come. */
    (void)setvbuf(stdout, NULL, _IONBF, 0);
    (void)setvbuf(stderr, NULL, _IONBF, 0);
}

/** @brief 1 while the console is usable */
int svcrt_stdio_ready(void)
{
    if(g_glue_out < 0)
    {
        svcrt_stdio_init();
    }
    return (g_glue_out >= 0) ? 1 : 0;
}

/** @brief Write a byte run to the console; returns bytes written or -1 */
static int glue_write(const unsigned char *buf, unsigned len)
{
    int32 written;

    if((buf == 0) || (len == 0u))
    {
        return 0;
    }
    if(svcrt_stdio_ready() == 0)
    {
        return -1;
    }
    written = svcrt_dev_write(g_glue_out, (void *)buf, (int32)len);
    if(written < 0)
    {
        return -1;
    }
    return (int)written;
}

/* ------------------------------------------------------------------
 * Retarget hooks
 * ------------------------------------------------------------------ */

/**
* @brief The one hook every printf-family call funnels through.
* @note  Returning EOF on a failed write is the only honest answer: claiming
*        the character went out would hide a console that is not there.
*/
int fputc(int ch, FILE *f)
{
    unsigned char c = (unsigned char)ch;

    (void)f;
    if(glue_write(&c, 1u) != 1)
    {
        return EOF;
    }
    return (int)c;
}

/** @brief Char-by-char console output (used by debuggers and some library paths) */
void _ttywrch(int ch)
{
    unsigned char c = (unsigned char)ch;

    (void)glue_write(&c, 1u);
}

/**
* @brief Input side.
* @note  There is no stdin on this board: no line discipline, no echo, and the
*        console belongs to whoever opened it. Reporting end of input is the
*        truthful answer; inventing a blocking read here would take the
*        console away from the shell without saying so.
*/
int fgetc(FILE *f)
{
    (void)f;
    return EOF;
}

/** @brief Process exit. An App has no process to end; park the thread. */
void svcrt_glue_exit(int code)
{
    (void)code;
    for(;;)
    {
        svcrt_task_wait(1000u);
    }
}

#if SVCRT_GLUE_AC6
/* ---------------- Arm Compiler 6 (armclang) ---------------- */

int _write(int fd, char *buf, int len)
{
    (void)fd;
    return glue_write((const unsigned char *)buf, (unsigned)len);
}

int _read(int fd, char *buf, int len)
{
    (void)fd;
    (void)buf;
    (void)len;
    return 0;       /* end of input, see fgetc */
}

void _exit(int code)
{
    svcrt_glue_exit(code);
}

#else
/* ---------------- Arm Compiler 5 (armcc) ---------------- */

/*
 * The library ships its own sys_io.o (../clib/angel/sysapp.c) which defines
 * this whole family as semihosting calls. __use_no_semihosting forbids that,
 * so every one of them has to be answered here: define only some and the
 * linker pulls sys_io.o in for the others, then reports them as multiply
 * defined (L6200E).
 *
 * ":tt" is the name the library uses for the terminal streams. It is the only
 * stream that exists on this board - a C library file layer would need a
 * filesystem and the App has the VFS instead. Answering -1 for every other
 * name is what makes fopen() fail loudly instead of handing back a plausible
 * empty file.
 */
#define SVCRT_GLUE_TTY_HANDLE   (1)

/* The library's own stdio streams hold the name they were opened with, and
 * that name lives in sys_io.o too. Defining it here is what keeps sys_io.o
 * out of the image: as long as nothing references it, its semihosting
 * versions of _sys_* are not linked and the ones below are. The type and
 * contents match what sys_io.o has (a 4 byte ":tt" array per stream), so a
 * name based open from anywhere still lands on the console. */
const char __stdin_name[]  = ":tt";
const char __stdout_name[] = ":tt";
const char __stderr_name[] = ":tt";

/** @brief NUL terminated string equality, without <string.h>: this file is
 *         the bottom of the C library and should not depend on it. */
static int glue_streq(const char *a, const char *b)
{
    if((a == 0) || (b == 0))
    {
        return 0;
    }
    while((*a != '\0') && (*a == *b))
    {
        a++;
        b++;
    }
    return (*a == *b) ? 1 : 0;
}

int _sys_open(const char *name, int openmode)
{
    (void)openmode;
    return (glue_streq(name, ":tt") != 0) ? SVCRT_GLUE_TTY_HANDLE : -1;
}

int _sys_close(FILE *f)
{
    (void)f;
    return 0;
}

int _sys_write(FILE *f, const unsigned char *buf, unsigned len, int mode)
{
    (void)f;
    (void)mode;
    return glue_write(buf, len);
}

int _sys_read(FILE *f, unsigned char *buf, unsigned len, int mode)
{
    (void)f;
    (void)buf;
    (void)len;
    (void)mode;
    return 0;       /* end of input, see fgetc */
}

/** @brief 1 = interactive terminal, so the library goes through fputc/fgetc.
 *         That is the path we want: unbuffered, one byte per write. */
int _sys_istty(FILE *f)
{
    (void)f;
    return 1;
}

/** @brief A console has no file position, so it cannot be seeked. */
int _sys_seek(FILE *f, long pos)
{
    (void)f;
    (void)pos;
    return -1;
}

/** @brief Flush request. Output is unbuffered, so there is nothing to do. */
int _sys_ensure(FILE *f)
{
    (void)f;
    return 0;
}

/** @brief Length is unknown for a console stream. */
long _sys_flen(FILE *f)
{
    (void)f;
    return -1L;
}

void _sys_exit(int code)
{
    svcrt_glue_exit(code);
}

#endif /* SVCRT_GLUE_AC6 */
