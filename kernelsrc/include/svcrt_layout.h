/**
* @file svcrt_layout.h
* @brief Kernel-side layout resolution: reads the device-side configuration
*        record (if any), validates it and publishes the effective layout.
* @details Called once during boot, right after svcrt_ptable_init() and before
*          the pool is scanned: the loader, the installer and the shell all ask
*          this module where an image is allowed to live and whether the device
*          runs in fixed-slot or automatic-placement mode.
*
*          The module owns exactly one window of Flash (the CONFIG region) and
*          is the only writer of it. Everything else stays read-only for
*          callers, so a broken or missing record can never make the device
*          unusable: the compile-time default layout in
*          config/svcrt_partition.h is always available as a fallback.
*
* @note The record format itself lives in svcrt_layout_def.h, which is shared
*       with the host tools. This header is kernel-only.
*/

#ifndef __SVCRT_LAYOUT_H__
#define __SVCRT_LAYOUT_H__

#include "svcrt_types.h"
#include "svcrt_layout_def.h"

/* ============================================================
 * 启动期解析
 * ============================================================ */

/**
* @brief Resolve the effective layout and publish it into the partition table.
* @details Order: start from the compile-time default layout, then try to load
*          and validate the device-side record; a valid record overrides the
*          default, anything else (absent / bad magic / bad CRC / hw mismatch /
*          illegal slot table) keeps the default and stores a reason code that
*          the shell prints. Never returns an error: a device without a usable
*          layout still boots.
* @note Must be called after svcrt_ptable_init() (it writes partition-table
*       fields) and before the pool scan (the scan uses the mode).
*/
void   svcrt_layout_init(void);

/* ============================================================
 * 只读查询
 * ============================================================ */

uint32 svcrt_layout_mode(void);        /**< SVCRT_LAYOUT_MODE_x currently in effect */
uint32 svcrt_layout_source(void);      /**< SVCRT_LAYOUT_SOURCE_x */
uint32 svcrt_layout_slot_count(void);  /**< entries in the effective fixed-slot table */
uint32 svcrt_layout_reason(void);      /**< SVCRT_CFG_ERR_x of the last rejected record (0 = none) */

/**
* @brief Apply the runtime knobs of the effective configuration.
* @details Called at the end of svcrt_layout_init(). Only the knobs this
*          kernel build really honours are applied -- today the log level
*          and the crash-restart budget, both of which the shared
*          partition table then carries for the Loader. Every other knob
*          in the record is rejected by svcrt_layout_validate() with
*          SVCRT_CFG_ERR_NOT_IMPL rather than accepted and ignored: a
*          configuration that says one thing and makes the device do
*          another is the hardest kind of problem to find later.
*/
void   svcrt_layout_apply_runtime(void);

uint32 svcrt_layout_reclaim_mode(void);     /**< SVCRT_CFG_RECLAIM_x in effect */
uint32 svcrt_layout_cfg_log_level(void);    /**< log level the record asked for (0 = compile-time) */
uint32 svcrt_layout_cfg_restart_max(void);  /**< crash restart budget in effect */

/** @brief Effective fixed-slot table (SVCRT_CFG_SLOT_MAX entries, type 0 = unused). */
const svcrt_cfg_slot_t *svcrt_layout_slots(void);

/** @brief One entry of the effective table, or 0 when index is out of range. */
const svcrt_cfg_slot_t *svcrt_layout_slot(uint32 index);

/**
* @brief Index of the fixed slot whose Flash range contains addr.
* @return Slot index, or -1 when no fixed slot covers addr.
*/
int32  svcrt_layout_slot_by_base(uint32 addr);

/** @brief Human-readable name of a validation reason code (ASCII, for the shell). */
const char *svcrt_layout_reason_text(uint32 reason);

/* ============================================================
 * 记录校验
 * ============================================================ */

/**
* @brief Validate a record: identity, CRC, hardware match and the whole slot table.
* @return SVCRT_CFG_OK or one of the SVCRT_CFG_ERR_x codes.
* @note The slot table is checked against the kernel's own pool and RAM bounds,
*       so a record prepared for another board or another memory map is rejected
*       instead of being applied.
*/
uint32 svcrt_layout_validate(const svcrt_cfg_record_t *rec);

/* ============================================================
 * 写入 / 擦除（上位机经串口下发时使用）
 * ============================================================ */

/**
* @brief Append a validated record to the CONFIG region and adopt it.
* @param record  exactly SVCRT_CFG_RECORD_SIZE bytes as produced by tools/svcrt_layout.py
* @param len     must equal SVCRT_CFG_RECORD_SIZE
* @param p_erased optional: set to 1 when the region had to be erased first
* @return >= 0 = record index written, < 0 = error (see the SVCRT_LAYOUT_ERR_x codes)
* @details The kernel stamps seq and hw_compat_id itself and recomputes the CRC,
*          so a host that sends a slightly stale hw id still gets a record that
*          matches this kernel. If the region is full it is erased once and
*          writing restarts at offset 0; a reset during that single erase falls
*          back to the default layout instead of losing the device.
*/
int32  svcrt_layout_write(const uint8 *record, uint32 len, uint32 *p_erased);

/**
* @brief Erase the CONFIG region and fall back to the compile-time default layout.
* @return 0 on success, < 0 on error.
*/
int32  svcrt_layout_erase(void);

/**
* @brief Read the live record (highest valid seq) from the CONFIG region.
* @param out  destination buffer
* @param p_seq optional: seq of the record that was read
* @return 0 on success, < 0 when the region holds no valid record.
*/
int32  svcrt_layout_read(svcrt_cfg_record_t *out, uint32 *p_seq);

/** @brief CRC32 of a record with its crc32 field taken as zero (zlib-compatible). */
uint32 svcrt_layout_crc(const svcrt_cfg_record_t *rec);

/* error codes returned by svcrt_layout_write() / svcrt_layout_erase() */
#define SVCRT_LAYOUT_ERR_ARG      (-1)   /* bad pointer / wrong length */
#define SVCRT_LAYOUT_ERR_NOREGION (-2)  /* CONFIG_SIZE is 0 or too small */
#define SVCRT_LAYOUT_ERR_CRC      (-3)  /* record CRC is wrong */
#define SVCRT_LAYOUT_ERR_INVALID  (-4)  /* record failed svcrt_layout_validate() */
#define SVCRT_LAYOUT_ERR_ERASE    (-5)
#define SVCRT_LAYOUT_ERR_WRITE    (-6)
#define SVCRT_LAYOUT_ERR_VERIFY   (-7)   /* read-back did not match what was written */

#endif /* __SVCRT_LAYOUT_H__ */
