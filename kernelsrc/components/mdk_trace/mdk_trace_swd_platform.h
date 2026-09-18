/* mdk_trace_swd_platform.h - SVCrtOS platform layer for the SWD trace backend.
 *
 * The SWD backend of the mdk_trace component needs four things from the
 * project it is dropped into:
 *
 *   1. a free running cycle counter (SWD_NOW)
 *   2. a critical section that can be nested and restored (SWD_CRIT_ENTER/EXIT)
 *   3. the fault status registers (SWD_SCB_*)
 *   4. stack pointer accessors for the fault snapshot (SWD_PSP / SWD_MSP)
 *
 * The component's built-in path gets these from CMSIS intrinsics. This kernel
 * deliberately keeps vendor headers out of the core sources, so the backend is
 * built with MDK_TRACE_SWD_EXTERNAL_PLATFORM and the primitives live here.
 *
 * Everything below is a plain register access or a single instruction, which
 * is the whole point: these run inside exception handlers, on every recorded
 * event, so nothing here may call into the kernel or touch global state.
 *
 * Two inline assembler dialects are needed. Keil ARMCC 5 (__CC_ARM) has no
 * GCC-style operand syntax at all - the only forms it accepts are the
 * __asm { ... } block and "register T x __asm(\"core-reg\")" - so the special
 * registers below are read with the block form there. ARMCLANG (AC6), GCC and
 * Clang get the operand form, which is what the component itself uses.
 *
 * ASCII only: Keil ARMCC (AC5) mis-renders UTF-8 comments in some setups, same
 * reason the component sources are ASCII.
 */

#ifndef MDK_TRACE_SWD_PLATFORM_H
#define MDK_TRACE_SWD_PLATFORM_H

#include <stdint.h>

/* ----------------------------------------------------------- time base
 * DWT_CYCCNT. mdk_trace_init() enables DEMCR.TRCENA and DWT_CTRL.CYCCNTENA
 * before the first event is recorded, so the counter is already running here.
 * A plain load - safe to do from an ISR at any priority.
 */
#define SWD_NOW()   (*((volatile uint32_t *)0xE0001004u))

/* ------------------------------------------------------ critical section
 * PRIMASK save/restore. The SWD encoder hands the returned value straight
 * back, so nesting and the fault handler paths both behave: an event recorded
 * while interrupts were already masked returns with them still masked.
 */
static uint32_t mdk_trace_swd_plat_crit_enter(void)
{
    uint32_t pm;

#if defined(__CC_ARM) && !defined(__clang__)
    __asm { MRS pm, PRIMASK }
    __asm { CPSID i }
#else
    __asm volatile ("MRS %0, PRIMASK" : "=r" (pm));
    __asm volatile ("CPSID i" ::: "memory");
#endif
    return pm;
}

static void mdk_trace_swd_plat_crit_exit(uint32_t pm)
{
#if defined(__CC_ARM) && !defined(__clang__)
    __asm { MSR PRIMASK, pm }
#else
    __asm volatile ("MSR PRIMASK, %0" :: "r" (pm) : "memory");
#endif
}

#define SWD_CRIT_ENTER()   mdk_trace_swd_plat_crit_enter()
#define SWD_CRIT_EXIT(pm)  mdk_trace_swd_plat_crit_exit(pm)

/* ------------------------------------------------- fault status registers */
#define SWD_SCB_CFSR    (*((volatile uint32_t *)0xE000ED28u))
#define SWD_SCB_HFSR    (*((volatile uint32_t *)0xE000ED2Cu))
#define SWD_SCB_MMFAR   (*((volatile uint32_t *)0xE000ED34u))
#define SWD_SCB_BFAR    (*((volatile uint32_t *)0xE000ED38u))

/* --------------------------------------------------------- stack pointers
 * Used by the fault snapshot to pick which stack the exception frame is on:
 * EXC_RETURN bit 2 clear means the handler runs on MSP, set means PSP.
 */
#define SWD_PSP()       mdk_trace_swd_plat_psp()
#define SWD_MSP()       mdk_trace_swd_plat_msp()

static uint32_t mdk_trace_swd_plat_psp(void)
{
    uint32_t v;

#if defined(__CC_ARM) && !defined(__clang__)
    __asm { MRS v, PSP }
#else
    __asm volatile ("MRS %0, PSP" : "=r" (v));
#endif
    return v;
}

static uint32_t mdk_trace_swd_plat_msp(void)
{
    uint32_t v;

#if defined(__CC_ARM) && !defined(__clang__)
    __asm { MRS v, MSP }
#else
    __asm volatile ("MRS %0, MSP" : "=r" (v));
#endif
    return v;
}

#endif /* MDK_TRACE_SWD_PLATFORM_H */
