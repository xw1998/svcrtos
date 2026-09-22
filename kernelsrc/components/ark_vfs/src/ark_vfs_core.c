/**
* @file ark_vfs_core.c
* @brief Mount table, path resolution and the fd table.
*
* The core deliberately knows nothing about storage.  It answers three
* questions and hands the rest down:
*
*   - which mount point owns this path?   (longest prefix wins)
*   - is that path allowed to be written? (mount flags, then fs caps)
*   - which open file does this fd name?  (a fixed table, no allocation)
*
* Everything below the fsdrv line sees only a normalised path relative to a
* mount root.  That is what keeps a filesystem driver portable: it never
* learns where it was mounted, and it cannot tell a partition from a whole
* chip.
*/
#include "ark_vfs.h"
#include "ark_vfs_priv.h"

/* ------------------------------------------------------------
 * State.  All of it is static: the library has no heap and no globals that
 * grow.  Footprint = MAX_MOUNTS * sizeof(mnt) + MAX_FDS * sizeof(fd).
 * ------------------------------------------------------------ */

struct ark_vfs_fd_slot {
    uint8_t              used;
    uint8_t              is_dir;
    uint8_t              flags;
    struct ark_vfs_mnt  *mnt;
    struct ark_vfs_file *file;
};

static struct ark_vfs_mnt    g_mounts[ARK_VFS_MAX_MOUNTS];
static uint8_t               g_mount_used[ARK_VFS_MAX_MOUNTS];
static struct ark_vfs_fd_slot g_fds[ARK_VFS_MAX_FDS];
static ark_vfs_hooks_t       g_hooks;
static ark_size_t            g_open_count;
static uint8_t               g_inited;

/* ------------------------------------------------------------
 * Locking.  Every public entry point takes the recursive lock if one was
 * handed in, and does nothing if none was.  The inner functions never lock,
 * so the library cannot deadlock against itself when one call is built on
 * top of another (read_all is open + read + close).
 * ------------------------------------------------------------ */

static void *ark_p_enter(void)
{
    if (g_hooks.lock != NULL)
    {
        return g_hooks.lock(g_hooks.user);
    }
    return NULL;
}

static void ark_p_exit(void *token)
{
    if (g_hooks.unlock != NULL)
    {
        g_hooks.unlock(g_hooks.user, token);
    }
}

static void ark_p_warn(const char *msg, const char *detail)
{
    if (g_hooks.log != NULL)
    {
        g_hooks.log(g_hooks.user, "%s%s", msg, (detail != NULL) ? detail : "");
    }
}

/* ------------------------------------------------------------
 * Static tables
 * ------------------------------------------------------------ */

void ark_vfs_init(const ark_vfs_hooks_t *hooks)
{
    int i;

    for (i = 0; i < ARK_VFS_MAX_MOUNTS; i++)
    {
        g_mount_used[i] = 0u;
        g_mounts[i].target[0] = '\0';
        g_mounts[i].fs = NULL;
        g_mounts[i].be = NULL;
        g_mounts[i].flags = 0u;
        g_mounts[i].fs_priv = NULL;
        g_mounts[i].user = NULL;
    }
    for (i = 0; i < ARK_VFS_MAX_FDS; i++)
    {
        g_fds[i].used = 0u;
        g_fds[i].is_dir = 0u;
        g_fds[i].flags = 0u;
        g_fds[i].mnt = NULL;
        g_fds[i].file = NULL;
    }
    g_open_count = 0u;

    if (hooks != NULL)
    {
        g_hooks = *hooks;
    }
    else
    {
        ark_p_memset(&g_hooks, 0, (ark_size_t)sizeof(g_hooks));
    }
    g_inited = 1u;
}

int32_t ark_vfs_is_ready(void)
{
    return (g_inited != 0u) ? 1 : 0;
}

