/**
* @file svcrt_crash.h
* @brief Cross-reset crash journal: per-slot fault counts, disable state and
*        the reason the kernel stopped trusting an image.
* @details The restart policy has always had one hole. A fault is counted in
*          the partition table, which lives in shared RAM: shared RAM survives
*          a plain reset, but the counter is rebuilt from scratch on every
*          boot, so an image that takes the whole board down with it (a wedged
*          task the watchdog ends, a HardFault that escalates) restarts with a
*          clean slate and can do it forever. The board does not survive that:
*          the crash happens again before anyone can get a console prompt.
*
*          This module keeps the part of the bookkeeping that has to outlive a
*          reset - the fault count per slot, whether the slot was disabled, and
*          why - in a section the linker is told never to initialize
*          ("UNINIT" in the scatter file, see CRASH_LOG_BASE in
*          config/svcrt_partition.h and tools/gen_scatter.py). Data there is
*          cleared by power-on, not by a reset, which is exactly the lifetime
*          the policy needs: a reset is the event we are counting, a power
*          cycle is the operator saying "start over".
*
*          What it deliberately is NOT:
*            - not a log. It keeps a count and a reason, not a history; the
*              fault ring (svcrt_fault_record) is the history, and it is
*              allowed to forget.
*            - not persistent storage. Power off and the counts are gone; the
*              image then has to earn the limit again. That limit is stated in
*              the header and in the shell output rather than papered over.
*            - not a signature. The journal is validated by a magic, a version
*              and a checksum so that garbage or a half-written record is rejected
*              instead of being read as a crash history, but nothing here
*              protects it against an image that decides to write to it.
*
*          The identity used to decide "does this entry still describe the
*          image that is in that slot" is the pool address the image was
*          loaded at. Slot record indices are reusable; the pool address of a
*          live image is not (an image that moves is re-installed or
*          re-scanned, and both re-tag the entry).
*
* @note Kernel-only header. Apps and drivers must never include it.
*/

#ifndef __SVCRT_CRASH_H__
#define __SVCRT_CRASH_H__

#include "svcrt_types.h"
#include "svcrt_share.h"      /* SVCRT_SLOT_ARRAY_MAX - the slot array length is ABI */

/** @brief Journal magic "SCRJ" - an uninitialized RAM area holds garbage */
#define SVCRT_CRASH_MAGIC       (0x5343524Au)

/** @brief Journal layout version. A build that changes the struct must bump
 *         this: an older journal is then rejected as "not evidence" instead of
 *         being interpreted with the wrong field offsets. */
#define SVCRT_CRASH_VERSION     (1u)

/* ---- why a slot was disabled (stored per slot, 0 = not disabled) ---- */
#define SVCRT_CRASH_REASON_NONE     (0u)   /* slot is not disabled */
#define SVCRT_CRASH_REASON_FAULT    (1u)   /* ran into the consecutive-fault limit */
#define SVCRT_CRASH_REASON_HEARTBEAT (2u)  /* stopped honouring its heartbeat contract */
#define SVCRT_CRASH_REASON_AUDIT    (3u)   /* structure self-audit failed while it ran */

/**
* @brief The journal itself, placed in the UNINIT region by the linker.
* @note Size is checked at compile time against CRASH_LOG_SIZE; growing this
*       struct without growing the region is a build error, not a silent
*       overlap with the end of shared RAM.
*/
typedef struct {
    uint32 magic;                               /* SVCRT_CRASH_MAGIC */
    uint32 version;                             /* SVCRT_CRASH_VERSION */
    uint32 seq;                                 /* bumped by every committed write */
    uint32 boots;                               /* boots recorded here (>=2 = it survived a reset) */
    uint32 crash_cnt[SVCRT_SLOT_ARRAY_MAX];     /* consecutive faults per slot */
    uint32 last_reason[SVCRT_SLOT_ARRAY_MAX];   /* why the most recent fault happened */
    uint32 dis_reason[SVCRT_SLOT_ARRAY_MAX];    /* SVCRT_CRASH_REASON_x (0 = enabled) */
    uint32 dis_tick[SVCRT_SLOT_ARRAY_MAX];      /* tick when disabled (0 = before this boot) */
    uint32 base[SVCRT_SLOT_ARRAY_MAX];          /* pool address the entry belongs to */
    uint32 check;                               /* checksum over every field above */
} svcrt_crash_journal_t;

/**
* @brief Validate the journal, or start a clean one.
* @details Called once per boot, after the partition table exists and before
*          any image is started. A journal that fails magic / version / checksum is
*          discarded rather than trusted: leftover RAM bytes and a half-written
*          record both look like data, and acting on either would disable an
*          image for something it never did.
* @note The slot arrays are SVCRT_SLOT_ARRAY_MAX long, so the journal can
*       never describe fewer slots than the partition table.
*/
void svcrt_crash_init(void);

/**
* @brief Reconcile the journal with the slot table the pool scan just built.
* @details For a slot whose image still sits at the address the entry was
*          written for, the count is adopted. For anything else (free slot,
*          replaced image, moved image) the entry is dropped - a stale count
*          charged to a newcomer would disable an image for someone else's
*          fault. A slot whose adopted count already reached the limit is put
*          back into the disabled state the operator saw before the reset:
*          autostart cleared, slot state INVALID, and a line on the console.
* @note Must run after svcrt_loader_scan(), before the autostart pass.
*/
void svcrt_crash_scan_apply(void);

/**
* @brief Count one fault against a slot, and disable it when the limit is met.
* @param slot    Slot record index.
* @param reason  SVCRT_CRASH_REASON_x to remember as this fault's reason.
* @return The new consecutive-fault count (0 when the slot cannot be
*         accounted for at all).
* @note The count is mirrored into the partition table so existing readers
*       (shell, host GUI) keep working; the journal is the copy that survives
*       a reset. A slot with no pool address has no identity to tag, and is
*       therefore counted in RAM only - the caller is told so by the return
*       value being the RAM count while the journal stays untouched.
*/
uint32 svcrt_crash_fault(uint32 slot, uint32 reason);

/**
* @brief Is this slot disabled by the kernel?
* @return SVCRT_CRASH_REASON_x (non-zero) when disabled, 0 when enabled.
*/
uint32 svcrt_crash_disabled(uint32 slot);

/** @brief Kernel tick at which the slot was disabled (0 = before this boot). */
uint32 svcrt_crash_disabled_tick(uint32 slot);

/**
* @brief Consecutive faults on record for this slot.
* @note This is the copy that survives a reset; slot_crash_cnt in the
*       partition table is mirrored from it, so both answer the same
*       question rather than two slightly different ones.
*/
uint32 svcrt_crash_count(uint32 slot);

/** @brief Reason of the slot's most recent fault (SVCRT_CRASH_REASON_x). */
uint32 svcrt_crash_last_reason(uint32 slot);

/**
* @brief Forget one slot's crash history and clear its disabled state.
* @details Used by the two events that genuinely mean "start over": a fresh
*          install of the image, and an explicit human retry from the console.
*/
void svcrt_crash_forget(uint32 slot);

/** @brief Boots recorded by the current journal (1 right after power-on). */
uint32 svcrt_crash_boots(void);

/** @brief Committed writes to the journal (evidence that it is being kept). */
uint32 svcrt_crash_seq(void);

/** @brief Human-readable name of a disable reason, or "none" for 0. */
const char *svcrt_crash_reason_name(uint32 reason);

/** @brief Address and size of the journal region, for the shell and the audit. */
uint32 svcrt_crash_area_base(void);
uint32 svcrt_crash_area_size(void);

#endif /* __SVCRT_CRASH_H__ */
