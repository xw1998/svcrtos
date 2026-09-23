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
#define SVCRT_CRASH_VERSION     (2u)

/* ---- why a slot was disabled (stored per slot, 0 = not disabled) ---- */
#define SVCRT_CRASH_REASON_NONE     (0u)   /* slot is not disabled */
#define SVCRT_CRASH_REASON_FAULT    (1u)   /* ran into the consecutive-fault limit */
#define SVCRT_CRASH_REASON_HEARTBEAT (2u)  /* stopped honouring its heartbeat contract */
#define SVCRT_CRASH_REASON_AUDIT    (3u)   /* structure self-audit failed while it ran */

/* ---- MiniApps (svcrt_mini.c) -------------------------------------------------
 * A MiniApp has no slot and no pool address, so the identity used for slots does
 * not apply. Its identity is the FNV-1a hash of the image path inside the file
 * system, and the bookkeeping lives in its own table so the two policies cannot
 * be confused for one another. The limit itself is NOT a second constant: both
 * paths read the same cfg_restart_max (see svcrt_crash_limit).
 * The table has to fit in the same UNINIT region as the slot arrays, which is
 * why it is smaller than SVCRT_SLOT_ARRAY_MAX: a disabled MiniApp is a sticky
 * entry an operator may still want, while eight of them already means the board
 * is in a state no operator would keep. */
#define SVCRT_CRASH_MINI_MAX    (8u)
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
    uint32 mini_hash[SVCRT_CRASH_MINI_MAX];     /* path hash of the tracked MiniApp (0 = free) */
    uint32 mini_cnt[SVCRT_CRASH_MINI_MAX];      /* consecutive faults for that path */
    uint32 mini_last_reason[SVCRT_CRASH_MINI_MAX]; /* why its most recent fault happened */
    uint32 mini_dis_reason[SVCRT_CRASH_MINI_MAX];  /* SVCRT_CRASH_REASON_x (0 = enabled) */
    uint32 mini_dis_tick[SVCRT_CRASH_MINI_MAX];    /* tick when disabled (0 = before this boot) */
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

/* ==================================================================
 * MiniApp bookkeeping (see SVCRT_CRASH_MINI_MAX above)
 *
 * The identity is a 32-bit FNV-1a hash of the image path rather than the path
 * itself: the journal region is a fixed 512-byte window shared with the slot
 * arrays, and eight paths of up to 64 bytes would not fit. The hash is used
 * only to tell two paths apart, and both ends (the run path and the console)
 * recompute it from the same string, so a collision would need two live paths
 * hashing equal - and would then be about as visible as any other wrong answer
 * this module exists to refuse.
 * ================================================================== */

/** @brief FNV-1a hash of an image path, the MiniApp identity. @return 0 only
 *         for a null/empty path (never a real path) so 0 can mean "free". */
uint32 svcrt_crash_hash_path(const char *path);

/**
* @brief Count one fault against a MiniApp path, disabling it at the limit.
* @param hash    svcrt_crash_hash_path() of the path.
* @param reason  SVCRT_CRASH_REASON_x to remember.
* @return The new consecutive-fault count, or 0 when it could not be recorded
*         at all (hash 0, or every journal entry already held by a disabled
*         MiniApp - the caller is told so instead of being given a made-up
*         count).
* @note Runs from exception context on the fault path: critical section only.
*/
uint32 svcrt_crash_mini_fault(uint32 hash, uint32 reason);

/** @brief SVCRT_CRASH_REASON_x when this path is disabled, 0 when it may run. */
uint32 svcrt_crash_mini_disabled(uint32 hash);

/** @brief Consecutive faults on record for this path (0 when untracked). */
uint32 svcrt_crash_mini_count(uint32 hash);

/** @brief Tick at which this path was disabled (0 = not disabled / before boot). */
uint32 svcrt_crash_mini_disabled_tick(uint32 hash);

/**
* @brief Read one MiniApp journal entry by table index.
* @details The slot table can be listed because a slot has a number; a MiniApp
*          is identified by a path hash instead, so a listing has to walk the
*          table index by index. A free entry reads back as all zeroes so a
*          caller sees the same "nothing here" a slot listing sees for an
*          empty slot, rather than a path hash it then has to explain away.
* @param idx   0..SVCRT_CRASH_MINI_MAX-1.
* @param hash  receives the path hash (0 = free).
* @param cnt   receives the consecutive-fault count.
* @param last  receives the most recent fault reason (SVCRT_CRASH_REASON_x).
* @param hold  receives the disable reason (SVCRT_CRASH_REASON_x, 0 = enabled).
* @param tick  receives the tick it was disabled at (0 = not / before this boot).
* @return 1 when the entry is in use, 0 when free or idx is out of range.
* @note Only the hash is kept, never the path: the journal must stay a fixed
*       size in UNINIT RAM, and a path is variable-length. A caller that wants
*       the path has to match the hash against a path it already knows.
*/
uint32 svcrt_crash_mini_at(uint32 idx, uint32 *hash, uint32 *cnt,
                           uint32 *last, uint32 *hold, uint32 *tick);

/** @brief Forget this path's crash history and clear its disabled state. */
void svcrt_crash_mini_forget(uint32 hash);

#endif /* __SVCRT_CRASH_H__ */
