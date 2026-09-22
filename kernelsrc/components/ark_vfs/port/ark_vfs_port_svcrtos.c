/**
* @file ark_vfs_port_svcrtos.c
* @brief SVCrtOS side of the port: block backend, hooks, /dev.
*
* Three pieces, and that is the whole story of porting ark_vfs:
*
*   1. a backend - ark_vfs_backend_t over svcrt_blk_dev_t
*   2. hooks     - a lock and a millisecond clock out of the kernel
*   3. devfs     - optional, but it is what makes the namespace Linux-like
*
* Everything below is written so that the same source also compiles against
* the mock headers in ports/svcrtos/tests/, which is how the offset
* arithmetic and the error mapping are actually verified.
*/
#include <stdarg.h>
#include <stdio.h>

#include "ark_vfs_port_svcrtos.h"
#include "svcrt_blk.h"
#include "svcrt_dev.h"
#include "svcrt_log.h"
#include "svcrt_task.h"

/* SVCrtOS feature gate.  This translation unit *is* the SVCrtOS port, so it
 * disappears together with the kernel's VFS facade: nothing here is
 * reachable without svcrt_vfs.c, and a build with SVCRT_USE_BLK=0 could not
 * compile it anyway (the block backend is what this port is made of).
 * The mock header in tests/ pins the switch on, so the host test is
 * unaffected. */
#include "svcrt_features.h"

#if (SVCRT_USE_VFS != 1)

/* An empty translation unit is not valid C; keep a harmless declaration. */
typedef int ark_vfs_port_svcrtos_disabled_tu;

#else

/* Set to 0 in a build where exactly one task ever touches the VFS. */
#ifndef ARK_VFS_PORT_SVCRTOS_LOCK
#define ARK_VFS_PORT_SVCRTOS_LOCK   1
#endif

/* ============================================================
 * Backend: svcrt_blk_dev_t -> ark_vfs_backend_t
 *
 * The window (base/size) is the only piece of state.  Every offset the
 * filesystem hands down is relative to the volume, so it is translated
 * here and nowhere else - which is why a partition is free: it is just a
 * window that does not start at 0.
 * ============================================================ */

#define ARK_BLK_MAX_XFER   0x7fffffffu     /* int32_t return value limit */

static int32_t p_wnd(ark_vfs_port_blk_t *pb, uint32_t off, uint32_t len)
{
    if (pb == NULL)
    {
        return ARK_E_INVAL;
    }
    if (len > ARK_BLK_MAX_XFER)
    {
        return ARK_E_INVAL;
    }
    if (off > pb->size)
    {
        return ARK_E_INVAL;
    }
    if (len > (pb->size - off))
    {
        return ARK_E_INVAL;
    }
    return ARK_VFS_OK;
}

static int32_t p_blk_read(ark_vfs_backend_t *be, uint32_t off, void *buf,
                          uint32_t len)
{
    ark_vfs_port_blk_t *pb = (ark_vfs_port_blk_t *)be;

    if (buf == NULL)
    {
        return ARK_E_INVAL;
    }
    if (len == 0u)
    {
        return 0;
    }
    if (p_wnd(pb, off, len) != ARK_VFS_OK)
    {
        return ARK_E_INVAL;
    }
    if (svcrt_blk_read(pb->dev, pb->base + off, (uint8 *)buf, len) != 0)
    {
        return ARK_E_IO;
    }
    return (int32_t)len;
}

static int32_t p_blk_write(ark_vfs_backend_t *be, uint32_t off,
                           const void *buf, uint32_t len)
{
    ark_vfs_port_blk_t *pb = (ark_vfs_port_blk_t *)be;

    if (buf == NULL)
    {
        return ARK_E_INVAL;
    }
    if (len == 0u)
    {
        return 0;
    }
    if (p_wnd(pb, off, len) != ARK_VFS_OK)
    {
        return ARK_E_INVAL;
    }
    if (svcrt_blk_write(pb->dev, pb->base + off, (const uint8 *)buf, len) != 0)
    {
        return ARK_E_IO;
    }
    return (int32_t)len;
}

