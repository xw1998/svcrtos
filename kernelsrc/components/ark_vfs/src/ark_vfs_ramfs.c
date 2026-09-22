/**
* @file ark_vfs_ramfs.c
* @brief A complete filesystem in one file: files, directories, no allocator.
*
* Design choices worth knowing before you read the code:
*
*   - Fixed file capacity (ARK_RAMFS_FILE_CAP).  A shared byte pool would
*     fit more in the same footprint, but it needs a free list and suffers
*     fragmentation; a fixed cap is predictable, has no failure mode you
*     cannot see, and keeps this file short enough to read in one sitting.
*     A 64 KB image does not need a clever allocator.
*
*   - Directories are names, not inodes.  A directory holds no children
*     list; membership is derived from the path prefix of each entry, so
*     adding/removing a file can never leave the tree inconsistent.
*
*   - Open handles come from a static table.  Running out reports
*     ARK_E_FULL rather than growing.
*/
#include "ark_vfs.h"
#include "ark_vfs_priv.h"
#include "ark_vfs_ramfs.h"

#ifndef ARK_RAMFS_MAX_FILES
#define ARK_RAMFS_MAX_FILES     8
#endif
#ifndef ARK_RAMFS_MAX_DIRS
#define ARK_RAMFS_MAX_DIRS      4
#endif
/* Follows the core's fd table by default: if this were smaller, running out
 * of core fds would be masked by running out of filesystem handles first,
 * and the error would point at the wrong layer. */
#ifndef ARK_RAMFS_MAX_OPEN
#define ARK_RAMFS_MAX_OPEN      ARK_VFS_MAX_FDS
#endif
#ifndef ARK_RAMFS_FILE_CAP
#define ARK_RAMFS_FILE_CAP      512
#endif

/* Open handle.  The core treats this as an opaque struct, so the shape is
 * entirely ours - that is the point of the fsdrv boundary. */
struct ark_vfs_file {
    uint8_t  used;
    uint8_t  is_dir;
    uint8_t  flags;
    uint16_t pos;
    uint16_t scan;                  /* readdir cursor */
    int16_t  node;                  /* index into g_nodes, or -1 */
};

struct ark_ramfs_node {
    uint8_t  used;
    uint16_t size;
    char     name[ARK_VFS_PATH_MAX];   /* path relative to the mount root */
    uint8_t  data[ARK_RAMFS_FILE_CAP];
};

struct ark_ramfs_dir {
    uint8_t  used;
    char     name[ARK_VFS_PATH_MAX];
};

static struct ark_ramfs_node   g_nodes[ARK_RAMFS_MAX_FILES];
static struct ark_ramfs_dir    g_dirs[ARK_RAMFS_MAX_DIRS];
static struct ark_vfs_file     g_handles[ARK_RAMFS_MAX_OPEN];

/* ------------------------------------------------------------
 * Small local helpers
 * ------------------------------------------------------------ */

static int p_has_slash(const char *s)
{
    while (*s != '\0')
    {
        if (*s == '/')
        {
            return 1;
        }
        s++;
    }
    return 0;
}

static struct ark_ramfs_node *p_find_file(const char *rel)
{
    int i;

    for (i = 0; i < ARK_RAMFS_MAX_FILES; i++)
    {
        if ((g_nodes[i].used != 0u) && (ark_p_strcmp(g_nodes[i].name, rel) == 0))
        {
            return &g_nodes[i];
        }
    }
    return NULL;
}

static struct ark_ramfs_dir *p_find_dir(const char *rel)
{
    int i;

    for (i = 0; i < ARK_RAMFS_MAX_DIRS; i++)
    {
        if ((g_dirs[i].used != 0u) && (ark_p_strcmp(g_dirs[i].name, rel) == 0))
        {
            return &g_dirs[i];
        }
    }
    return NULL;
}

/** @brief Is @p full a direct child of directory @p dir_rel?
 *
 *  @param out_base receives the name inside that directory (no prefix).
 *  @return 1 if it is a direct child.  A directory is a child of itself by
 *  this test, which is why callers skip an exact match. */
static int p_child_of(const char *dir_rel, const char *full, const char **out_base)
{
    ark_size_t dlen = ark_p_strlen(dir_rel);

    if (dlen == 0u)
    {
        if (p_has_slash(full) != 0)
        {
            return 0;
        }
        *out_base = full;
        return 1;
    }
    if (ark_p_strncmp(full, dir_rel, dlen) != 0)
    {
        return 0;
    }
    if (full[dlen] != '/')
    {
        return 0;
    }
    if (p_has_slash(&full[dlen + 1u]) != 0)
    {
        return 0;
    }
    *out_base = &full[dlen + 1u];
    return 1;
}

