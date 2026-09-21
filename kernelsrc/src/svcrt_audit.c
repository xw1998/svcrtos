/**
* @file svcrt_audit.c
* @brief Kernel self-audit -- checks, see svcrt_audit.h for the contract
* @details Every check here is a statement the rest of the kernel already
*          assumes to be true. When one of them is false the structure is
*          wrong, not the check: nothing in this file tries to make the
*          system run anyway.
*/

#include "svcrt_audit.h"
#include "svcrt_cfg.h"
#include "svcrt_ptable.h"
#include "svcrt_share.h"
#include "svcrt_task.h"
#include "svcrt_fault.h"
#include "svcrt_log.h"
#include "svcrt_config.h"
#include "svcrt_app_image.h"
#include "svcrt_layout_def.h"
#include "svcrt_partition.h"

static svcrt_audit_result_t g_audit_last;

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

static uint32 audit_is_pow2(uint32 v)
{
    return ((v != 0u) && ((v & (v - 1u)) == 0u)) ? 1u : 0u;
}

/* Half-open ranges [base, base+size). A zero-sized range never overlaps. */
static uint32 audit_overlap(uint32 a_base, uint32 a_size, uint32 b_base, uint32 b_size)
{
    if((a_size == 0u) || (b_size == 0u))
    {
        return 0u;
    }
    return (((a_base < (b_base + b_size)) && (b_base < (a_base + a_size))) ? 1u : 0u);
}

/* Remember the first offender once per bit: later offenders of the same kind
 * would only bury the one that appeared first. */
static void audit_fail(svcrt_audit_result_t *p, uint32 bit, uint32 index)
{
    if((p->mask & bit) == 0u)
    {
        if(p->index == SVCRT_AUDIT_INDEX_NONE)
        {
            p->index = index;
        }
    }
    p->mask |= bit;
}

/* ------------------------------------------------------------------ */
/* partition table: identity and geometry                              */
/* ------------------------------------------------------------------ */

