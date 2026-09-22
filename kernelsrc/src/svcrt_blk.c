/**
* @file svcrt_blk.c
* @brief SVCrtOS block device registry - see svcrt_blk.h for the contract
* @details This file owns three things and nothing else:
*            1) the fixed-size table of registered backends;
*            2) the "has this one been brought up" latch, so bringup runs once
*               even when two callers race for the same device;
*            3) the range/alignment checks that run before any hardware call.
*
*          It deliberately owns no storage of its own: descriptors are static
*          data provided by the board, so the RAM cost is 4 pointers plus 4
*          flags. A board that registers nothing is valid - every lookup then
*          returns NULL and the shell simply reports an empty list.
*
* @author xw
* @date 2026.09.21
*/

#include "svcrt_blk.h"
#include "svcrt_config.h"    /* feature gates: SVCRT_USE_BLK */
#if SVCRT_USE_BLK

static const svcrt_blk_dev_t *g_dev[SVCRT_BLK_MAX_DEVS];
static uint8                  g_ready[SVCRT_BLK_MAX_DEVS];
static uint32                 g_count;

/* ============================================================
 * Registry
 * ============================================================ */
static int32 blk_index(const svcrt_blk_dev_t *dev)
{
    uint32 i;

    if(dev == 0)
    {
        return -1;
    }

    for(i = 0u; i < g_count; i++)
    {
        if(g_dev[i] == dev)
        {
            return (int32)i;
        }
    }

    return -1;
}

int32 svcrt_blk_register(const svcrt_blk_dev_t *dev)
{
    uint32 n = 0u;

    if((dev == 0) || (dev->name == 0) ||
       (dev->read == 0) || (dev->write == 0) || (dev->erase == 0))
    {
        return -1;
    }

    while((dev->name[n] != '\0') && (n < SVCRT_BLK_NAME_MAX))
    {
        n++;
    }

    if((n == 0u) || (n >= SVCRT_BLK_NAME_MAX))
    {
        return -1;
    }

    /* Registering the same descriptor twice is a caller mistake that would
     * make svcrt_blk_find() order-dependent; refuse it instead. */
    if(blk_index(dev) >= 0)
    {
        return -1;
    }

    if(g_count >= SVCRT_BLK_MAX_DEVS)
    {
        return -1;
    }

    g_dev[g_count]   = dev;
    g_ready[g_count] = 0u;
    g_count++;

    return 0;
}

uint32 svcrt_blk_count(void)
{
    return g_count;
}

const svcrt_blk_dev_t *svcrt_blk_at(uint32 idx)
{
    if(idx >= g_count)
    {
        return 0;
    }

    return g_dev[idx];
}

const svcrt_blk_dev_t *svcrt_blk_find(const char *name)
{
    uint32 i;

    if(name == 0)
    {
        return 0;
    }

    for(i = 0u; i < g_count; i++)
    {
        const char *a = g_dev[i]->name;
        const char *b = name;
        uint32 same = 1u;

        while((*a != '\0') || (*b != '\0'))
        {
            if(*a != *b)
            {
                same = 0u;
                break;
            }
            a++;
            b++;
        }

        if(same != 0u)
        {
            return g_dev[i];
        }
    }

    return 0;
}

/* ============================================================
 * Bring-up state
 * ============================================================ */
int32 svcrt_blk_init(const svcrt_blk_dev_t *dev)
{
    int32 idx = blk_index(dev);

    if(idx < 0)
    {
        return -1;
    }

    if(g_ready[idx] != 0u)
    {
        return 0;
    }

    if(dev->bringup != 0)
    {
        if(dev->bringup() != 0)
        {
            return -1;
        }
    }

    /* A backend that reports a size must report a usable one, otherwise the
     * range checks below would accept nothing (or, worse, everything). */
    if((dev->size == 0u) && (dev->size_fn == 0))
    {
        return -1;
    }

    if(svcrt_blk_size(dev) == 0u)
    {
        return -1;
    }

    if((dev->erase_unit == 0u) || ((dev->erase_unit & (dev->erase_unit - 1u)) != 0u))
    {
        return -1;
    }

    g_ready[idx] = 1u;

    return 0;
}