void ark_vfs_deinit(void)
{
    int i;

    if (g_inited == 0u)
    {
        return;         /* already down; a second sweep would be a no-op anyway */
    }

    for (i = 0; i < ARK_VFS_MAX_MOUNTS; i++)
    {
        if (g_mount_used[i] != 0u)
        {
            (void)ark_vfs_umount(g_mounts[i].target);
        }
    }
    ark_p_memset(&g_hooks, 0, (ark_size_t)sizeof(g_hooks));
    g_inited = 0u;
}

/* ------------------------------------------------------------
 * Path normalisation
 *
 * Written as an explicit segment walk rather than a string rewrite, because
 * ".." has to be applied against what came before it: the only correct way
 * to normalise "/a/b/../c" is to forget "b" first.
 * ------------------------------------------------------------ */

int32_t ark_vfs_normalize(const char *path, char *out, ark_size_t out_size)
{
    ark_size_t seg[ARK_VFS_PATH_MAX / 2u + 2u];   /* start offset per level */
    ark_size_t o = 1u;
    ark_size_t depth = 0u;
    ark_size_t i = 0u;

    if ((path == NULL) || (out == NULL) || (out_size < 2u))
    {
        return ARK_E_INVAL;
    }
    if (path[0] == '\0')
    {
        return ARK_E_INVAL;
    }

    out[0] = '/';
    out[1] = '\0';

    while (path[i] == '/')
    {
        i++;
    }

    while (path[i] != '\0')
    {
        ark_size_t start = i;
        ark_size_t len;

        while ((path[i] != '\0') && (path[i] != '/'))
        {
            i++;
        }
        len = i - start;

        if ((len == 1u) && (path[start] == '.'))
        {
            /* "current directory": nothing to do, but it is a real segment */
        }
        else if ((len == 2u) && (path[start] == '.') && (path[start + 1u] == '.'))
        {
            if (depth == 0u)
            {
                /* Walking above the root is a caller bug: report it instead
                 * of quietly turning "../../etc" into "/etc". */
                out[0] = '\0';
                return ARK_E_LOOP;
            }
            depth--;
            o = seg[depth];
            out[o] = '\0';
        }
        else if (len > 0u)
        {
            ark_size_t need;

            if (len >= ARK_VFS_NAME_MAX)
            {
                out[0] = '\0';
                return ARK_E_NAMETOOLONG;
            }
            need = ((o > 1u) ? 1u : 0u) + len + 1u;
            if ((o + need) > out_size)
            {
                out[0] = '\0';
                return ARK_E_NAMETOOLONG;
            }
            seg[depth] = o;
            if (o > 1u)
            {
                out[o] = '/';
                o++;
            }
            depth++;
            ark_p_memcpy(&out[o], &path[start], len);
            o += len;
            out[o] = '\0';
        }
        else
        {
            /* empty segment: "//" or a trailing '/', already collapsed */
        }

        while (path[i] == '/')
        {
            i++;
        }
    }

    return ARK_VFS_OK;
}

/* ------------------------------------------------------------
 * Mount table
 * ------------------------------------------------------------ */

static struct ark_vfs_mnt *ark_p_mount_at(const char *norm_target)
{
    int i;

    for (i = 0; i < ARK_VFS_MAX_MOUNTS; i++)
    {
        if ((g_mount_used[i] != 0u) &&
            (ark_p_strcmp(g_mounts[i].target, norm_target) == 0))
        {
            return &g_mounts[i];
        }
    }
    return NULL;
}

/* Longest prefix match with a component boundary check, so "/mnt/n" does
 * not match the mount point "/mnt/nor".
 *
 * The boundary test has to treat "/" as an empty prefix: its own trailing
 * separator is the boundary, and every normalised path already starts with
 * that character.  Comparing "/" literally instead would make the root
 * mount match nothing but the root itself. */
static struct ark_vfs_mnt *ark_p_mount_for(const char *norm, ark_size_t *out_tlen)
{
    struct ark_vfs_mnt *best = NULL;
    ark_size_t best_len = 0u;
    int i;