static void audit_partition(svcrt_audit_result_t *p)
{
    const svcrt_partition_table_t *t = svcrt_ptable_get();
    uint32 flash_end;
    uint32 ram_end;

    if(t == 0)
    {
        audit_fail(p, SVCRT_AUDIT_PARTITION, SVCRT_AUDIT_INDEX_NONE);
        return;
    }

    flash_end = CHIP_FLASH_BASE + CHIP_FLASH_SIZE;
    ram_end   = CHIP_RAM_BASE + CHIP_RAM_SIZE;

    p->checks++;

    /* Identity: a table that does not claim to be this table is not read
     * any further -- its fields would be meaningless. */
    if((t->magic != SVCRT_PARTITION_MAGIC) ||
       (t->version != SVCRT_PARTITION_VERSION) ||
       (t->hw_compat_id == 0u))
    {
        audit_fail(p, SVCRT_AUDIT_PARTITION, SVCRT_AUDIT_INDEX_NONE);
        return;
    }

    /* Flash: kernel and pool must exist and fit in the chip. */
    if((t->kernel_size == 0u) || (t->kernel_base < CHIP_FLASH_BASE) ||
       ((t->kernel_base + t->kernel_size) > flash_end))
    {
        audit_fail(p, SVCRT_AUDIT_PARTITION, SVCRT_AUDIT_INDEX_NONE);
    }
    if((t->pool_size == 0u) || (t->pool_base < CHIP_FLASH_BASE) ||
       ((t->pool_base + t->pool_size) > flash_end))
    {
        audit_fail(p, SVCRT_AUDIT_PARTITION, SVCRT_AUDIT_INDEX_NONE);
    }
    if(audit_overlap(t->kernel_base, t->kernel_size, t->pool_base, t->pool_size) != 0u)
    {
        audit_fail(p, SVCRT_AUDIT_PARTITION, SVCRT_AUDIT_INDEX_NONE);
    }

    /* Pool geometry: sector-aligned, self-consistent, with the reserve
     * accounted for exactly once. */
    if((t->pool_sector == 0u) || ((t->pool_base % t->pool_sector) != 0u) ||
       ((t->pool_size % t->pool_sector) != 0u))
    {
        audit_fail(p, SVCRT_AUDIT_PARTITION, SVCRT_AUDIT_INDEX_NONE);
    }
    if(t->pool_units != (t->pool_size / t->pool_sector))
    {
        audit_fail(p, SVCRT_AUDIT_PARTITION, SVCRT_AUDIT_INDEX_NONE);
    }
    if(t->pool_reserve > t->pool_units)
    {
        audit_fail(p, SVCRT_AUDIT_PARTITION, SVCRT_AUDIT_INDEX_NONE);
    }
    if(t->pool_usable_size != (t->pool_size - (t->pool_reserve * t->pool_sector)))
    {
        audit_fail(p, SVCRT_AUDIT_PARTITION, SVCRT_AUDIT_INDEX_NONE);
    }
    if((audit_is_pow2(t->pool_alloc_unit) == 0u) || (t->pool_alloc_unit < 4u) ||
       (t->pool_alloc_unit > t->pool_sector))
    {
        audit_fail(p, SVCRT_AUDIT_PARTITION, SVCRT_AUDIT_INDEX_NONE);
    }

    /* Device config region: either absent (both fields 0) or a real range
     * that fits in Flash and does not sit on top of kernel or pool. */
    if((t->config_size != 0u) && (t->config_base == 0u))
    {
        audit_fail(p, SVCRT_AUDIT_PARTITION, SVCRT_AUDIT_INDEX_NONE);
    }
    if((t->config_base != 0u) &&
       ((t->config_size == 0u) || ((t->config_base + t->config_size) > flash_end)))
    {
        audit_fail(p, SVCRT_AUDIT_PARTITION, SVCRT_AUDIT_INDEX_NONE);
    }
    if((t->config_base != 0u) &&
       ((audit_overlap(t->config_base, t->config_size, t->kernel_base, t->kernel_size) != 0u) ||
        (audit_overlap(t->config_base, t->config_size, t->pool_base, t->pool_size) != 0u)))
    {
        audit_fail(p, SVCRT_AUDIT_PARTITION, SVCRT_AUDIT_INDEX_NONE);
    }

    /* RAM: three regions that must not touch, all inside the chip. */
    if((t->share_ram_size == 0u) || (t->share_ram_base < CHIP_RAM_BASE) ||
       ((t->share_ram_base + t->share_ram_size) > ram_end))
    {
        audit_fail(p, SVCRT_AUDIT_PARTITION, SVCRT_AUDIT_INDEX_NONE);
    }
    if((t->kernel_ram_size == 0u) || (t->kernel_ram_base < CHIP_RAM_BASE) ||
       ((t->kernel_ram_base + t->kernel_ram_size) > ram_end))
    {
        audit_fail(p, SVCRT_AUDIT_PARTITION, SVCRT_AUDIT_INDEX_NONE);
    }
    if((t->image_ram_total == 0u) || (t->image_ram_base < CHIP_RAM_BASE) ||
       ((t->image_ram_base + t->image_ram_total) > ram_end))
    {
        audit_fail(p, SVCRT_AUDIT_PARTITION, SVCRT_AUDIT_INDEX_NONE);
    }
    if((audit_overlap(t->share_ram_base, t->share_ram_size,
                      t->kernel_ram_base, t->kernel_ram_size) != 0u) ||
       (audit_overlap(t->share_ram_base, t->share_ram_size,
                      t->image_ram_base, t->image_ram_total) != 0u) ||
       (audit_overlap(t->kernel_ram_base, t->kernel_ram_size,
                      t->image_ram_base, t->image_ram_total) != 0u))
    {
        audit_fail(p, SVCRT_AUDIT_PARTITION, SVCRT_AUDIT_INDEX_NONE);
    }

    /* Image RAM pool shape: buddy allocation needs powers of two, and the
     * pool must be able to hold at least one smallest block. */
    if((audit_is_pow2(t->image_ram_min_block) == 0u) ||
       (audit_is_pow2(t->image_ram_max_block) == 0u) ||
       (t->image_ram_min_block > t->image_ram_max_block) ||
       ((t->image_ram_total % t->image_ram_min_block) != 0u) ||
       ((t->image_ram_base % t->image_ram_min_block) != 0u))
    {
        audit_fail(p, SVCRT_AUDIT_PARTITION, SVCRT_AUDIT_INDEX_NONE);
    }
}

