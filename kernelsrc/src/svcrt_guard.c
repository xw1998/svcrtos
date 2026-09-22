/**
* @file svcrt_guard.c
* @brief Watchdog gating and the per-slot heartbeat contract - see svcrt_guard.h
*/

#include "svcrt_guard.h"
#include "svcrt_cfg.h"
#include "svcrt_task.h"
#include "svcrt_ptable.h"
#include "svcrt_audit.h"
#include "svcrt_crash.h"
#include "svcrt_fault.h"
#include "svcrt_log.h"
#include "svcrt_hal.h"
#include "svcrt_config.h"    /* feature gates: SVCRT_USE_KERNEL_GUARD */
#if SVCRT_USE_KERNEL_GUARD

#ifndef SVCRT_WDG_ENABLE
#define SVCRT_WDG_ENABLE           (0)
#endif

#ifndef SVCRT_WDG_TIMEOUT_MS
#define SVCRT_WDG_TIMEOUT_MS       (4000u)
#endif

/* Service period: the shortest window the contract can be judged against, and
 * the granularity of every age this module reports. */
#define SVCRT_GUARD_SERVICE_MS     (SVCRT_GUARD_MIN_PERIOD_MS)

/* Self-audit cadence. The audit is a few hundred loads, so running it once a
 * second is noise; running it every service period would buy nothing, because
 * a corrupted structure stays corrupted. */
#define SVCRT_GUARD_AUDIT_DIV      (10u)

/* For the tick arithmetic. */
#define SVCRT_GUARD_TICKS_PER_MS   (1000u / SVCRT_TICK_PERIOD_US)
#define SVCRT_GUARD_SERVICE_TICKS  (SVCRT_GUARD_SERVICE_MS * SVCRT_GUARD_TICKS_PER_MS)

/** @brief "no evidence recorded yet" */
#define SVCRT_GUARD_AGE_NONE       (0xFFFFFFFFu)

typedef struct {
    uint32 period_ms;       /* 0 = no contract declared for this task */
    uint32 beat_tick;       /* last explicit heartbeat  (0 = never) */
    uint32 svc_tick;        /* last SVC call from this task (0 = never) */
    uint32 cpu_tick;        /* last time it was the running task (0 = never) */
    uint32 misses;          /* service periods spent in violation */
    uint32 task_id;         /* mirrored so a report is readable without the TCB */
} svcrt_guard_entry_t;

/* Indexed by task id (1-based) so the hot path is one array store. Index 0 is
 * unused: task id 0 is the idle task, which every SVC path excludes anyway. */
static svcrt_guard_entry_t g_guard[SVCRT_TASK_MAX_NUM + 1];

static svcrt_guard_status_t g_status;
static uint32 g_audit_div = 0u;

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

static uint32 guard_age_ms(uint32 now_tick, uint32 stamp_tick)
{
    if(stamp_tick == 0u)
    {
        return SVCRT_GUARD_AGE_NONE;
    }
    return (uint32)((now_tick - stamp_tick) / SVCRT_GUARD_TICKS_PER_MS);
}

static uint32 guard_is_violating(const svcrt_guard_entry_t *p_e, uint32 now_tick)
{
    uint32 window_ticks;

    if((p_e->period_ms == 0u) || (p_e->task_id == 0u))
    {
        return 0u;
    }

    window_ticks = p_e->period_ms * SVCRT_GUARD_TICKS_PER_MS;
    if((now_tick - p_e->beat_tick) <= window_ticks)
    {
        return 0u;      /* the image reported in on time */
    }

    return 1u;
}

/* ------------------------------------------------------------------ */
/* public entry points                                                 */
/* ------------------------------------------------------------------ */

void svcrt_guard_init(void)
{
    uint32 i;

    for(i = 0u; i <= SVCRT_TASK_MAX_NUM; i++)
    {
        g_guard[i].period_ms = 0u;
        g_guard[i].beat_tick = 0u;
        g_guard[i].svc_tick  = 0u;
        g_guard[i].cpu_tick  = 0u;
        g_guard[i].misses    = 0u;
        g_guard[i].task_id   = 0u;
    }

    g_status.enabled    = (uint32)SVCRT_WDG_ENABLE;
    g_status.timeout_ms = 0u;
    g_status.feeds      = 0u;
    g_status.starves    = 0u;
    g_status.verdict    = SVCRT_GUARD_OK;
    g_status.audit_mask = 0u;
    g_status.audit_runs = 0u;
    g_status.violating  = 0u;
    g_status.declared   = 0u;
    g_audit_div         = 0u;

    #if (SVCRT_WDG_ENABLE == 1)
    if(svcrt_port_wdg_init((uint32)SVCRT_WDG_TIMEOUT_MS) != 0)
    {
        /* Requested but not running: say so instead of leaving the reader to
         * conclude from "enabled = 1" that a watchdog is protecting the board. */
        SVCRT_LOGE("GUARD", "watchdog requested but the port refused it (%u ms)",
                   (uint32)SVCRT_WDG_TIMEOUT_MS);
    }
    else
    {
        g_status.timeout_ms = svcrt_port_wdg_timeout_ms();
        SVCRT_LOGI("GUARD", "watchdog armed, hardware timeout %u ms",
                   (uint32)g_status.timeout_ms);
    }
    #endif
}

