# -*- coding: utf-8 -*-
"""
Close the MPU isolation gap (round 8).

What was missing (see docs/死代码与未接线审计.md section 3.1):
  1. Nobody filled TCB.mpu -> svcrt_task_register() zeroed it, so every region
     was disabled and an unprivileged task would fault on its first access.
  2. The data region source was wrong: TCB.ram_start/ram_size is the task stack,
     not a power-of-two aligned window. The right source is the partition layout.
  3. No policy for the peripheral area -> unprivileged tasks could not touch
     LED / UART registers at all.
  4. (on-board calibration - cannot be done statically; see docs)

Also fixes a real regression introduced by commit 9453526:
  svcrt_port_mpu_set_region() changed the data region AP field from 0b011 to
  0b010, claiming 0b011 was "unprivileged read-only". Per the ARMv7-M reference
  manual (and CMSIS ARM_MPU_AP_FULL=3 / ARM_MPU_AP_URO=2) it is the other way
  round: 0b011 is full read/write, 0b010 is privileged read/write +
  unprivileged read-only. With 0b010 an unprivileged task faults on its first
  store to its own RAM. Restored to 0b011.

Design (kernel stays architecture independent):
  - svcrt_mpu.c decides *which* windows a task may touch.
  - the port layer decides *how* a window becomes hardware bits
    (new svcrt_port_mpu_encode()).
  - addresses still come from config/svcrt_partition.h only; the peripheral
    policy is a board setting (SVCRT_MPU_PERIPH* in svcrt_config.h, overridden
    by board/stm32f427/svcrt_board_config.h).

All new comments are ASCII / English so that patching GBK and UTF-8 files is
byte-identical work.

Usage:  python patch/p_fix12_mpu_isolation.py
"""
import os
import shutil

ROOT = r'D:\工作\git_project\svcrtos_new'
BK = r'C:\Users\14905\OneDrive\Documents\lingxi-claw\20260911-13-47-19-445\backup_fix12'

NEW_MPU_H = '''/**
* @brief SVCrtOS memory protection manager (kernel internal interface)
* @details Kernel code calls only this layer; all MPU register access lives in
*          the port layer (svcrt_port_mpu_*). Porting to a new architecture
*          therefore only needs a new port implementation.
*          The whole interface collapses to empty macros when SVCRT_USE_MPU == 0.
*/

#ifndef __SVCRT_MPU_H__
#define __SVCRT_MPU_H__

#include "svcrt_def.h"
#include "svcrt_hal.h"
#include "svcrt_task.h"
#include "svcrt_config.h"

/* Region index layout used by every task context built in svcrt_mpu.c. */
#define SVCRT_MPU_RGN_CODE       (0u)   /* task code window (read-only, executable) */
#define SVCRT_MPU_RGN_DATA       (1u)   /* task data/stack window (read/write, XN) */
#define SVCRT_MPU_RGN_SHARE      (2u)   /* kernel shared RAM (partition table) */
#define SVCRT_MPU_RGN_PERIPH1    (3u)   /* peripheral window 1 (board configured) */
#define SVCRT_MPU_RGN_PERIPH2    (4u)   /* peripheral window 2 (board configured) */

#if (SVCRT_USE_MPU == 1)

void svcrt_mpu_module_init(void);
void svcrt_mpu_set_app(svcrt_task_t *p_app);
void svcrt_mpu_set_idle(void);
void svcrt_mpu_build_task(svcrt_task_t *p_task);
void svcrt_mpu_reset(void);

#else

#define svcrt_mpu_module_init()
#define svcrt_mpu_set_app(p_app)
#define svcrt_mpu_set_idle()
#define svcrt_mpu_build_task(p_task)
#define svcrt_mpu_reset()

#endif

#endif
'''

NEW_MPU_C = '''/**
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
'''