    for (i = 0; i < ARK_VFS_MAX_MOUNTS; i++)
    {
        ark_size_t tlen;
        ark_size_t plen;

        if (g_mount_used[i] == 0u)
        {
            continue;
        }
        tlen = ark_p_strlen(g_mounts[i].target);
        plen = (tlen > 1u) ? tlen : 0u;
        if (ark_p_strncmp(norm, g_mounts[i].target, plen) != 0)
        {
            continue;
        }
        if ((norm[plen] != '\0') && (norm[plen] != '/'))
        {
            continue;
        }
        if (tlen >= best_len)
        {
            best = &g_mounts[i];
            best_len = tlen;
        }
    }
    if (out_tlen != NULL)
    {
        *out_tlen = best_len;
    }
    return best;
}

int32_t ark_vfs_resolve(const char *path, struct ark_vfs_mnt **out_mnt, char *rel)
{
    char norm[ARK_VFS_PATH_MAX];
    ark_size_t tlen = 0u;
    struct ark_vfs_mnt *mnt;
    int32_t r;

    r = ark_vfs_normalize(path, norm, (ark_size_t)sizeof(norm));
    if (r != ARK_VFS_OK)
    {
        return r;
    }

    mnt = ark_p_mount_for(norm, &tlen);
    if (mnt == NULL)
    {
        return ARK_E_NOENT;
    }

    if (rel != NULL)
    {
        const char *p = &norm[tlen];

        while (*p == '/')
        {
            p++;
        }
        if (ark_p_strcpy(rel, ARK_VFS_PATH_MAX, p) < 0)
        {
            return ARK_E_NAMETOOLONG;
        }
    }

    if (out_mnt != NULL)
    {
        *out_mnt = mnt;
    }
    return ARK_VFS_OK;
}

int32_t ark_vfs_mount(const char *source, const char *target,
                      const ark_vfs_fsdrv_t *fs, ark_vfs_backend_t *be,
                      uint32_t flags, void *user)
{
    char norm[ARK_VFS_PATH_MAX];
    struct ark_vfs_mnt *mnt = NULL;
    int32_t r;
    int i;
    void *token;

    (void)source;   /* kept by the caller for listing; the core does not read it */

    if ((target == NULL) || (fs == NULL))
    {
        return ARK_E_INVAL;
    }
    r = ark_vfs_normalize(target, norm, (ark_size_t)sizeof(norm));
    if (r != ARK_VFS_OK)
    {
        return r;
    }

    token = ark_p_enter();

    if (ark_p_mount_at(norm) != NULL)
    {
        ark_p_exit(token);
        return ARK_E_EXIST;
    }
    for (i = 0; i < ARK_VFS_MAX_MOUNTS; i++)
    {
        if (g_mount_used[i] == 0u)
        {
            mnt = &g_mounts[i];
            break;
        }
    }
    if (mnt == NULL)
    {
        ark_p_exit(token);
        return ARK_E_FULL;
    }

    (void)ark_p_strcpy(mnt->target, ARK_VFS_PATH_MAX, norm);
    mnt->fs = fs;
    mnt->be = be;
    mnt->flags = (uint8_t)flags;
    mnt->fs_priv = NULL;
    mnt->user = user;
    g_mount_used[i] = 1u;

    /* Bring up storage first, then the filesystem that sits on it.  A
     * failure at either step leaves no trace: the slot goes back to free,
     * so a failed mount cannot be mistaken for a successful one. */
    if ((be != NULL) && (be->open != NULL))
    {
        if (be->open(be) != 0)
        {
            g_mount_used[i] = 0u;
            ark_p_exit(token);
            return ARK_E_IO;
        }
    }
    if ((fs->mount != NULL) && (fs->mount(mnt) != ARK_VFS_OK))
    {
        if ((be != NULL) && (be->close != NULL))
        {
            be->close(be);
        }
        g_mount_used[i] = 0u;
        mnt->fs = NULL;
        mnt->be = NULL;
        ark_p_exit(token);
        return ARK_E_IO;
    }

    ark_p_exit(token);
    return ARK_VFS_OK;
}