/**
* @brief Erase every erase unit the range touches.
*
* The range is widened outwards to unit boundaries, never inwards: a
* filesystem asks to erase a whole block, and erasing less than asked
* would leave stale bytes that look like a successful write later.  The
* widening is bounded by the window, which is why ark_vfs_port_blk_init()
* insists the window length be a whole number of erase units.
*/
static int32_t p_blk_erase(ark_vfs_backend_t *be, uint32_t off, uint32_t len)
{
    ark_vfs_port_blk_t *pb = (ark_vfs_port_blk_t *)be;
    uint32_t eu;
    uint32_t lo;
    uint32_t hi;

    if (len == 0u)
    {
        return ARK_VFS_OK;
    }
    if (p_wnd(pb, off, len) != ARK_VFS_OK)
    {
        return ARK_E_INVAL;
    }
    if (pb->dev == NULL)
    {
        return ARK_E_IO;
    }
    eu = pb->dev->erase_unit;
    if (eu == 0u)
    {
        /* RAM-like device: there is no erase, and pretending there is would
         * be worse than saying so. */
        return ARK_E_NOSYS;
    }

    lo = off & ~(eu - 1u);
    hi = off + len;
    hi = (hi + (eu - 1u)) & ~(eu - 1u);
    if (hi > pb->size)
    {
        hi = pb->size;
    }
    if (hi <= lo)
    {
        return ARK_E_INVAL;
    }
    if (svcrt_blk_erase(pb->dev, pb->base + lo, hi - lo) != 0)
    {
        return ARK_E_IO;
    }
    return ARK_VFS_OK;
}

static int32_t p_blk_open(ark_vfs_backend_t *be)
{
    ark_vfs_port_blk_t *pb = (ark_vfs_port_blk_t *)be;

    if ((pb == NULL) || (pb->dev == NULL))
    {
        return ARK_E_INVAL;
    }
    /* Idempotent by contract: a second mount of the same chip is free. */
    if (svcrt_blk_init(pb->dev) != 0)
    {
        return ARK_E_IO;
    }
    if (svcrt_blk_size(pb->dev) < (pb->base + pb->size))
    {
        return ARK_E_IO;
    }
    return ARK_VFS_OK;
}

static void p_blk_close(ark_vfs_backend_t *be)
{
    (void)be;
    /* Nothing to power down: the block layer owns the device lifetime, and
     * dropping power here would break a second volume on the same chip. */
}

int32_t ark_vfs_port_blk_init(ark_vfs_port_blk_t *pb, const char *dev_name,
                              uint32_t base, uint32_t size, const char *label)
{
    const svcrt_blk_dev_t *dev;
    uint32_t total;
    uint32_t eu;

    if ((pb == NULL) || (dev_name == NULL) || (dev_name[0] == '\0'))
    {
        return ARK_E_INVAL;
    }

    dev = svcrt_blk_find(dev_name);
    if (dev == NULL)
    {
        return ARK_E_NOENT;
    }
    if (svcrt_blk_init(dev) != 0)
    {
        return ARK_E_IO;
    }
    total = svcrt_blk_size(dev);
    if (total == 0u)
    {
        return ARK_E_IO;
    }
    if (size == 0u)
    {
        size = total - base;
    }
    if ((base >= total) || (size > (total - base)))
    {
        return ARK_E_INVAL;      /* refuse, never clamp */
    }

    eu = dev->erase_unit;
    if (eu != 0u)
    {
        if ((eu & (eu - 1u)) != 0u)
        {
            return ARK_E_INVAL;  /* not a power of two: alignment is undefined */
        }
        if ((size & (eu - 1u)) != 0u)
        {
            /* A partial tail unit cannot be erased without touching the
             * neighbouring volume, so it is refused up front instead of
             * failing much later inside an erase. */
            return ARK_E_INVAL;
        }
    }

    pb->dev         = dev;
    pb->base        = base;
    pb->size        = size;
    pb->label       = label;
    pb->be.name     = (label != NULL) ? label : dev_name;
    pb->be.size     = size;
    pb->be.erase_size = eu;
    pb->be.read     = p_blk_read;
    pb->be.write    = p_blk_write;
    pb->be.erase    = p_blk_erase;
    pb->be.open     = p_blk_open;
    pb->be.close    = p_blk_close;
    pb->be.priv     = pb;
    return ARK_VFS_OK;
}