PORT_MPU_SECTION = '''#if (SVCRT_USE_MPU == 1)

/* Idle-task MPU context, captured by svcrt_port_mpu_set_region() at startup.
 * A task switch rewrites every region register, so the kernel re-applies this
 * context on each switch back to the idle task (svcrt_mpu_set_idle()). */
static svcrt_arch_mpu_t svcrt_mpu_idle_ctx;

void svcrt_port_mpu_init(void)
{
    int32 i;

    if(MPU->TYPE == 0)
    {
        return;
    }

    SVCRT_DMB();
    MPU->CTRL = 0;

    for(i = 0; i < 8; i++)
    {
        MPU->RNR = i;
        MPU->RBAR = 0;
        MPU->RASR = 0;
    }

    MPU->CTRL = MPU_CTRL_ENABLE_Msk | MPU_CTRL_PRIVDEFENA_Msk;
    SVCRT_DSB();
    SVCRT_ISB();
}

/* Smallest MPU SIZE field (RASR bits[5:1]) that covers `size` bytes.
 * An MPU region is a power of two and at least 32 bytes, so SIZE = log2(size) - 1.
 * The previous version searched with
 *     for(reg_idx = 4; reg_idx < 32; reg_idx++)
 *         if((1 << (reg_idx + 1)) >= size)
 * which reaches reg_idx == 31 when size > 2^31 and evaluates `1 << 32`, i.e.
 * undefined behaviour on a 32-bit int. This version clamps the input to 2GB,
 * uses unsigned shifts only, and keeps the shift count <= 31. */
static uint32 svcrt_mpu_size_field(uint32 size)
{
    uint32 field = 4u;                  /* smallest region: 32 bytes */

    if(size > 0x80000000u)
    {
        size = 0x80000000u;
    }

    while((field < 30u) && ((1u << (field + 1u)) < size))
    {
        field++;
    }

    return field;
}

/* Encode one region into the architecture independent context.
 * Returns 0 on success, -1 on a bad index or unknown attribute kind. */
int32 svcrt_port_mpu_encode(svcrt_arch_mpu_t *p_mpu, uint32 idx, uint32 base, uint32 size,
                            svcrt_mpu_mem_t mem)
{
    uint32 attr;
    uint32 region;

    if((p_mpu == 0) || (idx >= SVCRT_MPU_REGION_MAX) || (size == 0u))
    {
        return -1;
    }

    region = svcrt_mpu_size_field(size);

    /* A region base must be aligned to the region size: round down. */
    base = base & ~((1u << (1u + region)) - 1u);

    switch(mem)
    {
        case SVCRT_MPU_MEM_ROM:
            attr = 0x06020001u;     /* AP=0b110: read-only for both, XN=0 */
            break;

        case SVCRT_MPU_MEM_RAM:
            attr = 0x13060001u;     /* AP=0b011: read-write, XN=1 */
            break;

        case SVCRT_MPU_MEM_PERIPH_RO:
            attr = 0x12060001u;     /* AP=0b010: unprivileged read-only, XN=1 */
            break;

        case SVCRT_MPU_MEM_PERIPH_RW:
            attr = 0x13060001u;     /* AP=0b011: unprivileged read-write, XN=1 */
            break;

        default:
            return -1;
    }

    p_mpu->region_base[idx] = base;
    p_mpu->region_attr[idx] = attr | (region << 1);
    return 0;
}

/* Startup path, called by the board through svcrt_port_set_idle_mpu():
 * capture the idle task windows (code + stack) and apply them. */
void svcrt_port_mpu_set_region(uint32 rom_addr, uint32 rom_size, uint32 ram_addr, uint32 ram_size)
{
    /* AP encoding on ARMv7-M (see CMSIS ARM_MPU_AP_FULL=3, ARM_MPU_AP_URO=2):
     * 0b011 = privileged and unprivileged read-write,
     * 0b010 = privileged read-write, unprivileged read-only,
     * 0b110 = read-only for both.
     * The data window must be writable by the task, so AP=0b011 with XN=1.
     * An earlier change had flipped it to 0b010 thinking 0b011 was read-only;
     * that would fault on an unprivileged task's first store to its own RAM. */
    if(MPU->TYPE == 0)
    {
        return;
    }

    (void)svcrt_port_mpu_encode(&svcrt_mpu_idle_ctx, 0u, rom_addr, rom_size, SVCRT_MPU_MEM_ROM);
    (void)svcrt_port_mpu_encode(&svcrt_mpu_idle_ctx, 1u, ram_addr, ram_size, SVCRT_MPU_MEM_RAM);
    svcrt_port_mpu_set_app(&svcrt_mpu_idle_ctx);
}

/* Re-apply the captured idle context (called on every switch to idle). */
void svcrt_port_mpu_set_idle(void)
{
    svcrt_port_mpu_set_app(&svcrt_mpu_idle_ctx);
}

void svcrt_port_mpu_set_app(const svcrt_arch_mpu_t *p_mpu)
{
    int32 rnr = 0;

    SVCRT_DMB();
    MPU->CTRL = 0;

    /* svcrt_port_mpu_init()/reset() clear regions 0~7, so writing only the
     * first few would leave stale windows from the previous task in 4~7.
     * Clear them here as well. */
    for(rnr = 0; rnr < 8; rnr++)
    {
        MPU->RNR = rnr;

        if(rnr < (int32)SVCRT_MPU_REGION_MAX)
        {
            MPU->RBAR = p_mpu->region_base[rnr];
            MPU->RASR = p_mpu->region_attr[rnr];
        }
        else
        {
            MPU->RBAR = 0;
            MPU->RASR = 0;
        }
    }

    MPU->CTRL = MPU_CTRL_ENABLE_Msk | MPU_CTRL_PRIVDEFENA_Msk;
    SVCRT_DSB();
    SVCRT_ISB();
}

void svcrt_port_mpu_reset(void)
{
    int32 rnr = 0;

    SVCRT_DMB();
    MPU->CTRL = 0;

    for(rnr = 0; rnr < 8; rnr++)
    {
        MPU->RNR  = rnr;
        MPU->RASR = 0;
    }

    MPU->CTRL = MPU_CTRL_ENABLE_Msk | MPU_CTRL_PRIVDEFENA_Msk;
    SVCRT_DSB();
    SVCRT_ISB();
}

'''