int32_t ark_vfs_umount(const char *target)
{
    char norm[ARK_VFS_PATH_MAX];
    struct ark_vfs_mnt *mnt;
    int32_t r;
    int i;
    void *token;

    if (target == NULL)
    {
        return ARK_E_INVAL;
    }
    r = ark_vfs_normalize(target, norm, (ark_size_t)sizeof(norm));
    if (r != ARK_VFS_OK)
    {
        return r;
    }

    token = ark_p_enter();
    mnt = ark_p_mount_at(norm);
    if (mnt == NULL)
    {
        ark_p_exit(token);
        return ARK_E_NOENT;
    }

    /* Anything still open under this mount would be left pointing at a
     * filesystem that no longer exists.  Refuse instead. */
    for (i = 0; i < ARK_VFS_MAX_FDS; i++)
    {
        if ((g_fds[i].used != 0u) && (g_fds[i].mnt == mnt))
        {
            ark_p_exit(token);
            ark_p_warn("ark_vfs: refusing to unmount ", mnt->target);
            return ARK_E_BUSY;
        }
    }

    if ((mnt->fs != NULL) && (mnt->fs->umount != NULL))
    {
        (void)mnt->fs->umount(mnt);
    }
    if ((mnt->be != NULL) && (mnt->be->close != NULL))
    {
        mnt->be->close(mnt->be);
    }

    for (i = 0; i < ARK_VFS_MAX_MOUNTS; i++)
    {
        if (&g_mounts[i] == mnt)
        {
            g_mount_used[i] = 0u;
            break;
        }
    }
    mnt->target[0] = '\0';
    mnt->fs = NULL;
    mnt->be = NULL;
    mnt->fs_priv = NULL;
    mnt->user = NULL;

    ark_p_exit(token);
    return ARK_VFS_OK;
}

struct ark_vfs_mnt *ark_vfs_find_mount(const char *target)
{
    char norm[ARK_VFS_PATH_MAX];
    struct ark_vfs_mnt *mnt;
    void *token;

    if (target == NULL)
    {
        return NULL;
    }
    if (ark_vfs_normalize(target, norm, (ark_size_t)sizeof(norm)) != ARK_VFS_OK)
    {
        return NULL;
    }
    token = ark_p_enter();
    mnt = ark_p_mount_at(norm);
    ark_p_exit(token);
    return mnt;
}

struct ark_vfs_mnt *ark_vfs_next_mount(struct ark_vfs_mnt *prev)
{
    int start = 0;
    int i;

    if (prev != NULL)
    {
        start = (int)(prev - &g_mounts[0]) + 1;
        if (start < 0)
        {
            start = 0;
        }
    }
    for (i = start; i < ARK_VFS_MAX_MOUNTS; i++)
    {
        if (g_mount_used[i] != 0u)
        {
            return &g_mounts[i];
        }
    }
    return NULL;
}

/* ------------------------------------------------------------
 * fd table
 * ------------------------------------------------------------ */

static int32_t ark_p_fd_alloc(struct ark_vfs_mnt *mnt, struct ark_vfs_file *file,
                              int flags, int is_dir)
{
    int i;

    for (i = 0; i < ARK_VFS_MAX_FDS; i++)
    {
        if (g_fds[i].used == 0u)
        {
            g_fds[i].used = 1u;
            g_fds[i].is_dir = (uint8_t)((is_dir != 0) ? 1u : 0u);
            g_fds[i].flags = (uint8_t)flags;
            g_fds[i].mnt = mnt;
            g_fds[i].file = file;
            g_open_count++;
            return i;
        }
    }
    return ARK_E_FULL;
}

static struct ark_vfs_fd_slot *ark_p_fd_get(int fd)
{
    if ((fd < 0) || (fd >= ARK_VFS_MAX_FDS))
    {
        return NULL;
    }
    if (g_fds[fd].used == 0u)
    {
        return NULL;
    }
    return &g_fds[fd];
}

