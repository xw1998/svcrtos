/**
* @file svcrt_mpu.c
* @brief SVCrtOS memory protection manager (the kernel's only MPU entry point)
* @details Kernel code calls this file; MPU register access lives entirely in the
*          port layer (svcrt_port_mpu_*). Everything here is architecture
*          independent: this file decides *which* windows a task may touch, the
*          port decides *how* a window is encoded into hardware registers.
*
*          Window addresses come from config/svcrt_partition.h only, so the
*          project still has exactly one place where addresses are defined.
*          The peripheral policy comes from SVCRT_MPU_PERIPH* (board setting).
*
* @note SVCRT_USE_MPU == 0 (the current board default) removes the whole body.
*       The isolation path is compiled and syntax checked with MPU forced on,
*       but it has NOT been validated on hardware. Before flipping
*       SVCRT_USE_MPU to 1, walk the on-board checklist in
*       docs/死代码与未接线审计.md (MemManage trigger points, window edges,
*       peripheral range, and the App/Driver slot isolation).
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

/* Flash window that contains addr. App slots are isolated from each other,
 * so exactly one slot window is granted. Returns 0 on success. */
static int32 svcrt_mpu_rom_window(uint32 addr, uint32 *p_base, uint32 *p_size)
{
    if((addr >= SVCRT_MPU_KERNEL_ROM_BASE) &&
       (addr < (SVCRT_MPU_KERNEL_ROM_BASE + SVCRT_MPU_KERNEL_ROM_SIZE)))
    {
        *p_base = SVCRT_MPU_KERNEL_ROM_BASE;
        *p_size = SVCRT_MPU_KERNEL_ROM_SIZE;
        return 0;
    }

    if((addr >= SVCRT_MPU_DRIVER_ROM_BASE) &&
       (addr < (SVCRT_MPU_DRIVER_ROM_BASE + SVCRT_MPU_DRIVER_ROM_SIZE)))
    {
        *p_base = SVCRT_MPU_DRIVER_ROM_BASE;
        *p_size = SVCRT_MPU_DRIVER_ROM_SIZE;
        return 0;
    }

    if((addr >= SVCRT_MPU_APP_ROM_BASE) &&
       (addr < (SVCRT_MPU_APP_ROM_BASE + SVCRT_MPU_APP_ROM_SIZE)))
    {
        uint32 slot = (addr - SVCRT_MPU_APP_ROM_BASE) / APP_SLOT_SIZE;

        *p_base = SVCRT_MPU_APP_ROM_BASE + (slot * APP_SLOT_SIZE);
        *p_size = APP_SLOT_SIZE;
        return 0;
    }

    return -1;
}

/* RAM window a task may use, derived from the partition its stack lives in.
 * Kernel internal tasks fall back to the whole chip RAM window: KERNEL_RAM_SIZE
 * absorbs the remainder and is not a power of two, so it cannot be an MPU
 * region. Kernel tasks are trusted code, the restriction only matters for
 * App / Driver tasks. */
static void svcrt_mpu_ram_window(uint32 addr, uint32 *p_base, uint32 *p_size)
{
    if((addr >= SVCRT_MPU_APP_RAM_BASE) &&
       (addr < (SVCRT_MPU_APP_RAM_BASE + SVCRT_MPU_APP_RAM_SIZE)))
    {
        *p_base = SVCRT_MPU_APP_RAM_BASE;
        *p_size = SVCRT_MPU_APP_RAM_SIZE;
        return;
    }

    if((addr >= SVCRT_MPU_DRIVER_RAM_BASE) &&
       (addr < (SVCRT_MPU_DRIVER_RAM_BASE + SVCRT_MPU_DRIVER_RAM_SIZE)))
    {
        *p_base = SVCRT_MPU_DRIVER_RAM_BASE;
        *p_size = SVCRT_MPU_DRIVER_RAM_SIZE;
        return;
    }

    *p_base = CHIP_RAM_BASE;
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

    if(p_task == 0)
    {
        return;
    }

    p_mpu = &p_task->mpu;
    svcrt_mpu_clear(p_mpu);

    /* Region 0: code. rom_start == 0 marks a kernel internal task, which runs
     * kernel code, so it gets the kernel flash window. */
    if(svcrt_mpu_rom_window(p_task->rom_start, &rom_base, &rom_size) != 0)
    {
        rom_base = SVCRT_MPU_KERNEL_ROM_BASE;
        rom_size = SVCRT_MPU_KERNEL_ROM_SIZE;
    }
    svcrt_mpu_add_region(p_mpu, SVCRT_MPU_RGN_CODE, rom_base, rom_size, SVCRT_MPU_MEM_ROM);

    /* Region 1: data / stack. */
    svcrt_mpu_ram_window(p_task->ram_start, &ram_base, &ram_size);
    svcrt_mpu_add_region(p_mpu, SVCRT_MPU_RGN_DATA, ram_base, ram_size, SVCRT_MPU_MEM_RAM);

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
