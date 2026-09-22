/**
* @file ark_vfs_devfs.c
* @brief SVCrtOS device registry exposed as a filesystem: /dev/<name>.
*
* Why a filesystem and not a special case in the core: every shortcut for
* "paths that are really devices" is a branch the core would carry forever
* and every other port would have to reproduce.  A device node table is a
* perfectly ordinary read-only-on-directory-listing filesystem, so it is
* one here.
*
* Shape:
*   - flat.  /dev/uart0, never /dev/serial/0 - the SVCrtOS registry has no
*     hierarchy and inventing one would be a lie about the hardware.
*   - names come from the registry, so they cannot drift from reality.
*   - a node is readable and writable; nothing else.  There is no create,
*     no unlink, no rename: a device that does not exist cannot be made to
*     exist by opening it.
*   - open() always passes param 0 to the driver.  A path has nowhere to
*     carry a driver-specific open parameter, and pretending otherwise
*     would mean guessing one.
*/
#include "ark_vfs.h"
#include "ark_vfs_priv.h"
#include "ark_vfs_port_svcrtos.h"
#include "svcrt_dev.h"

/* SVCrtOS feature gate: /dev only exists inside the kernel's VFS namespace,
 * so this unit goes away with SVCRT_USE_VFS.  The mock header in tests/
 * pins the switch on, so the host test is unaffected. */
#include "svcrt_features.h"

#if (SVCRT_USE_VFS != 1)

/* An empty translation unit is not valid C; keep a harmless declaration. */
typedef int ark_vfs_devfs_disabled_tu;

#else

/* The core treats this as opaque and keeps only a pointer, so the layout
 * is entirely ours.  (Each fsdrv defines its own; two filesystems in one
 * image never look inside each other's handles.) */
struct ark_vfs_file {
    uint8_t  used;
    uint8_t  is_dir;
    int16_t  dev;                   /* svcrt_dev handle, or -1          */
    uint32_t scan;                  /* readdir cursor, slot index       */
};

/* Local name buffer.  Only used while probing the registry: the entries
 * themselves are copied straight into ark_vfs_dirent.name, whose size is
 * ARK_VFS_NAME_MAX and is checked before every copy. */
#define DEVFS_NAME_MAX   (16)

static struct ark_vfs_file g_devfs_handles[ARK_VFS_MAX_FDS];

static struct ark_vfs_file *p_handle_alloc(void)
{
    uint32_t i;

    for (i = 0u; i < (uint32_t)ARK_VFS_MAX_FDS; i++)
    {
        if (g_devfs_handles[i].used == 0u)
        {
            ark_p_memset(&g_devfs_handles[i], 0, (ark_size_t)sizeof(g_devfs_handles[i]));
            g_devfs_handles[i].used = 1u;
            g_devfs_handles[i].dev  = -1;
            return &g_devfs_handles[i];
        }
    }
    return NULL;
}

/** @brief The name of a registered device, or NULL.  Writes into @p out. */
static const char *p_dev_name(uint32_t slot, char *out, uint32_t max)
{
    if (svcrt_dev_name_at(slot, out, max) != 0)
    {
        return NULL;
    }
    return out;
}

/* ------------------------------------------------------------
 * fsdrv
 * ------------------------------------------------------------ */

static int32_t devfs_mount(struct ark_vfs_mnt *mnt)
{
    uint32_t i;

    (void)mnt;
    for (i = 0u; i < (uint32_t)ARK_VFS_MAX_FDS; i++)
    {
        g_devfs_handles[i].used = 0u;
    }
    return ARK_VFS_OK;
}

static int32_t devfs_umount(struct ark_vfs_mnt *mnt)
{
    (void)mnt;
    return ARK_VFS_OK;
}

static int32_t devfs_open(struct ark_vfs_mnt *mnt, const char *rel, int flags,
                          struct ark_vfs_file **out)
{
    char name[DEVFS_NAME_MAX];
    struct ark_vfs_file *h;
    int32_t dev;
    uint32_t i;