static int ark_p_write_intent(int flags)
{
    if ((flags & ARK_O_ACCMODE) != ARK_O_RDONLY)
    {
        return 1;
    }
    if ((flags & (ARK_O_CREAT | ARK_O_TRUNC)) != 0)
    {
        return 1;
    }
    return 0;
}

int32_t ark_vfs_open(const char *path, int flags)
{
    char rel[ARK_VFS_PATH_MAX];
    struct ark_vfs_mnt *mnt = NULL;
    struct ark_vfs_file *file = NULL;
    int32_t r;
    int fd;
    int is_dir;
    void *token;

    token = ark_p_enter();

    r = ark_vfs_resolve(path, &mnt, rel);
    if (r != ARK_VFS_OK)
    {
        ark_p_exit(token);
        return r;
    }
    if ((mnt->flags & ARK_MNT_RO) != 0u)
    {
        if (ark_p_write_intent(flags) != 0)
        {
            ark_p_exit(token);
            return ARK_E_ROFS;
        }
    }

    is_dir = ((flags & ARK_O_DIRECTORY) != 0) ? 1 : 0;
    if (is_dir != 0)
    {
        if (mnt->fs->opendir == NULL)
        {
            ark_p_exit(token);
            return ARK_E_NOSYS;
        }
        r = mnt->fs->opendir(mnt, rel, &file);
    }
    else
    {
        if (mnt->fs->open == NULL)
        {
            ark_p_exit(token);
            return ARK_E_NOSYS;
        }
        r = mnt->fs->open(mnt, rel, flags, &file);
    }
    if (r != ARK_VFS_OK)
    {
        ark_p_exit(token);
        return r;
    }

    fd = ark_p_fd_alloc(mnt, file, flags, is_dir);
    if (fd < 0)
    {
        /* No slot: undo the open so the filesystem does not leak a handle
         * the caller will never be able to close. */
        if (is_dir != 0)
        {
            if (mnt->fs->closedir != NULL)
            {
                (void)mnt->fs->closedir(mnt, file);
            }
        }
        else if (mnt->fs->close != NULL)
        {
            (void)mnt->fs->close(mnt, file);
        }
        ark_p_exit(token);
        return fd;
    }

    ark_p_exit(token);
    return fd;
}

int32_t ark_vfs_close(int fd)
{
    struct ark_vfs_fd_slot *slot;
    int32_t r = ARK_VFS_OK;
    void *token;

    token = ark_p_enter();
    slot = ark_p_fd_get(fd);
    if (slot == NULL)
    {
        ark_p_exit(token);
        return ARK_E_BADF;
    }

    if (slot->is_dir != 0u)
    {
        if ((slot->mnt->fs->closedir != NULL) && (slot->file != NULL))
        {
            r = slot->mnt->fs->closedir(slot->mnt, slot->file);
        }
    }
    else
    {
        if ((slot->mnt->fs->close != NULL) && (slot->file != NULL))
        {
            r = slot->mnt->fs->close(slot->mnt, slot->file);
        }
    }

    slot->used = 0u;
    slot->file = NULL;
    slot->mnt = NULL;
    if (g_open_count > 0u)
    {
        g_open_count--;
    }

    ark_p_exit(token);
    return r;
}

ark_ssize_t ark_vfs_read(int fd, void *buf, ark_size_t n)
{
    struct ark_vfs_fd_slot *slot;
    ark_ssize_t r;
    void *token;

    if ((buf == NULL) && (n > 0u))
    {
        return ARK_E_INVAL;
    }
    token = ark_p_enter();
    slot = ark_p_fd_get(fd);
    if (slot == NULL)
    {
        ark_p_exit(token);
        return ARK_E_BADF;
    }
    if (slot->is_dir != 0u)
    {
        ark_p_exit(token);
        return ARK_E_ISDIR;
    }
    if (slot->mnt->fs->read == NULL)
    {
        ark_p_exit(token);
        return ARK_E_NOSYS;
    }
    r = slot->mnt->fs->read(slot->file, buf, n);
    ark_p_exit(token);
    return r;
}