/* ------------------------------------------------------------------ */
/* layout knobs                                                        */
/* ------------------------------------------------------------------ */

static void audit_layout(svcrt_audit_result_t *p)
{
    const svcrt_partition_table_t *t = svcrt_ptable_get();

    if((t == 0) || (t->magic != SVCRT_PARTITION_MAGIC))
    {
        return;     /* already reported by audit_partition() */
    }

    p->checks++;

    if(t->layout_mode > SVCRT_LAYOUT_MODE_FIXED)
    {
        audit_fail(p, SVCRT_AUDIT_LAYOUT, SVCRT_AUDIT_INDEX_NONE);
    }
    if(t->layout_source > SVCRT_LAYOUT_SOURCE_CONFIG)
    {
        audit_fail(p, SVCRT_AUDIT_LAYOUT, SVCRT_AUDIT_INDEX_NONE);
    }
    if(t->reclaim_mode > SVCRT_CFG_RECLAIM_NONE)
    {
        audit_fail(p, SVCRT_AUDIT_LAYOUT, SVCRT_AUDIT_INDEX_NONE);
    }
    if((t->slot_max == 0u) || (t->slot_max > SVCRT_SLOT_ARRAY_MAX))
    {
        audit_fail(p, SVCRT_AUDIT_LAYOUT, SVCRT_AUDIT_INDEX_NONE);
    }
    /* The fixed-slot table indexes slots by record, so it can never name
     * more entries than the table has. */
    if(t->cfg_slot_count > t->slot_max)
    {
        audit_fail(p, SVCRT_AUDIT_LAYOUT, SVCRT_AUDIT_INDEX_NONE);
    }
}

/* ------------------------------------------------------------------ */
/* slot records: fields, ranges, and pairwise overlap                  */
/* ------------------------------------------------------------------ */

