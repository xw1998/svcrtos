/**
* @file svcrt_blk.h
* @brief SVCrtOS block device registry - one handle per non-volatile backend
* @details The image pool (and the filesystem on top of it) needs to talk to
*          "a chunk of non-volatile storage"without knowing whether that chunk
*          is the internal flash controller, an external SPI NOR, or something
*          a future board adds. This file is that seam.
*
*          A backend registers one static descriptor at board init; everything
*          above (loader / pool / littlefs / shell) then works with byte
*          offsets only and never sees a peripheral register or a CPU address.
*
*          Contract - read this before wiring a new backend:
*            - off/len are byte offsets relative to the device base, never CPU
*              addresses. A memory mapped backend translates internally.
*            - erase_unit is the smallest erasable unit. A write may only land
*              on bytes that are already 0xFF (i.e. erased). Programming a
*              non-erased byte is a caller bug: the device cannot do it and
*              this layer will not pretend otherwise.
*            - read/write/erase are all-or-nothing: they return 0 on success
*              and -1 on failure. There is no partial-length return; a short
*              transfer is a failure.
*            - callers must not access a device before svcrt_blk_init() has
*              returned 0 for it.
*
*          The name is the stable identity used by the shell and by config.
*          Keep it short, lowercase and unique: "int0" = internal flash pool
*          region, "nor0" = external SPI NOR on the F427 board.
*
* @author xw
* @date 2026.09.21
*/
#ifndef SVCRT_BLK_H
#define SVCRT_BLK_H

#include "svcrt_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Max bytes of a device name including the terminating zero */
#define SVCRT_BLK_NAME_MAX   (16u)

/** How many backends a build can carry. Raise here, not at the call sites. */
#define SVCRT_BLK_MAX_DEVS   (4u)

/**
 * @brief One storage backend.
 * @note  Every function pointer except read/write/erase is optional. The
 *        descriptor itself is expected to be static storage owned by the
 *        backend, because the registry keeps a pointer, not a copy.
 */
typedef struct svcrt_blk_dev
{
    const char *name;        /**< registry key, see SVCRT_BLK_NAME_MAX      */
    uint32      size;        /**< capacity in bytes; 0 if only size_fn knows */
    uint32      erase_unit;  /**< smallest erasable unit, in bytes, pow2     */

    int32  (*bringup)(void);                        /**< optional, idempotent  */
    uint32 (*size_fn)(void);                        /**< optional, live probe  */
    int32  (*read)(uint32 off, uint8 *buf, uint32 len);
    int32  (*write)(uint32 off, const uint8 *buf, uint32 len);
    int32  (*erase)(uint32 off, uint32 len);
} svcrt_blk_dev_t;

/**
 * @brief Register a backend.
 * @return 0 on success, -1 if the table is full, the name is empty/too long,
 *         or the descriptor lacks read/write/erase.
 */
int32 svcrt_blk_register(const svcrt_blk_dev_t *dev);

/** @brief Number of registered backends (0 means the board registered none) */
uint32 svcrt_blk_count(void);

/** @brief Backend by index in registration order, NULL when out of range */
const svcrt_blk_dev_t *svcrt_blk_at(uint32 idx);

/** @brief Backend by name, NULL when not found */
const svcrt_blk_dev_t *svcrt_blk_find(const char *name);

/**
 * @brief Bring a backend up. Idempotent: the second call is free.
 * @return 0 when the device is usable, -1 otherwise (stay unavailable).
 */
int32 svcrt_blk_init(const svcrt_blk_dev_t *dev);

/** @brief 1 when svcrt_blk_init() has already succeeded for this backend */
uint8 svcrt_blk_is_ready(const svcrt_blk_dev_t *dev);

/** @brief Capacity in bytes; 0 when the device is not up or cannot answer */
uint32 svcrt_blk_size(const svcrt_blk_dev_t *dev);

/**
 * @brief Read len bytes at offset off.
 * @note  A range check runs first, so a bad command cannot touch hardware.
 */
int32 svcrt_blk_read (const svcrt_blk_dev_t *dev, uint32 off, uint8 *buf, uint32 len);

/** @brief Write len bytes at offset off (target must already be erased) */
int32 svcrt_blk_write(const svcrt_blk_dev_t *dev, uint32 off, const uint8 *buf, uint32 len);

/**
 * @brief Erase len bytes starting at off.
 * @note  off and len must both be multiples of erase_unit. A non-aligned
 *        request is rejected instead of being silently rounded, because
 *        rounding up can wipe a neighbouring record the caller still needs.
 */
int32 svcrt_blk_erase(const svcrt_blk_dev_t *dev, uint32 off, uint32 len);

#ifdef __cplusplus
}
#endif

#endif /* SVCRT_BLK_H */