ark_ssize_t ark_vfs_write(int fd, const void *buf, ark_size_t n)
{
    struct ark_vfs_fd_slot *slot;
    ark_ssize_t r;
    void *token;

    if ((buf == NULL) && (n > 0u))
    {
        return ARK_E_INVAL;
    }
    token = ark_p_enter();
    slot = ark_p_fd_get(fd);
    if (slot == NULL)
    {
        ark_p_exit(token);
        return ARK_E_BADF;
    }
    if ((slot->flags & ARK_O_ACCMODE) == ARK_O_RDONLY)
    {
        ark_p_exit(token);
        return ARK_E_BADF;      /* opened read-only */
    }
    if ((slot->mnt->flags & ARK_MNT_RO) != 0u)
    {
        ark_p_exit(token);
        return ARK_E_ROFS;
    }
    if (slot->mnt->fs->write == NULL)
    {
        ark_p_exit(token);
        return ARK_E_NOSYS;
    }
    r = slot->mnt->fs->write(slot->file, buf, n);
    ark_p_exit(token);
    return r;
}

int32_t ark_vfs_lseek(int fd, ark_off_t off, int whence)
{
    struct ark_vfs_fd_slot *slot;
    int32_t r;
    void *token;

    token = ark_p_enter();
    slot = ark_p_fd_get(fd);
    if (slot == NULL)
    {
        ark_p_exit(token);
        return ARK_E_BADF;
    }
    if (slot->mnt->fs->seek == NULL)
    {
        ark_p_exit(token);
        return ARK_E_NOSYS;
    }
    r = slot->mnt->fs->seek(slot->file, off, whence);
    ark_p_exit(token);
    return r;
}

/* ------------------------------------------------------------
 * Path operations
 * ------------------------------------------------------------ */

int32_t ark_vfs_stat(const char *path, struct ark_vfs_stat *st)
{
    char rel[ARK_VFS_PATH_MAX];
    struct ark_vfs_mnt *mnt = NULL;
    int32_t r;
    void *token;

    if (st == NULL)
    {
        return ARK_E_INVAL;
    }
    token = ark_p_enter();
    r = ark_vfs_resolve(path, &mnt, rel);
    if (r != ARK_VFS_OK)
    {
        ark_p_exit(token);
        return r;
    }

    if (rel[0] == '\0')
    {
        /* The mount root always exists and is always a directory, even if
         * the filesystem has no stat callback to say so. */
        st->mode = ARK_S_IFDIR;
        st->size = 0;
        st->mtime = 0u;
        ark_p_exit(token);
        return ARK_VFS_OK;
    }
    if (mnt->fs->stat == NULL)
    {
        ark_p_exit(token);
        return ARK_E_NOSYS;
    }
    r = mnt->fs->stat(mnt, rel, st);
    ark_p_exit(token);
    return r;
}

int32_t ark_vfs_mkdir(const char *path)
{
    char rel[ARK_VFS_PATH_MAX];
    struct ark_vfs_mnt *mnt = NULL;
    int32_t r;
    void *token;

    token = ark_p_enter();
    r = ark_vfs_resolve(path, &mnt, rel);
    if (r != ARK_VFS_OK)
    {
        ark_p_exit(token);
        return r;
    }
    if ((mnt->flags & ARK_MNT_RO) != 0u)
    {
        ark_p_exit(token);
        return ARK_E_ROFS;
    }
    if (rel[0] == '\0')
    {
        ark_p_exit(token);
        return ARK_E_EXIST;     /* the mount root is already a directory */
    }
    if (mnt->fs->mkdir == NULL)
    {
        ark_p_exit(token);
        return ARK_E_NOSYS;
    }
    r = mnt->fs->mkdir(mnt, rel);
    ark_p_exit(token);
    return r;
}