def detect_enc(raw):
    try:
        raw.decode('utf-8')
        return 'utf-8'
    except UnicodeDecodeError:
        return 'gbk'


def backup(rel):
    p = os.path.join(ROOT, rel.replace('/', os.sep))
    shutil.copy2(p, os.path.join(BK, rel.replace('/', '__')))


def load(rel):
    p = os.path.join(ROOT, rel.replace('/', os.sep))
    raw = open(p, 'rb').read()
    enc = detect_enc(raw)
    # keep a BOM-free decode; utf-8-sig would change byte layout on write
    return p, raw.decode(enc), enc


def save(p, text, enc):
    open(p, 'wb').write(text.encode(enc))


def replace(rel, pairs, required=True):
    p, text, enc = load(rel)
    backup(rel)
    for old, new in pairs:
        old_e = old
        if old_e not in text:
            # tolerate the other line ending
            alt = old.replace('\n', '\r\n')
            if alt in text:
                old_e, new = alt, new.replace('\n', '\r\n')
            elif required:
                raise AssertionError('%s: anchor not found:\n%r' % (rel, old[:120]))
            else:
                print('  [skip] %s: %r' % (rel, old[:60]))
                continue
        assert text.count(old_e) == 1, '%s: anchor not unique (%d)' % (rel, text.count(old_e))
        text = text.replace(old_e, new)
    save(p, text, enc)
    print('  [ok] %s' % rel)


def write_file(rel, content):
    p = os.path.join(ROOT, rel.replace('/', os.sep))
    if os.path.exists(p):
        backup(rel)
    enc = 'gbk' if rel.endswith('.h') and 'mpu' in rel else 'utf-8'
    content.encode(enc)          # fail early if not representable
    open(p, 'wb').write(content.encode(enc))
    print('  [ok] %s (rewritten, %s)' % (rel, enc))


# --------------------------------------------------------------------- main