/** @brief Everything under @p dir_rel must be gone before it can be removed. */
static int p_dir_has_children(const char *dir_rel)
{
    int i;

    for (i = 0; i < ARK_RAMFS_MAX_FILES; i++)
    {
        const char *base;
        if ((g_nodes[i].used != 0u) &&
            (p_child_of(dir_rel, g_nodes[i].name, &base) != 0) &&
            (ark_p_strcmp(g_nodes[i].name, dir_rel) != 0))
        {
            return 1;
        }
    }
    for (i = 0; i < ARK_RAMFS_MAX_DIRS; i++)
    {
        const char *base;
        if ((g_dirs[i].used != 0u) &&
            (p_child_of(dir_rel, g_dirs[i].name, &base) != 0) &&
            (ark_p_strcmp(g_dirs[i].name, dir_rel) != 0))
        {
            return 1;
        }
    }
    return 0;
}

/** @brief Creating "a/b" implicitly creates "a" - and must, or the tree
 *         would contain a file whose parent does not exist. */
static int32_t p_ensure_parents(const char *rel)
{
    ark_size_t i;
    char parent[ARK_VFS_PATH_MAX];

    for (i = 0u; rel[i] != '\0'; i++)
    {
        if (rel[i] == '/')
        {
            if (i == 0u)
            {
                return ARK_E_INVAL;     /* leading '/' should not survive normalisation */
            }
            ark_p_memcpy(parent, rel, i);
            parent[i] = '\0';
            if (p_find_dir(parent) == NULL)
            {
                int j;

                for (j = 0; j < ARK_RAMFS_MAX_DIRS; j++)
                {
                    if (g_dirs[j].used == 0u)
                    {
                        if (ark_p_strcpy(g_dirs[j].name, ARK_VFS_PATH_MAX, parent) < 0)
                        {
                            return ARK_E_NAMETOOLONG;
                        }
                        g_dirs[j].used = 1u;
                        break;
                    }
                }
                if (j == ARK_RAMFS_MAX_DIRS)
                {
                    return ARK_E_FULL;
                }
            }
        }
    }
    return ARK_VFS_OK;
}

/* ------------------------------------------------------------
 * Open handles
 * ------------------------------------------------------------ */

static struct ark_vfs_file *p_handle_alloc(void)
{
    int i;

    for (i = 0; i < ARK_RAMFS_MAX_OPEN; i++)
    {
        if (g_handles[i].used == 0u)
        {
            ark_p_memset(&g_handles[i], 0, (ark_size_t)sizeof(g_handles[i]));
            g_handles[i].used = 1u;
            g_handles[i].node = -1;
            return &g_handles[i];
        }
    }
    return NULL;
}

static int p_node_index(struct ark_ramfs_node *node)
{
    if (node == NULL)
    {
        return -1;
    }
    return (int)(node - &g_nodes[0]);
}

/* ------------------------------------------------------------
 * fsdrv callbacks
 * ------------------------------------------------------------ */

static int32_t ramfs_mount(struct ark_vfs_mnt *mnt)
{
    /* Contents survive a mount/umount cycle on purpose: remounting a scratch
     * area should not silently erase it.  Use ark_ramfs_reset() to clear. */
    (void)mnt;
    return ARK_VFS_OK;
}

static int32_t ramfs_umount(struct ark_vfs_mnt *mnt)
{
    (void)mnt;
    return ARK_VFS_OK;
}

static int32_t ramfs_open(struct ark_vfs_mnt *mnt, const char *rel, int flags,
                          struct ark_vfs_file **out)
{
    struct ark_ramfs_node *node;
    struct ark_vfs_file *h;
    int write_intent = ((flags & ARK_O_ACCMODE) != ARK_O_RDONLY) ||
                       ((flags & (ARK_O_CREAT | ARK_O_TRUNC)) != 0);

    (void)mnt;

    if (rel[0] == '\0')
    {
        return ARK_E_ISDIR;         /* the mount root is a directory */
    }