int32_t ark_vfs_unlink(const char *path)
{
    char rel[ARK_VFS_PATH_MAX];
    struct ark_vfs_mnt *mnt = NULL;
    int32_t r;
    void *token;

    token = ark_p_enter();
    r = ark_vfs_resolve(path, &mnt, rel);
    if (r != ARK_VFS_OK)
    {
        ark_p_exit(token);
        return r;
    }
    if ((mnt->flags & ARK_MNT_RO) != 0u)
    {
        ark_p_exit(token);
        return ARK_E_ROFS;
    }
    if (rel[0] == '\0')
    {
        ark_p_exit(token);
        return ARK_E_INVAL;     /* cannot remove a mount point this way */
    }
    if (mnt->fs->unlink == NULL)
    {
        ark_p_exit(token);
        return ARK_E_NOSYS;
    }
    r = mnt->fs->unlink(mnt, rel);
    ark_p_exit(token);
    return r;
}

int32_t ark_vfs_rename(const char *old_path, const char *new_path)
{
    char old_rel[ARK_VFS_PATH_MAX];
    char new_rel[ARK_VFS_PATH_MAX];
    struct ark_vfs_mnt *old_mnt = NULL;
    struct ark_vfs_mnt *new_mnt = NULL;
    int32_t r;
    void *token;

    token = ark_p_enter();
    r = ark_vfs_resolve(old_path, &old_mnt, old_rel);
    if (r == ARK_VFS_OK)
    {
        r = ark_vfs_resolve(new_path, &new_mnt, new_rel);
    }
    if (r != ARK_VFS_OK)
    {
        ark_p_exit(token);
        return r;
    }
    if (old_mnt != new_mnt)
    {
        /* A cross-mount rename would be a copy in disguise; say so rather
         * than pretending to have moved something. */
        ark_p_exit(token);
        return ARK_E_NOSYS;
    }
    if ((old_mnt->flags & ARK_MNT_RO) != 0u)
    {
        ark_p_exit(token);
        return ARK_E_ROFS;
    }
    if ((old_rel[0] == '\0') || (new_rel[0] == '\0'))
    {
        ark_p_exit(token);
        return ARK_E_INVAL;
    }
    if (old_mnt->fs->rename == NULL)
    {
        ark_p_exit(token);
        return ARK_E_NOSYS;
    }
    r = old_mnt->fs->rename(old_mnt, old_rel, new_rel);
    ark_p_exit(token);
    return r;
}

/* ------------------------------------------------------------
 * Directories
 * ------------------------------------------------------------ */

int32_t ark_vfs_opendir(const char *path)
{
    return ark_vfs_open(path, ARK_O_RDONLY | ARK_O_DIRECTORY);
}

int32_t ark_vfs_readdir(int fd, struct ark_vfs_dirent *ent)
{
    struct ark_vfs_fd_slot *slot;
    int32_t r;
    void *token;

    if (ent == NULL)
    {
        return ARK_E_INVAL;
    }
    token = ark_p_enter();
    slot = ark_p_fd_get(fd);
    if (slot == NULL)
    {
        ark_p_exit(token);
        return ARK_E_BADF;
    }
    if (slot->is_dir == 0u)
    {
        ark_p_exit(token);
        return ARK_E_NOTDIR;
    }
    if (slot->mnt->fs->readdir == NULL)
    {
        ark_p_exit(token);
        return ARK_E_NOSYS;
    }
    r = slot->mnt->fs->readdir(slot->file, ent);
    ark_p_exit(token);
    return r;
}

int32_t ark_vfs_closedir(int fd)
{
    struct ark_vfs_fd_slot *slot;

    slot = ark_p_fd_get(fd);
    if (slot == NULL)
    {
        return ARK_E_BADF;
    }
    if (slot->is_dir == 0u)
    {
        return ARK_E_NOTDIR;
    }
    return ark_vfs_close(fd);
}