/* ============================================================
 * Hooks
 * ============================================================ */

#if (ARK_VFS_PORT_SVCRTOS_LOCK == 1)

static void *p_hook_lock(void *user)
{
    (void)user;
    /* Scheduler lock, not a spinlock: an ark_vfs call can end up erasing a
     * flash sector, and a spinlock held that long would stall every
     * interrupt on the chip.  This only stops task switches, and it nests,
     * which is what the core's recursive contract asks for. */
    svcrt_sched_lock_internal();
    return NULL;
}

static void p_hook_unlock(void *user, void *token)
{
    (void)user;
    (void)token;
    (void)svcrt_sched_unlock_internal();
}

#endif /* ARK_VFS_PORT_SVCRTOS_LOCK */

static uint32_t p_hook_now_ms(void *user)
{
    uint32_t tick;
    (void)user;

    tick = svcrt_kernel_get_tick();
#if (SVCRT_TICK_PERIOD_US == 1000u)
    return tick;
#elif ((SVCRT_TICK_PERIOD_US != 0u) && ((1000u % SVCRT_TICK_PERIOD_US) == 0u))
    return tick / (1000u / SVCRT_TICK_PERIOD_US);
#else
    return (uint32_t)(((unsigned long long)tick * SVCRT_TICK_PERIOD_US) / 1000u);
#endif
}

static void p_hook_log(void *user, const char *fmt, ...)
{
    char buf[SVCRT_LOG_BUF_SIZE];
    va_list ap;

    (void)user;
    if (fmt == NULL)
    {
        return;
    }
    va_start(ap, fmt);
    (void)vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1u] = '\0';
    svcrt_log_emit(SVCRT_LOG_WARNING, "ark_vfs", 0u, buf);
}

void ark_vfs_port_hooks(ark_vfs_hooks_t *hooks)
{
    if (hooks == NULL)
    {
        return;
    }
    hooks->lock   = NULL;
    hooks->unlock = NULL;
#if (ARK_VFS_PORT_SVCRTOS_LOCK == 1)
    hooks->lock   = p_hook_lock;
    hooks->unlock = p_hook_unlock;
#endif
    hooks->now_ms = p_hook_now_ms;
    hooks->log    = p_hook_log;
    hooks->user   = NULL;
}

/* ============================================================
 * Ready-made namespace
 * ============================================================ */

int32_t ark_vfs_port_init(void)
{
    ark_vfs_hooks_t hooks;
    int32_t rc;

    ark_vfs_port_hooks(&hooks);
    ark_vfs_init(&hooks);

    /* Ask the core rather than assume: if a future init grows a failure path,
     * the mount below would otherwise report some secondary error and send
     * whoever reads the log looking in the wrong place. */
    if (ark_vfs_is_ready() == 0)
    {
        return ARK_E_IO;
    }

    /* /dev is the one mount the port can set up without being told
     * anything, because it comes from the device registry itself.  Storage
     * volumes stay the caller's decision: the port cannot know which
     * device holds a filesystem, and guessing would produce a mount that
     * looks fine and fails on the first read. */
    rc = ark_vfs_mount("devfs", "/dev", &ark_vfs_svcrtos_devfs, NULL, 0u, NULL);
    return rc;
}

#endif  /* SVCRT_USE_VFS */