void svcrt_guard_svc(int32 task_id)
{
    uint32 now;

    if((task_id <= 0) || (task_id > SVCRT_TASK_MAX_NUM))
    {
        return;
    }

    now = svcrt_kernel_get_tick();
    g_guard[task_id].svc_tick = now;
    g_guard[task_id].task_id  = (uint32)task_id;
}

int32 svcrt_guard_beat(int32 task_id, uint32 period_ms)
{
    uint32 now;

    if((task_id <= 0) || (task_id > SVCRT_TASK_MAX_NUM))
    {
        return -1;
    }

    if((period_ms != 0u) && (period_ms < SVCRT_GUARD_MIN_PERIOD_MS))
    {
        /* Refuse rather than accept a contract that cannot be checked: a
         * window shorter than the service period would be judged by sampling,
         * which produces verdicts that are not evidence. */
        return -1;
    }

    now = svcrt_kernel_get_tick();

    if(period_ms == 0u)
    {
        if(g_guard[task_id].period_ms != 0u)
        {
            g_status.declared--;
        }
        g_guard[task_id].period_ms = 0u;
        return 0;
    }

    if(g_guard[task_id].period_ms == 0u)
    {
        g_status.declared++;
    }
    g_guard[task_id].period_ms = period_ms;
    g_guard[task_id].beat_tick = now;
    g_guard[task_id].task_id   = (uint32)task_id;

    return 0;
}

void svcrt_guard_tick(void)
{
    uint32 now;
    uint32 violating = 0u;
    int32  id;

    now = svcrt_kernel_get_tick();

    /* Free evidence: whatever was on the CPU at this tick did run. Deliberately
     * not used to satisfy the contract (see the header). */
    if((svcrt_current_task_id > 0) && (svcrt_current_task_id <= SVCRT_TASK_MAX_NUM))
    {
        g_guard[svcrt_current_task_id].cpu_tick  = now;
        g_guard[svcrt_current_task_id].task_id   = (uint32)svcrt_current_task_id;
    }

    if((now % SVCRT_GUARD_SERVICE_TICKS) != 0u)
    {
        return;
    }

    /* ---- contract ---- */
    for(id = 1; id <= SVCRT_TASK_MAX_NUM; id++)
    {
        svcrt_guard_entry_t *p_e = &g_guard[id];

        if(p_e->period_ms == 0u)
        {
            continue;
        }

        /* The task is gone, so its contract goes with it. Without this the
         * entry would outlive the task and be charged to whoever is handed
         * the id next. */
        if((id > svcrt_task_count) ||
           ((uint32)svcrt_task_table[id - 1].status == SVCRT_TASK_INVALID))
        {
            p_e->period_ms = 0u;
            g_status.declared--;
            continue;
        }

        /* Judged on the beat age alone, whatever the task happens to be doing
         * at this instant: the period is the image's own promise, so a sleep
         * longer than the period is a broken promise, not an excuse. Excusing
         * blocked tasks would make the contract unenforceable for the ordinary
         * shape of an app loop, and an image that never wakes up would never
         * be caught. */
        if(guard_is_violating(p_e, now) != 0u)
        {
            p_e->misses++;
            violating++;
        }
    }

    g_status.violating = violating;

    /* ---- structure audit, at a slower cadence ---- */
    g_audit_div++;
    if((g_audit_div % SVCRT_GUARD_AUDIT_DIV) == 0u)
    {
        svcrt_audit_result_t r;

        svcrt_audit_run(&r);
        g_status.audit_mask = r.mask;
        g_status.audit_runs++;
    }

    g_status.verdict = SVCRT_GUARD_OK;
    if(g_status.audit_mask != 0u)
    {
        g_status.verdict |= SVCRT_GUARD_BAD_AUDIT;
    }
    if(violating != 0u)
    {
        g_status.verdict |= SVCRT_GUARD_BAD_BEAT;
    }

    /* ---- feed or starve ---- */
    if(g_status.enabled == 0u)
    {
        return;                 /* board asked for no watchdog */
    }
    if(g_status.timeout_ms == 0u)
    {
        return;                 /* requested but not armed; nothing to feed */
    }

    if(g_status.verdict == SVCRT_GUARD_OK)
    {
        svcrt_port_wdg_feed();
        g_status.feeds++;
    }
    else
    {
        /* Record the reason once per verdict change, not every period: a
         * full fault ring would otherwise bury the first occurrence. */
        if(g_status.starves == 0u)
        {
            /* Charge this reset to the images that broke their contract,
             * before the watchdog ends the run.
             *
             * Once per boot, not once per tick. The latch is g_status.starves,
             * which this function never clears after the first starvation: the
             * tick re-evaluates the same violation every period, so charging per
             * evaluation would turn one broken image into hundreds of "faults"
             * and hold a slot for the single condition it was already in. That
             * rate limit also sets the convergence speed - a slot that keeps
             * breaking its promise gains exactly one fault per boot, so it is
             * held on the boot after its count reaches the limit. Deliberately
             * not per episode: an image whose health flickers would otherwise
             * be accused faster than a reset can happen.
             *
             * Without the charge at all, the count dies with the RAM it lives in,
             * the image autostarts again and the board reboots forever instead
             * of holding one slot. */
            for(id = 1; id <= SVCRT_TASK_MAX_NUM; id++)
            {
                svcrt_guard_entry_t *p_v = &g_guard[id];
                int32 slot;
                uint32 n;

                if((p_v->period_ms == 0u) || (p_v->task_id == 0u) ||
                   (guard_is_violating(p_v, now) == 0u))
                {
                    continue;
                }

                slot = svcrt_ptable_find_task((uint32)id);

                /* No slot to tag (not an image task), or already held: the
                 * number would only grow without changing any decision. */
                if((slot < 0) || (svcrt_crash_disabled((uint32)slot) != 0u))
                {
                    continue;
                }

                n = svcrt_crash_fault((uint32)slot, SVCRT_CRASH_REASON_HEARTBEAT);

                if(svcrt_crash_disabled((uint32)slot) != 0u)
                {
                    SVCRT_LOGE("CRASH", "slot %d held: %s, %u consecutive faults"
                               " (declared %u ms, silent for %u ticks)",
                               (int)slot,
                               svcrt_crash_reason_name(svcrt_crash_disabled((uint32)slot)),
                               (unsigned)n,
                               (unsigned)p_v->period_ms,
                               (unsigned)(now - p_v->beat_tick));
                }
            }

            if((g_status.verdict & SVCRT_GUARD_BAD_AUDIT) != 0u)
            {
                svcrt_fault_record(SVCRT_FAULT_AUDITFAIL, 0);
            }
            if((g_status.verdict & SVCRT_GUARD_BAD_BEAT) != 0u)
            {
                svcrt_fault_record(SVCRT_FAULT_GUARDHB, 0);
            }
            SVCRT_LOGE("GUARD", "not feeding: verdict 0x%X (audit 0x%X, violating %u)",
                       (uint32)g_status.verdict, (uint32)g_status.audit_mask,
                       (uint32)violating);
        }
        g_status.starves++;
    }
}