    (void)mnt;
    (void)flags;   /* RDONLY/WRONLY/RDWR differ only in what the caller may
                    * do next; the device itself does not care. */

    if (rel[0] == '\0')
    {
        return ARK_E_ISDIR;
    }
    for (i = 0u; rel[i] != '\0'; i++)
    {
        if (rel[i] == '/')
        {
            return ARK_E_NOENT;      /* /dev is flat */
        }
    }
    if (ark_p_strlen(rel) >= (ark_size_t)sizeof(name))
    {
        return ARK_E_NAMETOOLONG;
    }
    ark_p_strcpy(name, (ark_size_t)sizeof(name), rel);

    dev = svcrt_dev_open_internal(name, 0u);
    if (dev < 0)
    {
        /* The registry is the authority on what exists, so a failed open
         * of a name that is not in it is "no such file", and a failed open
         * of a name that is in it is "I/O" - two different stories, and
         * the caller deserves to be told which one happened. */
        char probe[DEVFS_NAME_MAX];
        uint32_t slot;

        for (slot = 0u; slot < (uint32_t)SVCRT_DEV_MAX_NUM; slot++)
        {
            const char *n = p_dev_name(slot, probe, (uint32_t)sizeof(probe));
            if ((n != NULL) && (ark_p_strcmp(n, name) == 0))
            {
                return ARK_E_IO;
            }
        }
        return ARK_E_NOENT;
    }

    h = p_handle_alloc();
    if (h == NULL)
    {
        (void)svcrt_dev_close_internal(dev);
        return ARK_E_FULL;
    }
    h->is_dir = 0u;
    h->dev    = (int16_t)dev;
    *out = h;
    return ARK_VFS_OK;
}

static int32_t devfs_close(struct ark_vfs_mnt *mnt, struct ark_vfs_file *f)
{
    int32_t rc = ARK_VFS_OK;

    (void)mnt;
    if (f == NULL)
    {
        return ARK_E_INVAL;
    }
    if ((f->is_dir == 0u) && (f->dev >= 0))
    {
        if (svcrt_dev_close_internal(f->dev) != 0)
        {
            rc = ARK_E_IO;
        }
    }
    f->used = 0u;
    f->dev  = -1;
    return rc;
}

static ark_ssize_t devfs_read(struct ark_vfs_file *f, void *buf, ark_size_t n)
{
    int32_t rc;

    if ((f == NULL) || (f->is_dir != 0u) || (f->dev < 0))
    {
        return ARK_E_BADF;
    }
    if (buf == NULL)
    {
        return ARK_E_INVAL;
    }
    if (n > 0x7fffffffu)
    {
        return ARK_E_INVAL;
    }
    rc = svcrt_dev_read_internal(f->dev, (uint8 *)buf, (int32)n);
    if (rc < 0)
    {
        return ARK_E_IO;
    }
    /* 0 stays 0: on a character device that is "nothing available right
     * now", and it is the honest thing to pass through rather than
     * inventing an end-of-file for a device that never ends. */
    return (ark_ssize_t)rc;
}

static ark_ssize_t devfs_write(struct ark_vfs_file *f, const void *buf,
                               ark_size_t n)
{
    int32_t rc;

    if ((f == NULL) || (f->is_dir != 0u) || (f->dev < 0))
    {
        return ARK_E_BADF;
    }
    if (buf == NULL)
    {
        return ARK_E_INVAL;
    }
    if (n > 0x7fffffffu)
    {
        return ARK_E_INVAL;
    }
    rc = svcrt_dev_write_internal(f->dev, (uint8 *)buf, (int32)n);
    if (rc < 0)
    {
        return ARK_E_IO;
    }
    return (ark_ssize_t)rc;
}

static int32_t devfs_stat(struct ark_vfs_mnt *mnt, const char *rel,
                          struct ark_vfs_stat *st)
{
    char name[DEVFS_NAME_MAX];
    uint32_t i;
    uint32_t slot;