static void audit_slots(svcrt_audit_result_t *p)
{
    const svcrt_partition_table_t *t = svcrt_ptable_get();
    uint32 i;
    uint32 j;

    if((t == 0) || (t->magic != SVCRT_PARTITION_MAGIC) || (t->slot_max > SVCRT_SLOT_ARRAY_MAX))
    {
        return;
    }

    for(i = 0u; i < t->slot_max; i++)
    {
        uint32 type  = t->slot_type[i];
        uint32 state = t->slot_state[i];

        p->checks++;

        if((type != SVCRT_SLOT_FREE) && (type != SVCRT_SLOT_APP) && (type != SVCRT_SLOT_DRIVER))
        {
            audit_fail(p, SVCRT_AUDIT_SLOTS, i);
            continue;
        }
        if((state != SVCRT_APP_SLOT_EMPTY) && (state != SVCRT_APP_SLOT_LOADED) &&
           (state != SVCRT_APP_SLOT_RUNNING) && (state != SVCRT_APP_SLOT_INVALID) &&
           (state != SVCRT_APP_SLOT_INSTALLING) && (state != SVCRT_APP_SLOT_RAW))
        {
            audit_fail(p, SVCRT_AUDIT_SLOTS, i);
            continue;
        }

        if(type == SVCRT_SLOT_FREE)
        {
            /* An unallocated record must carry no leftovers: a non-zero
             * field here means someone wrote without owning the slot. */
            if((t->slot_base[i] != 0u) || (t->slot_size[i] != 0u) ||
               (t->slot_ram_base[i] != 0u) || (t->slot_ram_size[i] != 0u) ||
               (t->slot_entry[i] != 0u) || (t->slot_task_id[i] != 0u) ||
               (t->slot_autostart[i] != 0u) || (state != SVCRT_APP_SLOT_EMPTY))
            {
                audit_fail(p, SVCRT_AUDIT_SLOTS, i);
            }
            continue;
        }

        /* Occupied record: the image must sit inside the allocatable part of
         * the pool, on an allocation boundary, and be big enough to hold a
         * header. */
        if(t->slot_size[i] < SVCRT_APP_HEADER_SIZE)
        {
            audit_fail(p, SVCRT_AUDIT_SLOTS, i);
        }
        if((t->slot_base[i] < t->pool_base) ||
           (((t->slot_base[i] - t->pool_base) % t->pool_alloc_unit) != 0u) ||
           (((t->slot_base[i] - t->pool_base) + t->slot_size[i]) > t->pool_usable_size))
        {
            audit_fail(p, SVCRT_AUDIT_SLOTS, i);
        }
        /* Running means the kernel was given an entry point to call. */
        if((state == SVCRT_APP_SLOT_RUNNING) && (t->slot_entry[i] == 0u))
        {
            audit_fail(p, SVCRT_AUDIT_SLOTS, i);
        }
        /* A slot that claims a task must name one that the table has. */
        if((t->slot_task_id[i] != 0u) && (t->slot_task_id[i] > (uint32)svcrt_task_count))
        {
            audit_fail(p, SVCRT_AUDIT_SLOTS, i);
        }

        /* RAM block shape. Both fields are 0 or both are set; a half-filled
         * pair is a torn update, not a state the allocator can produce. */
        if(((t->slot_ram_base[i] == 0u) && (t->slot_ram_size[i] != 0u)) ||
           ((t->slot_ram_base[i] != 0u) && (t->slot_ram_size[i] == 0u)))
        {
            audit_fail(p, SVCRT_AUDIT_SLOT_RAM, i);
        }
        else if(t->slot_ram_size[i] != 0u)
        {
            if((audit_is_pow2(t->slot_ram_size[i]) == 0u) ||
               (t->slot_ram_size[i] < t->image_ram_min_block) ||
               (t->slot_ram_size[i] > t->image_ram_max_block) ||
               ((t->slot_ram_base[i] % t->slot_ram_size[i]) != 0u) ||
               (t->slot_ram_base[i] < t->image_ram_base) ||
               ((t->slot_ram_base[i] + t->slot_ram_size[i]) >
                (t->image_ram_base + t->image_ram_total)))
            {
                audit_fail(p, SVCRT_AUDIT_SLOT_RAM, i);
            }
        }
    }

    /* Pairwise overlap. Two images in one Flash range or one RAM block would
     * each be silently corrupted by the other, so this is checked directly
     * rather than trusted to the allocator. */
    for(i = 0u; i < t->slot_max; i++)
    {
        if(t->slot_type[i] == SVCRT_SLOT_FREE)
        {
            continue;
        }
        for(j = (i + 1u); j < t->slot_max; j++)
        {
            if(t->slot_type[j] == SVCRT_SLOT_FREE)
            {
                continue;
            }
            p->checks++;
            if(audit_overlap(t->slot_base[i], t->slot_size[i],
                             t->slot_base[j], t->slot_size[j]) != 0u)
            {
                audit_fail(p, SVCRT_AUDIT_SLOT_FLASH, j);
            }
            if(audit_overlap(t->slot_ram_base[i], t->slot_ram_size[i],
                             t->slot_ram_base[j], t->slot_ram_size[j]) != 0u)
            {
                audit_fail(p, SVCRT_AUDIT_SLOT_RAM, j);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* task table                                                          */
/* ------------------------------------------------------------------ */

static void audit_tasks(svcrt_audit_result_t *p)
{
    int32 i;

    p->checks++;

    if((svcrt_task_count < 0) || (svcrt_task_count > SVCRT_TASK_MAX_NUM))
    {
        audit_fail(p, SVCRT_AUDIT_TASKS, SVCRT_AUDIT_INDEX_NONE);
        return;
    }

    for(i = 0; i < svcrt_task_count; i++)
    {
        const svcrt_task_t *q = &svcrt_task_table[i];

        p->checks++;

        /* Status must be one of the four the scheduler switches on. */
        if((q->status != SVCRT_TASK_INVALID) && (q->status != SVCRT_TASK_READY) &&
           (q->status != SVCRT_TASK_WAIT) && (q->status != SVCRT_TASK_RUNNING))
        {
            audit_fail(p, SVCRT_AUDIT_TASKS, (uint32)i);
            continue;
        }
        /* Priority indexes a two-level readiness bitmap: 8 groups of 32 bits,
         * so 256 levels exist and the task-create path rejects only >= 255.
         * (The first version of this check wrote 32 -- it flagged a healthy
         *  task whose priority happened to be above 31. A wrong bound here
         *  is worse than no check: it costs a round of hunting a non-bug.) */
        if(q->priority >= 255u)
        {
            audit_fail(p, SVCRT_AUDIT_TASKS, (uint32)i);
        }
        /* Every task sits on a stack the kernel handed out. */
        if((q->stack_bottom == 0) || (q->stack_size == 0u))
        {
            audit_fail(p, SVCRT_AUDIT_TASKS, (uint32)i);
        }
        /* Link indices are either NIL or a real index; one past the end
         * would send a later walk off the table. */
        if(((q->ready_next != SVCRT_TASK_NIL) && ((int32)q->ready_next >= svcrt_task_count)) ||
           ((q->ready_prev != SVCRT_TASK_NIL) && ((int32)q->ready_prev >= svcrt_task_count)) ||
           ((q->delay_next != SVCRT_TASK_NIL) && ((int32)q->delay_next >= svcrt_task_count)) ||
           ((q->delay_prev != SVCRT_TASK_NIL) && ((int32)q->delay_prev >= svcrt_task_count)))
        {
            audit_fail(p, SVCRT_AUDIT_TASKS, (uint32)i);
        }
    }

    /* The bitmap and the chains are a second copy of the task table's
     * readiness state; svcrt_sched_check() is the comparison. */
    p->checks++;
    if(svcrt_sched_check() != 0)
    {
        audit_fail(p, SVCRT_AUDIT_SCHED, SVCRT_AUDIT_INDEX_NONE);
    }
}

/* ------------------------------------------------------------------ */
/* public entry points                                                 */
/* ------------------------------------------------------------------ */

void svcrt_audit_run(svcrt_audit_result_t *p_out)
{
    svcrt_audit_result_t r;

    if(p_out == 0)
    {
        return;
    }

    r.mask   = 0u;
    r.index  = SVCRT_AUDIT_INDEX_NONE;
    r.checks = 0u;

    audit_partition(&r);
    audit_layout(&r);
    audit_slots(&r);
    audit_tasks(&r);

    g_audit_last = r;
    *p_out = r;
}

uint32 svcrt_audit_last_mask(void)
{
    return g_audit_last.mask;
}

uint32 svcrt_audit_boot(void)
{
    svcrt_audit_result_t r;

    svcrt_audit_run(&r);

    if(r.mask != 0u)
    {
        /* The report says which structure and which index; it does not say
         * why. The why is a job for the debugger, on that exact field. */
        SVCRT_LOGE("AUDIT", "self-check failed: mask 0x%08X, first index %u, checks %u",
                   (uint32)r.mask, (uint32)r.index, (uint32)r.checks);
        svcrt_fault_record(SVCRT_FAULT_AUDITFAIL, 0);
    }
    else
    {
        SVCRT_LOGI("AUDIT", "self-check clean (%u checks)", (uint32)r.checks);
    }

    return r.mask;
}

const char *svcrt_audit_name(uint32 bit)
{
    switch(bit)
    {
    case SVCRT_AUDIT_PARTITION:  return "partition";
    case SVCRT_AUDIT_LAYOUT:     return "layout";
    case SVCRT_AUDIT_SLOTS:      return "slots";
    case SVCRT_AUDIT_SLOT_RAM:   return "slot-ram";
    case SVCRT_AUDIT_SLOT_FLASH: return "slot-flash";
    case SVCRT_AUDIT_TASKS:      return "tasks";
    case SVCRT_AUDIT_SCHED:      return "sched";
    default:                     return 0;
    }
}
