/**
* @file ark_vfs_port_svcrtos.h
* @brief ark_vfs port to SVCrtOS: block devices, locks, clock, /dev.
*
* This is the only place in the tree that knows both sides at once.  The
* core (src/) never sees an SVCrtOS header, and SVCrtOS never sees an
* ark_vfs structure it has to understand - it hands over three function
* pointers, two callbacks and a lock, which is the whole integration.
*
* What the port gives you:
*
*   - ark_vfs_port_blk_t   an ark_vfs_backend_t window onto one SVCrtOS
*                          block device (or onto a slice of one, which is
*                          how a partition becomes a filesystem).
*   - ark_vfs_port_hooks() lock + millisecond clock from the kernel, so a
*                          multi-task caller is safe and mtime is real.
*   - ark_vfs_svcrtos_devfs   the SVCrtOS device registry as a filesystem,
*                          so /dev/uart0 is openable by path.
*
* Porting ark_vfs to another RTOS is the same three pieces; this file is
* meant to be read as the worked example, not as special magic.
*/
#ifndef __ARK_VFS_PORT_SVCRTOS_H__
#define __ARK_VFS_PORT_SVCRTOS_H__

#include "ark_vfs.h"
#include "svcrt_blk.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
* @brief An ark_vfs backend that reads and writes through a SVCrtOS block
*        device.
*
* The struct starts with ark_vfs_backend_t so that &pb->be is a valid
* backend pointer - no cast, no lookup, no wrapper allocation.
*/
typedef struct ark_vfs_port_blk
{
    ark_vfs_backend_t      be;      /**< must stay the first member */
    const svcrt_blk_dev_t *dev;     /**< registry entry, never copied     */
    uint32_t               base;    /**< window start inside the device   */
    uint32_t               size;    /**< window length in bytes           */
    const char            *label;   /**< volume name, diagnostics only    */
} ark_vfs_port_blk_t;

/**
* @brief Bind a block-device window to @p pb and fill in its backend.
*
* @param pb       caller-owned storage; must outlive the mount.
* @param dev_name name in the svcrt_blk registry, e.g. "nor0".
* @param base     offset of the window inside the device.
* @param size     window length; 0 means "to the end of the device".
* @param label    name for diagnostics, may be NULL.
* @return ARK_VFS_OK, or:
*         ARK_E_NOENT  no such block device is registered
*         ARK_E_INVAL  pb/dev_name NULL, or the window runs off the device
*         ARK_E_IO     the device is registered but will not come up
*
* @note Bringing the device up is part of the call: svcrt_blk_init() is
*       idempotent, and a backend that is not up cannot answer `size`.
*       A window that does not fit is refused instead of clamped, because
*       a silently shortened volume is a corruption that shows up much
*       later, far away from here.
*/
int32_t ark_vfs_port_blk_init(ark_vfs_port_blk_t *pb, const char *dev_name,
                              uint32_t base, uint32_t size, const char *label);

/**
* @brief Fill @p hooks with the SVCrtOS lock and clock.
* @note The lock is svcrt_sched_lock()/unlock: it stops task switches for
*       the duration of one ark_vfs call but leaves interrupts running,
*       which is what you want around a filesystem call - a spinlock held
*       across a flash erase would stall every interrupt on the chip.
*       Set ARK_VFS_PORT_SVCRTOS_LOCK to 0 to compile the port lock-free,
*       for a build where only one task ever touches the VFS.
*       now_ms is derived from svcrt_kernel_get_tick(); with a 500 us tick
*       that is tick/2, and the divide is only pulled in when the tick
*       period actually divides 1000. */
void ark_vfs_port_hooks(ark_vfs_hooks_t *hooks);

/**
* @brief Convenience: ark_vfs_init() with the SVCrtOS hooks installed,
*        then mount devfs at /dev.
* @return ARK_VFS_OK, or the first mount error encountered.
* @note Mounting the storage volumes is the caller's business - the port
*       does not guess which device holds a filesystem.
*/
int32_t ark_vfs_port_init(void);

/** @brief The SVCrtOS device registry as a flat /dev: /dev/<name>. */
extern const ark_vfs_fsdrv_t ark_vfs_svcrtos_devfs;

#ifdef __cplusplus
}
#endif

#endif /* __ARK_VFS_PORT_SVCRTOS_H__ */