    node = p_find_file(rel);
    if (node == NULL)
    {
        int i;

        /* A directory is not a file, whatever the flags say.  Checked before
         * O_CREAT so that opening an existing directory never creates a
         * regular file with the same name. */
        if (p_find_dir(rel) != NULL)
        {
            return ARK_E_ISDIR;
        }
        if ((flags & ARK_O_CREAT) == 0)
        {
            return ARK_E_NOENT;
        }
        for (i = 0; i < ARK_RAMFS_MAX_FILES; i++)
        {
            if (g_nodes[i].used == 0u)
            {
                break;
            }
        }
        if (i == ARK_RAMFS_MAX_FILES)
        {
            return ARK_E_FULL;
        }
        node = &g_nodes[i];
        node->used = 1u;
        node->size = 0u;
        if (ark_p_strcpy(node->name, ARK_VFS_PATH_MAX, rel) < 0)
        {
            node->used = 0u;
            return ARK_E_NAMETOOLONG;
        }
        if (p_ensure_parents(rel) != ARK_VFS_OK)
        {
            node->used = 0u;
            return ARK_E_FULL;
        }
    }
    else
    {
        if ((flags & ARK_O_TRUNC) != 0)
        {
            node->size = 0u;
        }
    }

    h = p_handle_alloc();
    if (h == NULL)
    {
        if (write_intent != 0)
        {
            /* We may have just created the file; leave it behind rather than
             * deleting data we did not own.  The caller gets a clear error. */
        }
        return ARK_E_FULL;
    }
    h->flags = (uint8_t)flags;
    h->node = p_node_index(node);
    if ((flags & ARK_O_APPEND) != 0)
    {
        h->pos = node->size;
    }
    *out = h;
    return ARK_VFS_OK;
}

static int32_t ramfs_close(struct ark_vfs_mnt *mnt, struct ark_vfs_file *f)
{
    (void)mnt;
    if (f != NULL)
    {
        f->used = 0u;
    }
    return ARK_VFS_OK;
}

static ark_ssize_t ramfs_read(struct ark_vfs_file *f, void *buf, ark_size_t n)
{
    struct ark_ramfs_node *node;
    ark_size_t avail;
    ark_size_t take;

    if ((f == NULL) || (f->node < 0))
    {
        return ARK_E_BADF;
    }
    node = &g_nodes[f->node];
    avail = (f->pos < node->size) ? (ark_size_t)(node->size - f->pos) : 0u;
    take = (n < avail) ? n : avail;
    if (take > 0u)
    {
        ark_p_memcpy(buf, &node->data[f->pos], take);
        f->pos = (uint16_t)(f->pos + take);
    }
    return (ark_ssize_t)take;
}

static ark_ssize_t ramfs_write(struct ark_vfs_file *f, const void *buf, ark_size_t n)
{
    struct ark_ramfs_node *node;
    ark_size_t room;
    ark_size_t take;

    if ((f == NULL) || (f->node < 0))
    {
        return ARK_E_BADF;
    }
    node = &g_nodes[f->node];
    room = (f->pos < ARK_RAMFS_FILE_CAP) ? (ark_size_t)(ARK_RAMFS_FILE_CAP - f->pos) : 0u;
    if (room == 0u)
    {
        return ARK_E_NOSPC;
    }
    take = (n < room) ? n : room;
    ark_p_memcpy(&node->data[f->pos], buf, take);
    f->pos = (uint16_t)(f->pos + take);
    if (f->pos > node->size)
    {
        node->size = f->pos;
    }
    return (ark_ssize_t)take;
}

static int32_t ramfs_seek(struct ark_vfs_file *f, ark_off_t off, int whence)
{
    struct ark_ramfs_node *node;
    ark_off_t base;
    ark_off_t target;

    if ((f == NULL) || (f->node < 0))
    {
        return ARK_E_BADF;
    }
    node = &g_nodes[f->node];

    if (whence == ARK_SEEK_SET)
    {
        base = 0;
    }
    else if (whence == ARK_SEEK_CUR)
    {
        base = (ark_off_t)f->pos;
    }
    else if (whence == ARK_SEEK_END)
    {
        base = (ark_off_t)node->size;
    }
    else
    {
        return ARK_E_INVAL;
    }

    target = base + off;
    if ((target < 0) || (target > (ark_off_t)ARK_RAMFS_FILE_CAP))
    {
        return ARK_E_INVAL;
    }
    f->pos = (uint16_t)target;
    return (int32_t)f->pos;
}

static int32_t ramfs_stat(struct ark_vfs_mnt *mnt, const char *rel,
                          struct ark_vfs_stat *st)
{
    struct ark_ramfs_node *node;
    struct ark_ramfs_dir *dir;

    (void)mnt;

