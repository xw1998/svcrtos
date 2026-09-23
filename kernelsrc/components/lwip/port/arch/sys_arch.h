/**
* @file sys_arch.h
* @brief lwIP 的"操作系统"面：把 sem/mbox/mutex/thread 映射到 SVCrtOS 内核对象。
* @details 四类对象一一对应，没有自造轮子：
*            sys_sem   → svcrt_sem_create/wait/post
*            sys_mbox  → svcrt_mq_create/send/recv
*            sys_mutex → svcrt_mutex_create/lock/unlock
*            thread    → svcrt_task_register（静态栈，见 sys_arch.c）
*
*          两个必须记住的点：
*            1) lwIP 的 timeout==0 表示"永久等待"，而 SVCrtOS 的 0 表示"不等待"、
*               负值才是永久。sys_arch.c 里统一做了换算，不要照抄参数。
*            2) 句柄一律 int32，-1 = 无效（SYS_SEM_NULL / SYS_MBOX_NULL ...），
*               这样 sys_sem_valid() 这些宏才有东西可判。
*
* @author xw
* @date 2026.09.23
*/
#ifndef LWIP_ARCH_SYS_ARCH_H
#define LWIP_ARCH_SYS_ARCH_H

#include <stdint.h>

/** @brief 句柄类型：SVCrtOS 的内核对象号，-1 表示无效。 */
typedef int32_t sys_thread_t;

typedef int32_t sys_sem_t;
typedef int32_t sys_mbox_t;
typedef int32_t sys_mutex_t;

#define SYS_SEM_NULL    (-1)
#define SYS_MBOX_NULL   (-1)
#define SYS_MUTEX_NULL  (-1)
#define SYS_THREAD_NULL ((sys_thread_t)0)   /* 0 是内核保留的"不是任务" */

/* lwIP 传进来的是这些类型的**地址**，所以宏里要解引用。 */
#define sys_sem_valid(sem)          (((sem) != NULL) && (*(sem) >= 0))
#define sys_sem_set_invalid(sem)    do { if((sem) != NULL) { *(sem) = SYS_SEM_NULL; } } while(0)
#define sys_mbox_valid(mbox)        (((mbox) != NULL) && (*(mbox) >= 0))
#define sys_mbox_set_invalid(mbox)  do { if((mbox) != NULL) { *(mbox) = SYS_MBOX_NULL; } } while(0)

/* ---- 临界区：PRIMASK 存/取，可嵌套（返回上一次的掩码） ---- */
typedef uint32_t sys_prot_t;

#if defined(__CC_ARM) && !defined(__clang__)
static __inline sys_prot_t lwip_primask_enter(void)
{
    sys_prot_t pm;
    __asm { MRS pm, PRIMASK }
    __asm { CPSID i }
    return pm;
}
static __inline void lwip_primask_exit(sys_prot_t pm)
{
    __asm { MSR PRIMASK, pm }
}
#else
static inline sys_prot_t lwip_primask_enter(void)
{
    sys_prot_t pm;
    __asm volatile ("MRS %0, PRIMASK" : "=r" (pm));
    __asm volatile ("CPSID i" ::: "memory");
    return pm;
}
static inline void lwip_primask_exit(sys_prot_t pm)
{
    __asm volatile ("MSR PRIMASK, %0" :: "r" (pm) : "memory");
}
#endif

#define SYS_ARCH_DECL_PROTECT(lev)  sys_prot_t lev
#define SYS_ARCH_PROTECT(lev)       do { (lev) = lwip_primask_enter(); } while(0)
#define SYS_ARCH_UNPROTECT(lev)     lwip_primask_exit(lev)

#endif /* LWIP_ARCH_SYS_ARCH_H */
