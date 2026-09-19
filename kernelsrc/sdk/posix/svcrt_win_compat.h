/**
* @file svcrt_win_compat.h
* @brief Windows style names for programs written against the Win32 API.
* @details Sits on top of svcrt_posix.h so a snippet written for Windows can
*          be moved over with a changed include list instead of a rewrite.
*          Only the calls that have a real counterpart here are provided; the
*          ones that would need a process, a registry or a filesystem are
*          deliberately absent, so their use fails to compile rather than
*          failing mysteriously at run time.
*
*          Two mapping notes worth knowing up front:
*            - CreateThread's start routine is a DWORD(*)(LPVOID) while the
*              thread service wants void*(*)(void*). Both pass their argument
*              and result in r0 under AAPCS, so the pointer is converted and
*              called directly; WINAPI is empty on ARM.
*            - WaitForSingleObject only understands a thread handle joined
*              with INFINITE. Any other handle or timeout reports failure.
*
* @author xw
*/
#ifndef __SVCRT_WIN_COMPAT_H__
#define __SVCRT_WIN_COMPAT_H__

#include "svcrt_posix.h"

typedef uint8  svcrt_WORD;
typedef uint32 svcrt_DWORD;
typedef int32  svcrt_BOOL;
typedef void  *svcrt_HANDLE;
typedef void  *svcrt_LPVOID;
typedef const char *svcrt_LPCSTR;

#define WINAPI
#define TRUE   (1)
#define FALSE  (0)
#define INFINITE  (0xFFFFFFFFu)

#define WAIT_OBJECT_0  (0u)
#define WAIT_TIMEOUT   (0x102u)

/* ---- time and scheduling --------------------------------------------- */

#define Sleep(ms)          svcrt_task_wait((uint32)(ms))
#define GetTickCount()     svcrt_get_time_ms()

/* ---- threads ---------------------------------------------------------- */

svcrt_HANDLE svcrt_win_CreateThread(void *security, uint32 stack_size,
                                    svcrt_DWORD (*start)(svcrt_LPVOID),
                                    svcrt_LPVOID arg, uint32 flags,
                                    uint32 *thread_id);
svcrt_DWORD  svcrt_win_WaitForSingleObject(svcrt_HANDLE h, uint32 ms);
svcrt_BOOL   svcrt_win_CloseHandle(svcrt_HANDLE h);

#define CreateThread(sec, stk, fn, arg, flg, id) \
        svcrt_win_CreateThread((sec), (stk), (fn), (arg), (flg), (id))
#define WaitForSingleObject(h, ms)  svcrt_win_WaitForSingleObject((h), (ms))
#define CloseHandle(h)              svcrt_win_CloseHandle(h)

/* ---- critical sections ------------------------------------------------ */

typedef struct
{
    svcrt_pthread_mutex_t mtx;
} svcrt_CRITICAL_SECTION;

svcrt_BOOL svcrt_win_InitializeCriticalSection(svcrt_CRITICAL_SECTION *cs);
void       svcrt_win_EnterCriticalSection(svcrt_CRITICAL_SECTION *cs);
void       svcrt_win_LeaveCriticalSection(svcrt_CRITICAL_SECTION *cs);
void       svcrt_win_DeleteCriticalSection(svcrt_CRITICAL_SECTION *cs);

#define InitializeCriticalSection(cs)  svcrt_win_InitializeCriticalSection(cs)
#define EnterCriticalSection(cs)       svcrt_win_EnterCriticalSection(cs)
#define LeaveCriticalSection(cs)       svcrt_win_LeaveCriticalSection(cs)
#define DeleteCriticalSection(cs)      svcrt_win_DeleteCriticalSection(cs)

/* ---- bounds checked string helpers ------------------------------------ */

int32      svcrt_win_strcpy_s(char *dst, uint32 dst_size, const char *src);
int32      svcrt_win_strcat_s(char *dst, uint32 dst_size, const char *src);
int        svcrt_win_snprintf_s(char *dst, uint32 dst_size, const char *fmt, ...);

#define strcpy_s(d, n, s)     svcrt_win_strcpy_s((d), (n), (s))
#define strcat_s(d, n, s)     svcrt_win_strcat_s((d), (n), (s))
#define sprintf_s(d, n, ...)  svcrt_win_snprintf_s((d), (n), __VA_ARGS__)
#define ZeroMemory(p, n)      svcrt_posix_memset((p), 0, (n))

#endif /* __SVCRT_WIN_COMPAT_H__ */