    if (rel[0] == '\0')
    {
        st->mode = ARK_S_IFDIR;
        st->size = 0;
        st->mtime = 0u;
        return ARK_VFS_OK;
    }
    node = p_find_file(rel);
    if (node != NULL)
    {
        st->mode = ARK_S_IFREG;
        st->size = (ark_off_t)node->size;
        st->mtime = 0u;
        return ARK_VFS_OK;
    }
    dir = p_find_dir(rel);
    if (dir != NULL)
    {
        st->mode = ARK_S_IFDIR;
        st->size = 0;
        st->mtime = 0u;
        return ARK_VFS_OK;
    }
    return ARK_E_NOENT;
}

static int32_t ramfs_unlink(struct ark_vfs_mnt *mnt, const char *rel)
{
    struct ark_ramfs_node *node;
    int i;

    (void)mnt;

    node = p_find_file(rel);
    if (node != NULL)
    {
        node->used = 0u;
        node->size = 0u;
        node->name[0] = '\0';
        return ARK_VFS_OK;
    }
    for (i = 0; i < ARK_RAMFS_MAX_DIRS; i++)
    {
        if ((g_dirs[i].used != 0u) && (ark_p_strcmp(g_dirs[i].name, rel) == 0))
        {
            if (p_dir_has_children(rel) != 0)
            {
                return ARK_E_NOTEMPTY;
            }
            g_dirs[i].used = 0u;
            g_dirs[i].name[0] = '\0';
            return ARK_VFS_OK;
        }
    }
    return ARK_E_NOENT;
}

static int32_t ramfs_mkdir(struct ark_vfs_mnt *mnt, const char *rel)
{
    int i;

    (void)mnt;

    if ((p_find_file(rel) != NULL) || (p_find_dir(rel) != NULL))
    {
        return ARK_E_EXIST;
    }
    if (p_ensure_parents(rel) != ARK_VFS_OK)
    {
        return ARK_E_FULL;
    }
    for (i = 0; i < ARK_RAMFS_MAX_DIRS; i++)
    {
        if (g_dirs[i].used == 0u)
        {
            if (ark_p_strcpy(g_dirs[i].name, ARK_VFS_PATH_MAX, rel) < 0)
            {
                return ARK_E_NAMETOOLONG;
            }
            g_dirs[i].used = 1u;
            return ARK_VFS_OK;
        }
    }
    return ARK_E_FULL;
}

static int32_t ramfs_rename(struct ark_vfs_mnt *mnt, const char *old_rel,
                            const char *new_rel)
{
    struct ark_ramfs_node *node;
    int i;

    (void)mnt;

    node = p_find_file(old_rel);
    if (node != NULL)
    {
        if ((p_find_file(new_rel) != NULL) || (p_find_dir(new_rel) != NULL))
        {
            return ARK_E_EXIST;
        }
        if (p_ensure_parents(new_rel) != ARK_VFS_OK)
        {
            return ARK_E_FULL;
        }
        if (ark_p_strcpy(node->name, ARK_VFS_PATH_MAX, new_rel) < 0)
        {
            return ARK_E_NAMETOOLONG;
        }
        return ARK_VFS_OK;
    }
    for (i = 0; i < ARK_RAMFS_MAX_DIRS; i++)
    {
        if ((g_dirs[i].used != 0u) && (ark_p_strcmp(g_dirs[i].name, old_rel) == 0))
        {
            if ((p_find_file(new_rel) != NULL) || (p_find_dir(new_rel) != NULL))
            {
                return ARK_E_EXIST;
            }
            if (p_ensure_parents(new_rel) != ARK_VFS_OK)
            {
                return ARK_E_FULL;
            }
            if (ark_p_strcpy(g_dirs[i].name, ARK_VFS_PATH_MAX, new_rel) < 0)
            {
                return ARK_E_NAMETOOLONG;
            }
            return ARK_VFS_OK;
        }
    }
    return ARK_E_NOENT;
}

static int32_t ramfs_opendir(struct ark_vfs_mnt *mnt, const char *rel,
                             struct ark_vfs_file **out)
{
    struct ark_vfs_file *h;

    (void)mnt;

    if ((rel[0] != '\0') && (p_find_dir(rel) == NULL))
    {
        return (p_find_file(rel) != NULL) ? ARK_E_NOTDIR : ARK_E_NOENT;
    }
    h = p_handle_alloc();
    if (h == NULL)
    {
        return ARK_E_FULL;
    }
    h->is_dir = 1u;
    h->scan = 0u;
    if (rel[0] != '\0')
    {
        /* Remember which directory we are listing by pointing at its node
         * slot; scan walks the tables from the start. */
        int i;

        for (i = 0; i < ARK_RAMFS_MAX_DIRS; i++)
        {
            if ((g_dirs[i].used != 0u) && (ark_p_strcmp(g_dirs[i].name, rel) == 0))
            {
                h->node = (int16_t)(ARK_RAMFS_MAX_FILES + i);   /* tagged index */
                break;
            }
        }
    }
    else
    {
        h->node = -1;    /* root */
    }
    *out = h;
    return ARK_VFS_OK;
}

