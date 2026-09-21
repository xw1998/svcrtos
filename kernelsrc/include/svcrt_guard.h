/**
* @file svcrt_guard.h
* @brief Watchdog gating and the per-slot heartbeat contract
* @details Two different jobs share one file because they are one decision:
*          "is the system still making the progress we expect, and if not,
*          should the hardware put an end to it?"
*
*          1. The independent watchdog is the last resort. The kernel owns the
*             counter, so a watchdog that stops being fed means "something the
*             kernel can see is wrong", not "an interrupt went missing".
*          2. The heartbeat is an *application contract*, not a kernel guess.
*             An image calls svcrt_heartbeat(period_ms) to say "I will report in
*             at least this often"; a missing report means that image is stuck
*             inside something the kernel cannot observe. An image that never
*             declares a period is reported as "not declared" - the kernel has
*             no basis for a health verdict and therefore does not give one.
*
*          Evidence the kernel collects on its own (every SVC call, and being
*          the running task at a tick) is kept as separate columns on purpose:
*          folding it into the contract would silently excuse a wedged image
*          that still happens to be on the CPU, and the contract would then
*          have no teeth.
*
*          A declared window is judged on the beat age alone, not on what the
*          task happens to be doing when the check runs. A sleep or a block
*          longer than the declared period is therefore a violation: the
*          period is the image's own promise, so it has to cover the image's
*          own longest wait. Exempting blocked tasks would make the contract
*          unenforceable for the ordinary shape of an app loop - sleep, work,
*          report - and an image that never wakes up would never be caught.
*
*          A contract ends when its task ends: an entry whose task has been
*          deleted is dropped, rather than carried until the id is handed out
*          again and charged to an image that never made the promise.
*
* @note Kernel-only header. Apps use svcrt_heartbeat() from svcrt.h.
*/

#ifndef __SVCRT_GUARD_H__
#define __SVCRT_GUARD_H__

#include "svcrt_types.h"

/** @brief Verdict bits (SVCRT_GUARD_OK means "nothing known to be wrong") */
#define SVCRT_GUARD_OK          (0u)
#define SVCRT_GUARD_BAD_AUDIT   (1u << 0)   /* a structure failed the self-audit */
#define SVCRT_GUARD_BAD_BEAT    (1u << 1)   /* a declared heartbeat window lapsed */

/** @brief Shortest contract the kernel can actually check (equal to the
 *         service period: a window shorter than the sampling interval could
 *         not be judged, and pretending to judge it would be a false verdict). */
#define SVCRT_GUARD_MIN_PERIOD_MS   (100u)

/** @brief One entry of the per-task heartbeat table, as reported to callers */
typedef struct {
    int32  task_id;         /* 0 when this index has no task */
    int32  slot;            /* owning image slot, -1 when the task is not an image */
    uint32 declared;        /* 1 = an image declared a period for this task */
    uint32 period_ms;       /* the declared period (0 = none) */
    uint32 state;           /* TCB status, 0xFFFFFFFF when there is no task */
    uint32 beat_age_ms;     /* since the last explicit heartbeat */
    uint32 svc_age_ms;      /* since the last SVC call from this task */
    uint32 cpu_age_ms;      /* since this task was last the running one */
    uint32 misses;          /* contract windows missed so far */
} svcrt_guard_task_t;

/** @brief Module-level status, for the shell and for a boot report */
typedef struct {
    uint32 enabled;         /* board switch SVCRT_WDG_ENABLE */
    uint32 timeout_ms;      /* timeout the hardware is running with (0 = off) */
    uint32 feeds;           /* times the watchdog was reloaded */
    uint32 starves;         /* times it was deliberately not reloaded */
    uint32 verdict;         /* last verdict bits */
    uint32 audit_mask;      /* last audit mask read into the verdict */
    uint32 audit_runs;      /* audit passes performed */
    uint32 violating;       /* tasks currently in violation */
    uint32 declared;        /* tasks under contract */
} svcrt_guard_status_t;

/**
* @brief Bring up the watchdog (when the board asks for it) and clear the
*        heartbeat table. Called once, after the partition table is populated.
*/
void svcrt_guard_init(void);

/**
* @brief Per-tick bookkeeping: records the running task, and every service
*        period re-evaluates the contract and decides whether to feed.
* @note Called from the tick handler; does no I/O and no allocation.
*/
void svcrt_guard_tick(void);

/**
* @brief Record free evidence: this task called into the kernel.
* @param task_id  1-based task id; values <= 0 (idle/kernel) are ignored.
*/
void svcrt_guard_svc(int32 task_id);

/**
* @brief Record an explicit heartbeat, and optionally declare/replace the
*        period the caller promises to keep.
* @param task_id   1-based task id; <= 0 is rejected.
* @param period_ms 0 = withdraw the contract; otherwise >= SVCRT_GUARD_MIN_PERIOD_MS.
* @return 0 on success, -1 when the task id or the period cannot be accepted.
*/
int32 svcrt_guard_beat(int32 task_id, uint32 period_ms);

/**
* @brief Fill in the module status.
*/
void svcrt_guard_status(svcrt_guard_status_t *p_out);

/**
* @brief Walk the heartbeat table.
* @param idx    0-based table index.
* @param p_out  Receives the entry; task_id is 0 once the table is exhausted.
* @return 0 on success, -1 when @p idx is past the end.
*/
int32 svcrt_guard_task_at(uint32 idx, svcrt_guard_task_t *p_out);

/** @brief Number of entries the walk in svcrt_guard_task_at() covers. */
uint32 svcrt_guard_task_slots(void);

#endif /* __SVCRT_GUARD_H__ */
