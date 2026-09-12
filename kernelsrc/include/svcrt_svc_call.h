/**
* @file svcrt_svc_call.h
* @brief Toolchain-independent SVC entry helpers for the App / Driver SDK.
* @details The unprivileged side (App, Driver) never touches kernel internals:
*          every system service is reached with an SVC instruction carrying the
*          service number as its 8-bit immediate. How that instruction is
*          emitted differs per compiler, which is what this header hides:
*
*            - ARM Compiler 5 (armcc): the __svc(n) intrinsic.
*            - ARM Compiler 6 (armclang) and other compilers: the SVC
*              instruction is emitted directly through inline assembly, with
*              the arguments pinned to r0/r1/r2.
*
*          Both encodings are identical on the wire: the kernel decodes the
*          service number from the SVC immediate in the stacked return address,
*          so the two paths are interchangeable.
*
*          The inline-assembly path follows the AAPCS argument order, which is
*          the same order the kernel expects to find in the exception frame.
*
* @note  Keep this header dependency-free: it is included by both SDKs and by
*        kernel-side code, so it must not pull in board or partition settings.
* @author xw
*/

#ifndef __SVCRT_SVC_CALL_H__
#define __SVCRT_SVC_CALL_H__

#if defined(__CC_ARM)

/* ---------------------------------------------------------------------------
 * ARM Compiler 5 (armcc). The intrinsic emits SVC #num and passes arguments
 * and the return value the ordinary AAPCS way, so a declaration is enough.
 * ------------------------------------------------------------------------- */

/** Declare an SVC service that takes one argument and returns a value. */
#define SVCRT_SVC_DECL_1(ret, num, name, t0)          ret __svc(num) name(t0);
/** Declare an SVC service that takes two arguments and returns a value. */
#define SVCRT_SVC_DECL_2(ret, num, name, t0, t1)      ret __svc(num) name(t0, t1);
/** Declare an SVC service that takes three arguments and returns a value. */
#define SVCRT_SVC_DECL_3(ret, num, name, t0, t1, t2)  ret __svc(num) name(t0, t1, t2);
/** Declare an SVC service that takes one argument and returns nothing. */
#define SVCRT_SVC_DECL_V1(num, name, t0)              void __svc(num) name(t0);
/** Declare an SVC service that takes two arguments and returns nothing. */
#define SVCRT_SVC_DECL_V2(num, name, t0, t1)          void __svc(num) name(t0, t1);

#else

/* ---------------------------------------------------------------------------
 * ARM Compiler 6 (armclang), GCC and Clang. There is no __svc intrinsic, so
 * the wrapper emits the SVC instruction itself. The arguments are pinned to
 * r0/r1/r2 by means of explicit register variables - the very registers the
 * kernel reads out of the exception stack frame - and the result comes back
 * in r0.
 *
 * The service number goes through two levels of stringification so that a
 * macro such as SVCRT_SVC_DRV_MGR is expanded before it becomes text.
 * ------------------------------------------------------------------------- */

#define SVCRT_STR_(x)  #x
#define SVCRT_STR(x)   SVCRT_STR_(x)

/* Some services are declared by a given SDK but only called from the other one;
 * marking the wrappers unused keeps -Wall quiet without changing the code. */
#define SVCRT_INLINE  static inline __attribute__((unused))

/** Declare an SVC service that takes one argument and returns a value. */
#define SVCRT_SVC_DECL_1(ret, num, name, t0)                          \
    SVCRT_INLINE ret name(t0 a0)                                     \
    {                                                                 \
        register unsigned int r0 __asm("r0") = (unsigned int)a0;      \
        __asm volatile ("svc #" SVCRT_STR(num) : "+r"(r0) : : "memory"); \
        return (ret)r0;                                               \
    }

/** Declare an SVC service that takes two arguments and returns a value. */
#define SVCRT_SVC_DECL_2(ret, num, name, t0, t1)                      \
    SVCRT_INLINE ret name(t0 a0, t1 a1)                              \
    {                                                                 \
        register unsigned int r0 __asm("r0") = (unsigned int)a0;      \
        register unsigned int r1 __asm("r1") = (unsigned int)a1;      \
        __asm volatile ("svc #" SVCRT_STR(num)                        \
                        : "+r"(r0) : "r"(r1) : "memory");             \
        return (ret)r0;                                               \
    }

/** Declare an SVC service that takes three arguments and returns a value. */
#define SVCRT_SVC_DECL_3(ret, num, name, t0, t1, t2)                  \
    SVCRT_INLINE ret name(t0 a0, t1 a1, t2 a2)                       \
    {                                                                 \
        register unsigned int r0 __asm("r0") = (unsigned int)a0;      \
        register unsigned int r1 __asm("r1") = (unsigned int)a1;      \
        register unsigned int r2 __asm("r2") = (unsigned int)a2;      \
        __asm volatile ("svc #" SVCRT_STR(num)                        \
                        : "+r"(r0) : "r"(r1), "r"(r2) : "memory");    \
        return (ret)r0;                                               \
    }

/** Declare an SVC service that takes one argument and returns nothing. */
#define SVCRT_SVC_DECL_V1(num, name, t0)                              \
    SVCRT_INLINE void name(t0 a0)                                    \
    {                                                                 \
        register unsigned int r0 __asm("r0") = (unsigned int)a0;      \
        __asm volatile ("svc #" SVCRT_STR(num) : "+r"(r0) : : "memory"); \
    }

/** Declare an SVC service that takes two arguments and returns nothing. */
#define SVCRT_SVC_DECL_V2(num, name, t0, t1)                          \
    SVCRT_INLINE void name(t0 a0, t1 a1)                             \
    {                                                                 \
        register unsigned int r0 __asm("r0") = (unsigned int)a0;      \
        register unsigned int r1 __asm("r1") = (unsigned int)a1;      \
        __asm volatile ("svc #" SVCRT_STR(num)                        \
                        : "+r"(r0) : "r"(r1) : "memory");             \
    }

#endif /* __CC_ARM */

#endif /* __SVCRT_SVC_CALL_H__ */