/* ------------------------------------------------------------
 * Convenience: whole file, in one call
 * ------------------------------------------------------------ */

int32_t ark_vfs_read_all(const char *path, void *buf, ark_size_t max,
                         ark_size_t *out_len)
{
    int fd;
    ark_size_t total = 0u;

    if (out_len != NULL)
    {
        *out_len = 0u;
    }
    if ((buf == NULL) || (max == 0u))
    {
        return ARK_E_INVAL;
    }

    fd = ark_vfs_open(path, ARK_O_RDONLY);
    if (fd < 0)
    {
        return fd;
    }

    for (;;)
    {
        ark_ssize_t got;

        if (total >= max)
        {
            /* Buffer is full but the file may not be: find out rather than
             * returning a truncated file as if it were complete. */
            uint8_t probe;
            got = ark_vfs_read(fd, &probe, 1u);
            if (got > 0)
            {
                (void)ark_vfs_close(fd);
                return ARK_E_NOSPC;
            }
            break;
        }
        got = ark_vfs_read(fd, (uint8_t *)buf + total, max - total);
        if (got < 0)
        {
            (void)ark_vfs_close(fd);
            return (int32_t)got;
        }
        if (got == 0)
        {
            break;
        }
        total += (ark_size_t)got;
    }

    (void)ark_vfs_close(fd);
    if (out_len != NULL)
    {
        *out_len = total;
    }
    return ARK_VFS_OK;
}

int32_t ark_vfs_write_all(const char *path, const void *buf, ark_size_t len)
{
    int fd;
    ark_size_t done = 0u;

    if ((buf == NULL) && (len > 0u))
    {
        return ARK_E_INVAL;
    }
    fd = ark_vfs_open(path, ARK_O_WRONLY | ARK_O_CREAT | ARK_O_TRUNC);
    if (fd < 0)
    {
        return fd;
    }
    while (done < len)
    {
        ark_ssize_t put = ark_vfs_write(fd, (const uint8_t *)buf + done, len - done);

        if (put <= 0)
        {
            (void)ark_vfs_close(fd);
            return (put < 0) ? (int32_t)put : ARK_E_IO;
        }
        done += (ark_size_t)put;
    }
    if (ark_vfs_close(fd) != ARK_VFS_OK)
    {
        return ARK_E_IO;
    }
    return ARK_VFS_OK;
}

int32_t ark_vfs_exists(const char *path)
{
    struct ark_vfs_stat st;
    int32_t r = ark_vfs_stat(path, &st);

    if (r == ARK_VFS_OK)
    {
        return 1;
    }
    if (r == ARK_E_NOENT)
    {
        return 0;
    }
    return r;
}

/* ------------------------------------------------------------
 * Diagnostics
 * ------------------------------------------------------------ */

const char *ark_vfs_version(void)
{
    return "ark_vfs 0.1.0";
}

const char *ark_vfs_error_name(int32_t err)
{
    switch (err)
    {
    case ARK_VFS_OK:        return "OK";
    case ARK_E_NOENT:       return "no such file or mount";
    case ARK_E_INVAL:       return "invalid argument";
    case ARK_E_FULL:        return "table full";
    case ARK_E_BADF:        return "bad file descriptor";
    case ARK_E_ISDIR:       return "is a directory";
    case ARK_E_NOTDIR:      return "not a directory";
    case ARK_E_EXIST:       return "already exists";
    case ARK_E_NOTEMPTY:    return "directory not empty";
    case ARK_E_ROFS:        return "read-only";
    case ARK_E_NOSPC:       return "no space";
    case ARK_E_IO:          return "backend error";
    case ARK_E_NOSYS:       return "not supported by this filesystem";
    case ARK_E_NAMETOOLONG: return "name too long";
    case ARK_E_LOOP:        return "path escapes its root";
    case ARK_E_BUSY:        return "busy";
    default:                return "unknown";
    }
}

ark_size_t ark_vfs_open_count(void)
{
    return g_open_count;
}
