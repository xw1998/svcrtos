/**
* @file svcrt_mpu.c
* @brief SVCrtOS memory protection manager (the kernel's only MPU entry point)
* @details Kernel code calls this file; MPU register access lives entirely in the
*          port layer (svcrt_port_mpu_*). Everything here is architecture
*          independent: this file decides *which* windows a task may touch, the
*          port decides *how* a window is encoded into hardware registers.
*
*          Fixed window addresses come from config/svcrt_partition.h only;
*          per-image windows come from the TCB region (rom_start/rom_size,
*          ram_start/ram_size) that the loader fills in from the runtime slot
*          table, so the project still has exactly one place where addresses
*          are defined. The peripheral policy comes from SVCRT_MPU_PERIPH*
*          (board setting).
*
* @note SVCRT_USE_MPU == 0 removes the whole body. On Cortex-M4 it defaults to
*       1 (SVCRT_ARCH_HAS_MPU), so every window built here must satisfy the
*       hardware rule "power of two in size, aligned to that size" - that rule
*       is enforced at compile time for the fixed windows, and for every image
*       window by svcrt_mpu_rom_window() / svcrt_mpu_ram_window(), which either
*       produce an exact region or the smallest enclosing aligned one.
*
* @author xw
* @date 2026.09.12
*/

#include "svcrt_mpu.h"

#if (SVCRT_USE_MPU == 1)

#include "svcrt_partition.h"    /* kernel only: single source of addresses */

/* ---------------------------------------------------------------- helpers */

static void svcrt_mpu_clear(svcrt_arch_mpu_t *p_mpu)
{
    uint32 i;

    for(i = 0u; i < SVCRT_MPU_REGION_MAX; i++)
    {
        p_mpu->region_base[i] = 0u;
        p_mpu->region_attr[i] = 0u;
    }
}

/* Round n up to the next power of two (an MPU region is a power of two). */
static uint32 svcrt_mpu_round_up(uint32 n)
{
    uint32 v = 32u;

    while((v < n) && (v < 0x80000000u))
    {
        v <<= 1;
    }

    return v;
}

/* Ask the port to encode one region; bad parameters are simply ignored
 * (a missing optional window must not break the context). */
static void svcrt_mpu_add_region(svcrt_arch_mpu_t *p_mpu, uint32 idx,
                                 uint32 base, uint32 size, svcrt_mpu_mem_t mem)
{
    if((size == 0u) || (idx >= SVCRT_MPU_REGION_MAX))
    {
        return;
    }

    (void)svcrt_port_mpu_encode(p_mpu, idx, base, size, mem);
}

/* Flash window a task may execute from.
 * External images (App / Driver) live in the unified image pool, so the image
 * no longer sits at a link-time address: the loader records the landing point
 * and the occupied span in the TCB. Those two numbers do not necessarily form
 * an MPU region, because placement prefers a 2^n-aligned address but falls
 * back to a tightly packed one when an aligned slot does not fit.
 *
 * The window is therefore the **smallest aligned power-of-two region that
 * contains the image**:
 *   - aligned landing point (= the normal case)  -> base == rom_start,
 *     size == round_up_pow2(span): an exact window, isolation is as strict as
 *     a fixed-slot layout used to be;
 *   - tight landing point (degraded case)        -> the window reaches below
 *     the image start, so the immediately preceding image becomes readable.
 *     The over-reach is bounded by the window size and never crosses the
 *     pool: neighbouring code can be read, nothing outside the pool can.
 *
 * Kernel internal tasks carry rom_start == 0 and fall back to kernel flash. */
/** Is this task's code window in RAM instead of the flash image pool?
 *  True for MiniApps only (they are loaded into RAM and run there, see
 *  svcrt_mini.c); every App / Driver / kernel task answers no. The test is on
 *  the recorded window, not on a flag: a window in the image RAM pool can only
 *  have been put there by the MiniApp loader. */
static uint8 svcrt_mpu_code_in_ram(const svcrt_task_t *p_task)
{
    uint32 start = p_task->rom_start;

    return ((start >= SVCRT_MPU_SLOT_RAM_BASE) &&
            (start <  (SVCRT_MPU_SLOT_RAM_BASE + SVCRT_MPU_SLOT_RAM_TOTAL))) ? 1u : 0u;
}