void svcrt_guard_status(svcrt_guard_status_t *p_out)
{
    if(p_out == 0)
    {
        return;
    }
    *p_out = g_status;
}

uint32 svcrt_guard_task_slots(void)
{
    return (uint32)SVCRT_TASK_MAX_NUM;
}

int32 svcrt_guard_task_at(uint32 idx, svcrt_guard_task_t *p_out)
{
    const svcrt_guard_entry_t *p_e;
    int32 slot;
    int32 id;
    uint32 now;

    if((p_out == 0) || (idx >= (uint32)SVCRT_TASK_MAX_NUM))
    {
        return -1;
    }

    id = (int32)(idx + 1u);
    p_e = &g_guard[id];
    now = svcrt_kernel_get_tick();

    p_out->task_id     = (id <= svcrt_task_count) ? id : 0;
    p_out->slot        = -1;
    p_out->declared    = (p_e->period_ms != 0u) ? 1u : 0u;
    p_out->period_ms   = p_e->period_ms;
    p_out->state       = 0xFFFFFFFFu;
    p_out->beat_age_ms = guard_age_ms(now, p_e->beat_tick);
    p_out->svc_age_ms  = guard_age_ms(now, p_e->svc_tick);
    p_out->cpu_age_ms  = guard_age_ms(now, p_e->cpu_tick);
    p_out->misses      = p_e->misses;

    if(p_out->task_id != 0)
    {
        p_out->state = (uint32)svcrt_task_table[id - 1].status;
        slot = svcrt_ptable_find_task((uint32)id);
        if(slot >= 0)
        {
            p_out->slot = slot;
        }
    }

    return 0;
}

#else   /* SVCRT_USE_KERNEL_GUARD == 0 */
/* Heartbeat contract compiled out.  Recording is a no-op (there is no table
 * to record into) and an explicit beat is refused, so an application that
 * asks for supervision is told "no", not "yes" - a caller that trusts a
 * silent success would ship a watchdog contract nobody is keeping. */

#include <string.h>

void svcrt_guard_init(void)
{
}

void svcrt_guard_tick(void)
{
}

void svcrt_guard_svc(int32 task_id)
{
    (void)task_id;
}

int32 svcrt_guard_beat(int32 task_id, uint32 period_ms)
{
    (void)task_id;
    (void)period_ms;
    return -1;
}

void svcrt_guard_status(svcrt_guard_status_t *p_out)
{
    if(p_out != 0)
    {
        memset(p_out, 0, sizeof(*p_out));
    }
}

int32 svcrt_guard_task_at(uint32 idx, svcrt_guard_task_t *p_out)
{
    (void)idx;
    (void)p_out;
    return -1;
}

uint32 svcrt_guard_task_slots(void)
{
    return 0u;
}
#endif /* SVCRT_USE_KERNEL_GUARD */