static int32_t ramfs_readdir(struct ark_vfs_file *d, struct ark_vfs_dirent *ent)
{
    const char *dir_rel;
    int i;

    if ((d == NULL) || (d->is_dir == 0u))
    {
        return ARK_E_NOTDIR;
    }

    if (d->node < 0)
    {
        dir_rel = "";
    }
    else
    {
        dir_rel = g_dirs[d->node - ARK_RAMFS_MAX_FILES].name;
    }

    for (i = (int)d->scan; i < (ARK_RAMFS_MAX_FILES + ARK_RAMFS_MAX_DIRS); i++)
    {
        const char *base = NULL;
        ark_size_t nlen;

        d->scan = (uint16_t)(i + 1);

        if (i < ARK_RAMFS_MAX_FILES)
        {
            if ((g_nodes[i].used == 0u) ||
                (p_child_of(dir_rel, g_nodes[i].name, &base) == 0) ||
                (ark_p_strcmp(g_nodes[i].name, dir_rel) == 0))
            {
                continue;
            }
            nlen = ark_p_strlen(base);
            if ((nlen + 1u) > ARK_VFS_NAME_MAX)
            {
                continue;
            }
            ark_p_memcpy(ent->name, base, nlen + 1u);
            ent->mode = ARK_S_IFREG;
            ent->size = (ark_off_t)g_nodes[i].size;
            return ARK_VFS_OK;
        }
        else
        {
            int j = i - ARK_RAMFS_MAX_FILES;

            if ((g_dirs[j].used == 0u) ||
                (p_child_of(dir_rel, g_dirs[j].name, &base) == 0) ||
                (ark_p_strcmp(g_dirs[j].name, dir_rel) == 0))
            {
                continue;
            }
            nlen = ark_p_strlen(base);
            if ((nlen + 1u) > ARK_VFS_NAME_MAX)
            {
                continue;
            }
            ark_p_memcpy(ent->name, base, nlen + 1u);
            ent->mode = ARK_S_IFDIR;
            ent->size = 0;
            return ARK_VFS_OK;
        }
    }

    /* End of directory.  ARK_E_NOENT is the documented "no more entries",
     * which is deliberately the same code a missing file gets: both mean
     * "there is nothing at this name". */
    return ARK_E_NOENT;
}

static int32_t ramfs_closedir(struct ark_vfs_mnt *mnt, struct ark_vfs_file *d)
{
    (void)mnt;
    if (d != NULL)
    {
        d->used = 0u;
    }
    return ARK_VFS_OK;
}

/* ------------------------------------------------------------
 * Driver descriptor
 * ------------------------------------------------------------ */

static const ark_vfs_fsdrv_t g_ramfs = {
    "ramfs",
    ARK_FS_CAP_READ | ARK_FS_CAP_WRITE | ARK_FS_CAP_CREATE | ARK_FS_CAP_UNLINK |
    ARK_FS_CAP_MKDIR | ARK_FS_CAP_RENAME | ARK_FS_CAP_DIR | ARK_FS_CAP_STAT |
    ARK_FS_CAP_TRUNC,
    ramfs_mount,
    ramfs_umount,
    ramfs_open,
    ramfs_close,
    ramfs_read,
    ramfs_write,
    ramfs_seek,
    ramfs_stat,
    ramfs_unlink,
    ramfs_mkdir,
    ramfs_rename,
    ramfs_opendir,
    ramfs_readdir,
    ramfs_closedir
};

const ark_vfs_fsdrv_t *ark_ramfs_fs(void)
{
    return &g_ramfs;
}

void ark_ramfs_reset(void)
{
    int i;

    for (i = 0; i < ARK_RAMFS_MAX_FILES; i++)
    {
        g_nodes[i].used = 0u;
        g_nodes[i].size = 0u;
        g_nodes[i].name[0] = '\0';
    }
    for (i = 0; i < ARK_RAMFS_MAX_DIRS; i++)
    {
        g_dirs[i].used = 0u;
        g_dirs[i].name[0] = '\0';
    }
    for (i = 0; i < ARK_RAMFS_MAX_OPEN; i++)
    {
        g_handles[i].used = 0u;
    }
}