static void svcrt_mpu_rom_window(const svcrt_task_t *p_task,
                                 uint32 *p_base, uint32 *p_size)
{
    uint32 start = p_task->rom_start;
    uint32 span  = p_task->rom_size;

    /* MiniApp: code lives in RAM. The block is a buddy block, so it is already
     * a power of two aligned to its own size and the window comes out exact.
     * An inexact window is REFUSED (size 0 = region left disabled) rather than
     * rounded outwards: an enclosing power-of-two region would reach into a
     * neighbour's RAM and hand this task an executable view of it. Failing
     * closed costs a fault in a case that cannot happen; failing open would
     * break isolation silently. */
    if(svcrt_mpu_code_in_ram(p_task) != 0u)
    {
        if((span != 0u) &&
           ((span & (span - 1u)) == 0u) &&
           ((start & (span - 1u)) == 0u) &&
           ((start + span) <= (SVCRT_MPU_SLOT_RAM_BASE + SVCRT_MPU_SLOT_RAM_TOTAL)))
        {
            *p_base = start;
            *p_size = span;
            return;
        }

        *p_base = 0u;
        *p_size = 0u;
        return;
    }

    if((start >= SVCRT_MPU_IMAGE_POOL_BASE) &&
       (start <  (SVCRT_MPU_IMAGE_POOL_BASE + SVCRT_MPU_IMAGE_POOL_SIZE)) &&
       (span != 0u) &&
       ((start + span) <= (SVCRT_MPU_IMAGE_POOL_BASE + SVCRT_MPU_IMAGE_POOL_SIZE)))
    {
        uint32 size = svcrt_mpu_round_up(span);
        uint32 base = start & ~(size - 1u);

        /* Grow until the window also covers the tail of the image. */
        while((base + size) < (start + span))
        {
            size <<= 1;
            base = start & ~(size - 1u);
        }

        *p_base = base;
        *p_size = size;
        return;
    }

    *p_base = SVCRT_MPU_KERNEL_ROM_BASE;
    *p_size = SVCRT_MPU_KERNEL_ROM_SIZE;
}

/* RAM window a task may use, taken from the TCB region the loader recorded.
 * The window is no longer derived from a slot number: the image RAM pool is a
 * buddy allocator, so the block that an image owns is already a power of two
 * aligned to its own size - exactly what one MPU region needs. Two images can
 * therefore never read each other's stack or data, whatever sizes they ask
 * for.
 * Kernel internal tasks fall back to the whole chip RAM window: KERNEL_RAM_SIZE
 * absorbs the remainder and is not a power of two, so it cannot be an MPU
 * region. Kernel tasks are trusted code, the restriction only matters for
 * App / Driver tasks. */
static void svcrt_mpu_ram_window(const svcrt_task_t *p_task,
                                 uint32 *p_base, uint32 *p_size)
{
    uint32 addr = p_task->ram_start;
    uint32 size = p_task->ram_size;

    if((addr >= SVCRT_MPU_SLOT_RAM_BASE) &&
       (addr <  (SVCRT_MPU_SLOT_RAM_BASE + SVCRT_MPU_SLOT_RAM_TOTAL)) &&
       (size >= SVCRT_MPU_RAM_BLOCK_MIN) &&
       (size <= SVCRT_MPU_RAM_BLOCK_MAX) &&
       ((size & (size - 1u)) == 0u) &&
       ((addr & (size - 1u)) == 0u) &&
       ((addr + size) <= (SVCRT_MPU_SLOT_RAM_BASE + SVCRT_MPU_SLOT_RAM_TOTAL)))
    {
        *p_base = addr;
        *p_size = size;
        return;
    }

    /* A window in the pool that does not qualify is refused (size 0 = region
     * left disabled, the task faults instead of running with a wrong window).
     * Only a task that is NOT an image gets the whole-chip fallback: handing a
     * MiniApp whose block has gone bad the kernel RAM window would let it read
     * and write every other task's stack, and nothing would look wrong. */
    if((addr >= SVCRT_MPU_SLOT_RAM_BASE) &&
       (addr <  (SVCRT_MPU_SLOT_RAM_BASE + SVCRT_MPU_SLOT_RAM_TOTAL)))
    {
        *p_base = 0u;
        *p_size = 0u;
        return;
    }

    *p_base = SVCRT_MPU_KERNEL_RAM_FALLBACK;
    *p_size = svcrt_mpu_round_up(CHIP_RAM_SIZE);
}

/* ------------------------------------------------------------ public API */

void svcrt_mpu_module_init(void)
{
    svcrt_port_mpu_init();
}

