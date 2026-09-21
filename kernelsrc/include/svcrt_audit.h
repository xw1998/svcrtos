/**
* @file svcrt_audit.h
* @brief Kernel self-audit: does the shared state still say what it should?
* @details The kernel hands its partition table, its slot records and its task
*          table to code that was not written by the kernel (the loader, the
*          installer, the shell) and runs image code that shares the same
*          privilege level. A stray store can therefore leave a structure
*          looking plausible while it is already wrong, and the failure shows
*          up much later as "the scheduler picked a task that is not runnable"
*          or "the pool handed out an overlapping range".
*
*          This module answers one question per structure: do its fields still
*          satisfy the invariants that the rest of the kernel relies on? It
*          reports the structure and the index of the first offender. It does
*          not try to repair anything and it does not guess a cause: a repair
*          would hide the corruption, and a guessed cause sends the reader in
*          the wrong direction.
*
*          Scope, stated so the output is not read as more than it is:
*            - bounds and cross-structure consistency only (no checksums over
*              the partition table: it is written field by field by several
*              components, so a checksum would only report "somebody wrote
*              something", which the bitmask already says per structure);
*            - the FIFO is reported through its own corruption counters
*              (svcrt_fifo_bad_magic/_bytes) rather than by walking the ring;
*            - "not checked" is a real answer here, and the shell prints it.
*
* @note Kernel-only header. Apps and drivers must never include it.
*/

#ifndef __SVCRT_AUDIT_H__
#define __SVCRT_AUDIT_H__

#include "svcrt_types.h"

/* One bit per structure group. A set bit means "this group contradicted one of
 * its own invariants"; the index carries the first offender seen. */
#define SVCRT_AUDIT_PARTITION   (1u << 0)   /* magic / version / hw id / geometry */
#define SVCRT_AUDIT_LAYOUT      (1u << 1)   /* mode, source, reclaim, slot_max knobs */
#define SVCRT_AUDIT_SLOTS       (1u << 2)   /* slot record fields and their ranges */
#define SVCRT_AUDIT_SLOT_RAM    (1u << 3)   /* per-slot RAM block shape and overlap */
#define SVCRT_AUDIT_SLOT_FLASH  (1u << 4)   /* per-slot Flash ranges must not overlap */
#define SVCRT_AUDIT_TASKS       (1u << 5)   /* task table fields and link indices */
#define SVCRT_AUDIT_SCHED       (1u << 6)   /* readiness bitmap / chains vs task table */

/** @brief Highest bit this module uses (kept explicit so the shell can loop) */
#define SVCRT_AUDIT_BIT_MAX     (SVCRT_AUDIT_SCHED)

/** @brief Result of one audit pass */
typedef struct {
    uint32 mask;        /* SVCRT_AUDIT_x bits that failed (0 = clean) */
    uint32 index;       /* first offending slot / task index, SVCRT_AUDIT_INDEX_NONE if n/a */
    uint32 checks;      /* number of individual field checks performed */
} svcrt_audit_result_t;

/** @brief "no offender index applies" */
#define SVCRT_AUDIT_INDEX_NONE  (0xFFFFFFFFu)

/**
* @brief Run one full audit pass and write the verdict to @p p_out.
* @param p_out  Receives mask / index / checks. Must not be NULL.
* @note Read-only: the audit never modifies the structures it inspects, so it
*       is safe to call from the shell while the system keeps running.
*/
void svcrt_audit_run(svcrt_audit_result_t *p_out);

/**
* @brief One pass kept from the last run, for callers that only want the mask.
* @return The mask from the most recent svcrt_audit_run()/svcrt_audit_boot().
*/
uint32 svcrt_audit_last_mask(void);

/**
* @brief Audit once at boot and record SVCRT_FAULT_AUDITFAIL when it fails.
* @return The failing mask (0 = clean). A non-zero value means the boot-time
*         state already violated an invariant, before any image ran.
*/
uint32 svcrt_audit_boot(void);

/**
* @brief Human-readable name of one audit bit.
* @param bit  A single SVCRT_AUDIT_x bit.
* @return Static string, or NULL when @p bit is not a defined audit bit.
*/
const char *svcrt_audit_name(uint32 bit);

#endif /* __SVCRT_AUDIT_H__ */