    (void)mnt;
    if (st == NULL)
    {
        return ARK_E_INVAL;
    }
    if (rel[0] == '\0')
    {
        st->mode  = ARK_S_IFDIR;
        st->size  = 0;
        st->mtime = 0u;
        return ARK_VFS_OK;
    }
    for (i = 0u; rel[i] != '\0'; i++)
    {
        if (rel[i] == '/')
        {
            return ARK_E_NOENT;
        }
    }
    if (ark_p_strlen(rel) >= (ark_size_t)sizeof(name))
    {
        return ARK_E_NAMETOOLONG;
    }
    ark_p_strcpy(name, (ark_size_t)sizeof(name), rel);

    for (slot = 0u; slot < (uint32_t)SVCRT_DEV_MAX_NUM; slot++)
    {
        char probe[DEVFS_NAME_MAX];
        if (svcrt_dev_name_at(slot, probe, (uint32_t)sizeof(probe)) != 0)
        {
            continue;
        }
        if (ark_p_strcmp(probe, name) == 0)
        {
            st->mode  = ARK_S_IFCHR;
            st->size  = 0;
            st->mtime = 0u;
            return ARK_VFS_OK;
        }
    }
    return ARK_E_NOENT;
}

static int32_t devfs_opendir(struct ark_vfs_mnt *mnt, const char *rel,
                             struct ark_vfs_file **out)
{
    struct ark_vfs_file *h;

    (void)mnt;
    if (rel[0] != '\0')
    {
        return ARK_E_NOTDIR;    /* no subdirectories to descend into */
    }
    h = p_handle_alloc();
    if (h == NULL)
    {
        return ARK_E_FULL;
    }
    h->is_dir = 1u;
    h->scan   = 0u;
    *out = h;
    return ARK_VFS_OK;
}

static int32_t devfs_readdir(struct ark_vfs_file *d, struct ark_vfs_dirent *ent)
{
    uint32_t slot;

    if ((d == NULL) || (d->is_dir == 0u))
    {
        return ARK_E_NOTDIR;
    }
    if (ent == NULL)
    {
        return ARK_E_INVAL;
    }
    for (slot = d->scan; slot < (uint32_t)SVCRT_DEV_MAX_NUM; slot++)
    {
        char name[DEVFS_NAME_MAX];
        ark_size_t nlen;

        d->scan = slot + 1u;
        if (svcrt_dev_name_at(slot, name, (uint32_t)sizeof(name)) != 0)
        {
            continue;
        }
        nlen = ark_p_strlen(name);
        if ((nlen + 1u) > (ark_size_t)ARK_VFS_NAME_MAX)
        {
            continue;
        }
        ark_p_memcpy(ent->name, name, nlen + 1u);
        ent->mode = ARK_S_IFCHR;
        ent->size = 0;
        return ARK_VFS_OK;
    }
    /* End of listing: same code as a missing name, by the core's design -
     * both mean "there is nothing at this name". */
    return ARK_E_NOENT;
}

static int32_t devfs_closedir(struct ark_vfs_mnt *mnt, struct ark_vfs_file *d)
{
    (void)mnt;
    if (d != NULL)
    {
        d->used = 0u;
    }
    return ARK_VFS_OK;
}

const ark_vfs_fsdrv_t ark_vfs_svcrtos_devfs = {
    "devfs",
    ARK_FS_CAP_READ | ARK_FS_CAP_WRITE | ARK_FS_CAP_DIR | ARK_FS_CAP_STAT,
    devfs_mount,
    devfs_umount,
    devfs_open,
    devfs_close,
    devfs_read,
    devfs_write,
    NULL,               /* seek: a device has no file position to set    */
    devfs_stat,
    NULL,               /* unlink: nothing to remove                     */
    NULL,               /* mkdir:  /dev is flat                          */
    NULL,               /* rename: a name IS the device                  */
    devfs_opendir,
    devfs_readdir,
    devfs_closedir
};

#endif  /* SVCRT_USE_VFS */