def main():
    os.makedirs(BK, exist_ok=True)

    # 1) partition.h: derived MPU windows + compile-time power-of-two checks
    replace('config/svcrt_partition.h', [(
        '#define SVCRT_HW_COMPAT_ID   (0x42700001u)   /* 0x4270 = STM32F427，0x0001 = ABI v1 */',
        '''#define SVCRT_HW_COMPAT_ID   (0x42700001u)   /* 0x4270 = STM32F427, 0x0001 = ABI v1 */

/* ============================================================
 * 八、MPU isolation windows (derived; do not edit by hand)
 * @details svcrt_mpu.c builds every task's MPU context from these windows
 *          only, so the project still has exactly one place where addresses
 *          are defined.
 *          An MPU region must be a power of two in size and aligned to that
 *          size; the compile-time check below enforces both for all windows.
 * ============================================================ */
#define SVCRT_MPU_KERNEL_ROM_BASE   (KERNEL_BASE)
#define SVCRT_MPU_KERNEL_ROM_SIZE   (KERNEL_SIZE)
#define SVCRT_MPU_DRIVER_ROM_BASE   (DRIVER_POOL_BASE)
#define SVCRT_MPU_DRIVER_ROM_SIZE   (DRIVER_POOL_SIZE)
#define SVCRT_MPU_APP_ROM_BASE      (APP_USER_BASE)
#define SVCRT_MPU_APP_ROM_SIZE      (APP_USER_SIZE)
#define SVCRT_MPU_DRIVER_RAM_BASE   (DRIVER_RAM_BASE)
#define SVCRT_MPU_DRIVER_RAM_SIZE   (DRIVER_RAM_SIZE)
#define SVCRT_MPU_APP_RAM_BASE      (APP_RAM_BASE)
#define SVCRT_MPU_APP_RAM_SIZE      (APP_RAM_SIZE)
#define SVCRT_MPU_SHARE_RAM_BASE    (SHARE_RAM_BASE)
#define SVCRT_MPU_SHARE_RAM_SIZE    (SHARE_RAM_SIZE)

/* Power-of-two and alignment self-check: the array size is the product of the
 * individual results, so a single failure makes it negative and the compiler
 * refuses the file. */
#define SVCRT_IS_POW2(x)            (((x) != 0) && (((x) & ((x) - 1)) == 0))
typedef char svcrt_mpu_window_check[
      (SVCRT_IS_POW2(KERNEL_SIZE)                ? 1 : -1)
    * (SVCRT_IS_POW2(DRIVER_POOL_SIZE)           ? 1 : -1)
    * (SVCRT_IS_POW2(APP_SLOT_SIZE)              ? 1 : -1)
    * (SVCRT_IS_POW2(APP_RAM_SIZE)               ? 1 : -1)
    * (SVCRT_IS_POW2(DRIVER_RAM_SIZE)            ? 1 : -1)
    * (SVCRT_IS_POW2(SHARE_RAM_SIZE)             ? 1 : -1)
    * (((KERNEL_BASE      % KERNEL_SIZE)      == 0) ? 1 : -1)
    * (((DRIVER_POOL_BASE % DRIVER_POOL_SIZE) == 0) ? 1 : -1)
    * (((APP_USER_BASE    % APP_SLOT_SIZE)    == 0) ? 1 : -1)
    * (((APP_RAM_BASE     % APP_RAM_SIZE)     == 0) ? 1 : -1)
    * (((DRIVER_RAM_BASE  % DRIVER_RAM_SIZE)  == 0) ? 1 : -1)
    * (((SHARE_RAM_BASE   % SHARE_RAM_SIZE)   == 0) ? 1 : -1)
    * 1];''')])

    # 2) kernel default peripheral policy (0 = window disabled)
    replace('kernelsrc/include/svcrt_config.h', [(
        '''/* ============================================================
 * 任务与调度相关参数
 * ============================================================ */''',
        '''/* ============================================================
 * MPU peripheral windows (only used when SVCRT_USE_MPU == 1)
 * @details Unprivileged tasks may reach peripheral registers only through
 *          these two windows. 0 = window disabled by default; the real
 *          ranges are chip characteristics and come from the board config,
 *          the kernel does not know any peripheral address.
 *          SVCRT_MPU_PERIPH_RW: 1 = unprivileged read/write, 0 = readonly.
 * ============================================================ */
#ifndef SVCRT_MPU_PERIPH_BASE
#define SVCRT_MPU_PERIPH_BASE     (0u)
#endif
#ifndef SVCRT_MPU_PERIPH_SIZE
#define SVCRT_MPU_PERIPH_SIZE     (0u)
#endif
#ifndef SVCRT_MPU_PERIPH2_BASE
#define SVCRT_MPU_PERIPH2_BASE    (0u)
#endif
#ifndef SVCRT_MPU_PERIPH2_SIZE
#define SVCRT_MPU_PERIPH2_SIZE    (0u)
#endif
#ifndef SVCRT_MPU_PERIPH_RW
#define SVCRT_MPU_PERIPH_RW       (0)
#endif

/* ============================================================
 * 任务与调度相关参数
 * ============================================================ */''')])

    # 3) board peripheral policy
    replace('board/stm32f427/svcrt_board_config.h', [(
        '#define APP_ALLOW_RAW_IMAGE       1',
        '''#define APP_ALLOW_RAW_IMAGE       1

/* MPU peripheral windows (only used when SVCRT_USE_MPU == 1).
 * STM32F427: APB1/APB2/AHB1 live in 0x40000000..0x4007FFFF,
 *            AHB2 (USB OTG FS/HS) lives in 0x50000000..0x5007FFFF.
 * Unprivileged App/Driver tasks reach peripherals only through these
 * windows; the sizes are powers of two so they can be MPU regions. */
#define SVCRT_MPU_PERIPH_BASE      (0x40000000u)
#define SVCRT_MPU_PERIPH_SIZE      (512u * 1024u)
#define SVCRT_MPU_PERIPH2_BASE     (0x50000000u)
#define SVCRT_MPU_PERIPH2_SIZE     (512u * 1024u)
#define SVCRT_MPU_PERIPH_RW        (1)''')])

    # 4) arch: memory attribute kinds
    replace('kernelsrc/include/svcrt_arch.h', [(
        '''typedef struct {
    uint32 region_base[SVCRT_MPU_REGION_MAX];
    uint32 region_attr[SVCRT_MPU_REGION_MAX];
} svcrt_arch_mpu_t;''',
        '''typedef struct {
    uint32 region_base[SVCRT_MPU_REGION_MAX];
    uint32 region_attr[SVCRT_MPU_REGION_MAX];
} svcrt_arch_mpu_t;

/* Memory attribute kind of one region (architecture independent).
 * svcrt_mpu.c passes one of these to svcrt_port_mpu_encode(), which maps it
 * to the local encoding. */
typedef enum {
    SVCRT_MPU_MEM_NONE = 0,     /* region disabled */
    SVCRT_MPU_MEM_ROM,          /* read-only, executable (task code) */
    SVCRT_MPU_MEM_RAM,          /* read/write, non-executable (data / stack) */
    SVCRT_MPU_MEM_PERIPH_RO,    /* peripheral, unprivileged read-only */
    SVCRT_MPU_MEM_PERIPH_RW     /* peripheral, unprivileged read/write */
} svcrt_mpu_mem_t;''')])

    # 5) hal: new port entry points
    replace('kernelsrc/include/svcrt_hal.h', [(
        '''void svcrt_port_mpu_init(void);
void svcrt_port_mpu_set_region(uint32 rom_addr, uint32 rom_size, uint32 ram_addr, uint32 ram_size);
void svcrt_port_mpu_set_app(const svcrt_arch_mpu_t *p_mpu);
void svcrt_port_mpu_reset(void);

#else

#define svcrt_port_mpu_init()
#define svcrt_port_mpu_set_region(rom_addr, rom_size, ram_addr, ram_size)
#define svcrt_port_mpu_set_app(p_mpu)
#define svcrt_port_mpu_reset()''',
        '''void svcrt_port_mpu_init(void);
void svcrt_port_mpu_set_region(uint32 rom_addr, uint32 rom_size, uint32 ram_addr, uint32 ram_size);
void svcrt_port_mpu_set_app(const svcrt_arch_mpu_t *p_mpu);
void svcrt_port_mpu_reset(void);

/* Encode one region into the architecture independent context.
 * @param p_mpu target context
 * @param idx   region index (0 .. SVCRT_MPU_REGION_MAX-1)
 * @param base  region base address (rounded down to the region size)
 * @param size  bytes to cover (rounded up to a power of two)
 * @param mem   memory attribute kind
 * @return 0 on success, -1 on bad parameters */
int32 svcrt_port_mpu_encode(svcrt_arch_mpu_t *p_mpu, uint32 idx, uint32 base, uint32 size,
                            svcrt_mpu_mem_t mem);

/* Re-apply the idle-task context captured by svcrt_port_mpu_set_region(). */
void svcrt_port_mpu_set_idle(void);

#else

#define svcrt_port_mpu_init()
#define svcrt_port_mpu_set_region(rom_addr, rom_size, ram_addr, ram_size)
#define svcrt_port_mpu_set_app(p_mpu)
#define svcrt_port_mpu_reset()
#define svcrt_port_mpu_encode(p_mpu, idx, base, size, mem)   (-1)
#define svcrt_port_mpu_set_idle()''')])

    # 6) mpu.h / mpu.c rewritten
    write_file('kernelsrc/include/svcrt_mpu.h', NEW_MPU_H)
    write_file('kernelsrc/src/svcrt_mpu.c', NEW_MPU_C)

    # 7) both ARM ports: replace the whole MPU section
    for rel in ('kernelsrc/port/arm/cortex-m3/svcrt_port.c',
                'kernelsrc/port/arm/cortex-m4/svcrt_port.c'):
        p, text, enc = load(rel)
        backup(rel)
        i = text.rfind('#if (SVCRT_USE_MPU == 1)')
        j = text.rfind('#endif')
        assert 0 <= i < j, rel
        assert 'svcrt_port_mpu_set_region' in text[i:], rel
        new = PORT_MPU_SECTION.replace('\n', '\r\n' if '\r\n' in text[i:] else '\n')
        text = text[:i] + new + text[j:]
        save(p, text, enc)
        print('  [ok] %s (MPU section replaced)' % rel)

    # 8) task register fills the context
    replace('kernelsrc/src/svcrt_cfg.c', [
        ('#include "svcrt_fault.h"', '#include "svcrt_fault.h"\n#include "svcrt_mpu.h"'),
        ('''    #if (SVCRT_USE_MPU == 1)
    {
        int32 i;
        for(i = 0; i < SVCRT_MPU_REGION_MAX; i++)
        {
            p_task->mpu.region_base[i] = 0;
            p_task->mpu.region_attr[i] = 0;
        }
    }
    #endif''',
         '''    /* Fill the MPU context (no-op when SVCRT_USE_MPU == 0).
     * The loader calls this again after rewriting rom_start / ram_size for
     * App and Driver tasks, because those windows are only known then. */
    svcrt_mpu_build_task(p_task);''')])

    # 9) loader: rebuild the context after the windows are final, and drop the
    #    context of a task that has just been disabled by the crash policy
    replace('kernelsrc/src/svcrt_loader.c', [
        ('#include "svcrt_task.h"', '#include "svcrt_task.h"\n#include "svcrt_mpu.h"'),
        ('''    svcrt_task_table[task_id - 1].rom_size  = pt->driver_pool_size;''',
         '''    svcrt_task_table[task_id - 1].rom_size  = pt->driver_pool_size;

    /* Windows are final now: build this task's MPU context from them. */
    svcrt_mpu_build_task(&svcrt_task_table[task_id - 1]);'''),
        ('''                                              ? hdr.image_size : 0u;''',
         '''                                              ? hdr.image_size : 0u;

    /* Windows are final now: build this task's MPU context from them. */
    svcrt_mpu_build_task(&svcrt_task_table[task_id - 1]);'''),
        ('''            /* 记一条可诊断的故障：上位机可用 svcrt_fault_record_read() 看到“被禁用”的原因 */
            svcrt_fault_record(SVCRT_FAULT_APPDISABLED, task_id);''',
         '''            /* This task will never be scheduled again: drop its MPU
             * windows immediately instead of leaving a stale context in the
             * hardware until the next task switch. */
            svcrt_mpu_reset();

            /* 记一条可诊断的故障：上位机可用 svcrt_fault_record_read() 看到“被禁用”的原因 */
            svcrt_fault_record(SVCRT_FAULT_APPDISABLED, task_id);''')])

    # 10) switch back to idle re-applies the idle context
    replace('kernelsrc/src/svcrt_task.c', [(
        '''    else
    {
        svcrt_current_task_id = 0;
        return svcrt_idle_stack_ptr;
    }''',
        '''    else
    {
        svcrt_current_task_id = 0;

        /* A task switch rewrites every MPU region register, so the idle task
         * needs its own context restored here (captured at startup by the
         * board through svcrt_port_set_idle_mpu()). */
        svcrt_mpu_set_idle();
        return svcrt_idle_stack_ptr;
    }''')])

    print('MPU isolation patch done')


if __name__ == '__main__':
    main()