/* Build the complete region set for one task. Call it after the TCB windows
 * are final: svcrt_task_register() covers kernel tasks, the loader calls it
 * again once it has rewritten rom_start / ram_size for App and Driver tasks. */
void svcrt_mpu_build_task(svcrt_task_t *p_task)
{
    svcrt_arch_mpu_t *p_mpu;
    uint32 rom_base = 0u;
    uint32 rom_size = 0u;
    uint32 ram_base = 0u;
    uint32 ram_size = 0u;
    uint8  code_in_ram;

    if(p_task == 0)
    {
        return;
    }

    code_in_ram = svcrt_mpu_code_in_ram(p_task);

    p_mpu = &p_task->mpu;
    svcrt_mpu_clear(p_mpu);

    /* Region 0: code. rom_start == 0 marks a kernel internal task, which runs
     * kernel code, so it gets the kernel flash window. A MiniApp executes
     * from its RAM block, which therefore needs an executable attribute
     * (SVCRT_MPU_MEM_RAMX) instead of the read-only flash one. */
    svcrt_mpu_rom_window(p_task, &rom_base, &rom_size);
    svcrt_mpu_add_region(p_mpu, SVCRT_MPU_RGN_CODE, rom_base, rom_size,
                         (code_in_ram != 0u) ? SVCRT_MPU_MEM_RAMX
                                             : SVCRT_MPU_MEM_ROM);

    /* Region 1: data / stack. A MiniApp has two separate blocks (code block in
     * region 0, RAM block here), so this window is a plain non-executable data
     * window - the normal case, same as an App whose RAM lives in the pool.
     * The only exception is a task whose RAM window IS its code window (same
     * base and size): region 1 outranks region 0 on overlap, so it must then
     * carry the same executable attribute, otherwise the task would fault on
     * its own first instruction fetch. Keep that case working even though no
     * current loader produces it. */
    svcrt_mpu_ram_window(p_task, &ram_base, &ram_size);
    svcrt_mpu_add_region(p_mpu, SVCRT_MPU_RGN_DATA, ram_base, ram_size,
                         ((code_in_ram != 0u) && (ram_base == rom_base) &&
                          (ram_size == rom_size)) ? SVCRT_MPU_MEM_RAMX
                                                  : SVCRT_MPU_MEM_RAM);

    /* Region 2: shared RAM, where the partition table lives. Every external
     * task reads it at boot. Skipped when the RAM window already covers it. */
    if((SVCRT_MPU_SHARE_RAM_BASE < ram_base) ||
       (SVCRT_MPU_SHARE_RAM_BASE >= (ram_base + ram_size)))
    {
        svcrt_mpu_add_region(p_mpu, SVCRT_MPU_RGN_SHARE,
                             SVCRT_MPU_SHARE_RAM_BASE, SVCRT_MPU_SHARE_RAM_SIZE,
                             SVCRT_MPU_MEM_RAM);
    }

    /* Regions 3/4: peripheral windows from the board policy. */
    #if (SVCRT_MPU_PERIPH_SIZE != 0)
    svcrt_mpu_add_region(p_mpu, SVCRT_MPU_RGN_PERIPH1,
                         SVCRT_MPU_PERIPH_BASE, SVCRT_MPU_PERIPH_SIZE,
                         (SVCRT_MPU_PERIPH_RW == 1) ? SVCRT_MPU_MEM_PERIPH_RW
                                                    : SVCRT_MPU_MEM_PERIPH_RO);
    #endif

    #if (SVCRT_MPU_PERIPH2_SIZE != 0)
    svcrt_mpu_add_region(p_mpu, SVCRT_MPU_RGN_PERIPH2,
                         SVCRT_MPU_PERIPH2_BASE, SVCRT_MPU_PERIPH2_SIZE,
                         (SVCRT_MPU_PERIPH_RW == 1) ? SVCRT_MPU_MEM_PERIPH_RW
                                                    : SVCRT_MPU_MEM_PERIPH_RO);
    #endif
}

void svcrt_mpu_set_app(svcrt_task_t *p_app)
{
    if(p_app == 0)
    {
        /* Switching to the idle task: the kernel calls svcrt_mpu_set_idle()
         * instead, which re-applies the context captured at startup. */
        return;
    }

    svcrt_port_mpu_set_app(&p_app->mpu);
}

void svcrt_mpu_set_idle(void)
{
    svcrt_port_mpu_set_idle();
}

void svcrt_mpu_reset(void)
{
    svcrt_port_mpu_reset();
}

#endif