uint8 svcrt_blk_is_ready(const svcrt_blk_dev_t *dev)
{
    int32 idx = blk_index(dev);

    if(idx < 0)
    {
        return 0u;
    }

    return g_ready[idx];
}

uint32 svcrt_blk_size(const svcrt_blk_dev_t *dev)
{
    if(dev == 0)
    {
        return 0u;
    }

    if(dev->size_fn != 0)
    {
        return dev->size_fn();
    }

    return dev->size;
}

/* ============================================================
 * Guarded access
 *
 * Every entry point answers the same way when it cannot do what was asked:
 * -1, without touching hardware. There is no "best effort" path here: a
 * loader that writes a truncated image and a caller that believes a rounded
 * erase both end up with silently corrupted flash, which is far more
 * expensive to debug than a refused command.
 * ============================================================ */
static int32 blk_check(const svcrt_blk_dev_t *dev, uint32 off, uint32 len, uint32 align)
{
    uint32 size;

    if((dev == 0) || (svcrt_blk_is_ready(dev) == 0u))
    {
        return -1;
    }

    if(len == 0u)
    {
        return -1;
    }

    size = svcrt_blk_size(dev);

    if((len > size) || (off > (size - len)))
    {
        return -1;
    }

    if(align != 0u)
    {
        if(((off % align) != 0u) || ((len % align) != 0u))
        {
            return -1;
        }
    }

    return 0;
}

int32 svcrt_blk_read(const svcrt_blk_dev_t *dev, uint32 off, uint8 *buf, uint32 len)
{
    if(buf == 0)
    {
        return -1;
    }

    if(blk_check(dev, off, len, 0u) != 0)
    {
        return -1;
    }

    return dev->read(off, buf, len);
}

int32 svcrt_blk_write(const svcrt_blk_dev_t *dev, uint32 off, const uint8 *buf, uint32 len)
{
    if(buf == 0)
    {
        return -1;
    }

    if(blk_check(dev, off, len, 0u) != 0)
    {
        return -1;
    }

    return dev->write(off, buf, len);
}

int32 svcrt_blk_erase(const svcrt_blk_dev_t *dev, uint32 off, uint32 len)
{
    if(dev == 0)
    {
        return -1;
    }

    if(blk_check(dev, off, len, dev->erase_unit) != 0)
    {
        return -1;
    }

    return dev->erase(off, len);
}

#else   /* SVCRT_USE_BLK == 0 */
/* Block layer compiled out: no device is ever registered, so the table is
 * empty and every transfer is refused.  A file system built on top must
 * therefore be switched off too (svcrt_features.h enforces that). */

int32 svcrt_blk_register(const svcrt_blk_dev_t *dev)
{
    (void)dev;
    return -1;
}

uint32 svcrt_blk_count(void)
{
    return 0u;
}

const svcrt_blk_dev_t *svcrt_blk_at(uint32 idx)
{
    (void)idx;
    return 0;
}

const svcrt_blk_dev_t *svcrt_blk_find(const char *name)
{
    (void)name;
    return 0;
}

int32 svcrt_blk_init(const svcrt_blk_dev_t *dev)
{
    (void)dev;
    return -1;
}

uint8 svcrt_blk_is_ready(const svcrt_blk_dev_t *dev)
{
    (void)dev;
    return 0u;
}

uint32 svcrt_blk_size(const svcrt_blk_dev_t *dev)
{
    (void)dev;
    return 0u;
}

int32 svcrt_blk_read(const svcrt_blk_dev_t *dev, uint32 off, uint8 *buf, uint32 len)
{
    (void)dev;
    (void)off;
    (void)buf;
    (void)len;
    return -1;
}

int32 svcrt_blk_write(const svcrt_blk_dev_t *dev, uint32 off, const uint8 *buf, uint32 len)
{
    (void)dev;
    (void)off;
    (void)buf;
    (void)len;
    return -1;
}

int32 svcrt_blk_erase(const svcrt_blk_dev_t *dev, uint32 off, uint32 len)
{
    (void)dev;
    (void)off;
    (void)len;
    return -1;
}
#endif /* SVCRT_USE_BLK */
