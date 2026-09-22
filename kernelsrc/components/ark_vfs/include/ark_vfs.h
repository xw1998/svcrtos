/**
* @file ark_vfs.h
* @brief ark_vfs - a tiny, dependency-free VFS anyone can port in an afternoon.
*
* Design promises (these are the whole point of the library):
*
*   1. No OS, no RTOS header, no libc beyond <stdint.h>/<stddef.h>.
*   2. No heap.  Every table lives in a statically sized array; sizes are
*      compile time constants in ark_vfs_config.h.
*   3. No blocking primitive baked in.  If two threads can touch the same
*      fd table, hand in a lock through ark_vfs_hooks_t - otherwise there is
*      no lock at all and the linker does not pull one in.
*   4. Filesystems are plugins.  The core only knows "mount table + path
*      + fd"; anything that can implement ark_vfs_fsdrv_t becomes a
*      filesystem, including a device node table (/dev).
*
* Layering:  application  ->  core (this file)  ->  fsdrv  ->  backend
*
*   - backend (ark_vfs_backend_t)   raw byte storage: read/write/erase + geometry
*   - fsdrv   (ark_vfs_fsdrv_t)     turns bytes into files: open/read/stat/...
*   - core                          paths, mount points, fd allocation
*
* A backend is optional: an in-memory filesystem needs no storage at all,
* and a device node table needs no bytes either.
*/
#ifndef __ARK_VFS_H__
#define __ARK_VFS_H__

#include <stdint.h>
#include <stddef.h>

#include "ark_vfs_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * Types and error codes
 * ============================================================ */

typedef int32_t  ark_ssize_t;
typedef uint32_t ark_size_t;
typedef int64_t  ark_off_t;

/** @brief Error codes.  All errors are negative so 0 stays "success".
 *
 *  Deliberately NOT errno values: this library runs where errno does not
 *  exist, and it must not silently change meaning on a host that has it.
 *  Map them at the port layer if you need POSIX errno. */
enum {
    ARK_VFS_OK           =   0,
    ARK_E_NOENT          = -1,   /* nothing mounted at/along this path, or no such file */
    ARK_E_INVAL          = -2,   /* bad argument: NULL, empty path, too long */
    ARK_E_FULL           = -3,   /* fd table, mount table or filesystem storage is full */
    ARK_E_BADF           = -4,   /* fd is not open */
    ARK_E_ISDIR          = -5,
    ARK_E_NOTDIR         = -6,
    ARK_E_EXIST          = -7,
    ARK_E_NOTEMPTY       = -8,
    ARK_E_ROFS           = -9,   /* mounted read-only, or the fs is read-only */
    ARK_E_NOSPC          = -10,
    ARK_E_IO             = -11,  /* the backend said no */
    ARK_E_NOSYS          = -12,  /* this filesystem does not implement that call */
    ARK_E_NAMETOOLONG    = -13,
    ARK_E_LOOP            = -14,  /* path had too many '..' for its own depth */
    ARK_E_BUSY            = -15   /* e.g. unmounting while files are still open */
};

/** @brief Open flags (bit mask). */
enum {
    ARK_O_RDONLY   = 0x0000,
    ARK_O_WRONLY   = 0x0001,
    ARK_O_RDWR     = 0x0002,
    ARK_O_ACCMODE  = 0x0003,
    ARK_O_CREAT    = 0x0100,
    ARK_O_TRUNC    = 0x0200,
    ARK_O_APPEND   = 0x0400,
    ARK_O_DIRECTORY= 0x0800
};

/** @brief Seek origins. */
enum {
    ARK_SEEK_SET = 0,
    ARK_SEEK_CUR = 1,
    ARK_SEEK_END = 2
};

/** @brief File type bits in ark_vfs_stat_t.mode. */
enum {
    ARK_S_IFDIR = 0x4000,
    ARK_S_IFREG = 0x8000,
    ARK_S_IFCHR = 0x2000     /* a device node - what /dev entries are */
};

/** @brief Filesystem capability bits an fsdrv declares. */
enum {
    ARK_FS_CAP_READ    = 0x0001,   /* read() works              */
    ARK_FS_CAP_WRITE   = 0x0002,   /* write() works             */
    ARK_FS_CAP_CREATE  = 0x0004,   /* O_CREAT / write_file      */
    ARK_FS_CAP_UNLINK  = 0x0008,
    ARK_FS_CAP_MKDIR   = 0x0010,
    ARK_FS_CAP_RENAME  = 0x0020,
    ARK_FS_CAP_DIR     = 0x0040,   /* opendir/readdir           */
    ARK_FS_CAP_STAT    = 0x0080,
    ARK_FS_CAP_TRUNC   = 0x0100
};

struct ark_vfs_stat {
    uint32_t mode;          /* ARK_S_IFxxx */
    ark_off_t size;
    uint32_t mtime;         /* seconds, 0 if the fs keeps no time */
};

struct ark_vfs_dirent {
    char     name[ARK_VFS_NAME_MAX];
    uint32_t mode;
    ark_off_t size;
};

struct ark_vfs_mnt;         /* mount point, private to the core */
struct ark_vfs_file;        /* open file, private to the filesystem */

/* ============================================================
 * Backend: raw byte storage
 *
 * An fsdrv reads and writes through this.  All offsets are relative to the
 * start of the backend's own address space, so a partition is just a
 * backend whose window starts somewhere inside a bigger chip.
 * ============================================================ */
typedef struct ark_vfs_backend {
    const char *name;
    uint32_t    size;              /* total bytes; 0 is invalid */
    uint32_t    erase_size;        /* smallest erasable unit, 0 if not flash */

    /** @brief Read @p len bytes at @p off.  Returns bytes read, or <0. */
    int32_t  (*read)(struct ark_vfs_backend *be, uint32_t off, void *buf, uint32_t len);
    /** @brief Write @p len bytes at @p off.  Returns bytes written, or <0. */
    int32_t  (*write)(struct ark_vfs_backend *be, uint32_t off, const void *buf, uint32_t len);
    /** @brief Erase the unit(s) covering [off, off+len).  May be NULL for RAM. */
    int32_t  (*erase)(struct ark_vfs_backend *be, uint32_t off, uint32_t len);
    /** @brief Called once at mount so the backend can power up whatever it is. */
    int32_t  (*open)(struct ark_vfs_backend *be);
    /** @brief Called once at umount. */
    void     (*close)(struct ark_vfs_backend *be);

    void    *priv;                 /* backend's own data */
} ark_vfs_backend_t;

/* ============================================================
 * Filesystem driver: bytes -> files
 *
 * Every callback that takes a path receives it relative to the mount point
 * and already normalised by the core: no leading '/', no '.'/'..', no
 * doubled separators.  The mount root itself is "".  That is the entire
 * contract a filesystem author has to honour - it never sees a mount point,
 * never assembles paths, never parses anything.
 *
 * Implement only what you support and leave the rest NULL: the core turns a
 * NULL callback into ARK_E_NOSYS instead of a crash.
 * ============================================================ */
typedef struct ark_vfs_fsdrv {
    const char *name;
    uint32_t    caps;              /* ARK_FS_CAP_xxx */

    /** @brief Bring the filesystem up.  Fill in mnt->fs_priv. */
    int32_t  (*mount)(struct ark_vfs_mnt *mnt);
    /** @brief Tear down.  mnt->fs_priv must be considered invalid afterwards. */
    int32_t  (*umount)(struct ark_vfs_mnt *mnt);

    int32_t  (*open)(struct ark_vfs_mnt *mnt, const char *rel, int flags,
                     struct ark_vfs_file **out);
    int32_t  (*close)(struct ark_vfs_mnt *mnt, struct ark_vfs_file *f);
    ark_ssize_t (*read)(struct ark_vfs_file *f, void *buf, ark_size_t n);
    ark_ssize_t (*write)(struct ark_vfs_file *f, const void *buf, ark_size_t n);
    /** @brief Seek.  Filesystems without random access may leave this NULL. */
    int32_t  (*seek)(struct ark_vfs_file *f, ark_off_t off, int whence);

    int32_t  (*stat)(struct ark_vfs_mnt *mnt, const char *rel, struct ark_vfs_stat *st);
    int32_t  (*unlink)(struct ark_vfs_mnt *mnt, const char *rel);
    int32_t  (*mkdir)(struct ark_vfs_mnt *mnt, const char *rel);
    int32_t  (*rename)(struct ark_vfs_mnt *mnt, const char *old_rel,
                       const char *new_rel);
    int32_t  (*opendir)(struct ark_vfs_mnt *mnt, const char *rel,
                        struct ark_vfs_file **out);
    int32_t  (*readdir)(struct ark_vfs_file *d, struct ark_vfs_dirent *ent);
    int32_t  (*closedir)(struct ark_vfs_mnt *mnt, struct ark_vfs_file *d);
} ark_vfs_fsdrv_t;

/** @brief A mount point (also the handle a filesystem gets back). */
struct ark_vfs_mnt {
    char        target[ARK_VFS_PATH_MAX];   /* normalised, starts with '/', no trailing '/' */
    const ark_vfs_fsdrv_t  *fs;
    ark_vfs_backend_t      *be;             /* may be NULL */
    uint8_t     flags;                      /* ARK_MNT_RO */
    void       *fs_priv;                    /* owned by the filesystem */
    void       *user;                       /* whatever mount() was given */
};

enum {
    ARK_MNT_RO = 0x01          /* read-only: write/create/unlink refused by the core */
};

/* ============================================================
 * Port hooks: everything the core cannot do by itself
 *
 * Leave the struct zeroed and the core will run bare: no locking, no time,
 * no output.  Every hook is optional.
 * ============================================================ */
typedef struct ark_vfs_hooks {
    /** @brief Take a recursive lock.  Return a token passed back to unlock. */
    void *(*lock)(void *user);
    /** @brief Release the lock taken by lock(). */
    void  (*unlock)(void *user, void *token);

    /** @brief Milliseconds since boot, for mtime bookkeeping.  0 = no clock. */
    uint32_t (*now_ms)(void *user);

    /** @brief One line of diagnostics.  Called for warnings only. */
    void  (*log)(void *user, const char *fmt, ...);

    void  *user;
} ark_vfs_hooks_t;

/* ============================================================
 * Public API
 * ============================================================ */

/** @brief Reset the mount table and the fd table, then install hooks.
 *
 *  Must be called before anything else.  Hooks may be NULL. */
void ark_vfs_init(const ark_vfs_hooks_t *hooks);

/** @brief Unmount everything still mounted and forget the hooks.
 *
 *  Idempotent: a second call is a no-op, not a second sweep over the mount
 *  table.  The flag that makes it so is the same one ark_vfs_is_ready()
 *  reports. */
void ark_vfs_deinit(void);

/** @brief 1 if ark_vfs_init() has run and ark_vfs_deinit() has not.
 *
 *  This is the honest answer to "is this thing usable yet": a caller that
 *  just mounted something should not also have to keep its own copy of a
 *  flag the core already keeps. */
int32_t ark_vfs_is_ready(void);

/**
* @brief Mount @p fs at @p target.
*
* @param source  free-form source name, kept for listing ("nor0", "ram", ...).
*                May be NULL.
* @param target  mount point, "/" or "/dev" or "/mnt/nor".  Normalised by the
*                core; a trailing '/' is dropped.  An existing mount at the
*                same point is refused with ARK_E_EXIST (umount it first).
* @param fs      filesystem driver.  NULL is refused - use a devfs-like
*                driver for device nodes, it keeps the core free of special
*                cases.
* @param be      storage backend, or NULL for filesystems that need no bytes.
* @param flags   ARK_MNT_RO for read-only.
* @param user    opaque pointer handed to fsdrv through mnt->user.
* @return 0, or a negative error code.
*/
int32_t ark_vfs_mount(const char *source, const char *target,
                      const ark_vfs_fsdrv_t *fs, ark_vfs_backend_t *be,
                      uint32_t flags, void *user);

/** @brief Unmount @p target.  Refused with ARK_E_BADF if files are still open under it. */
int32_t ark_vfs_umount(const char *target);

/** @brief Look up a mount point.  @return mount or NULL. */
struct ark_vfs_mnt *ark_vfs_find_mount(const char *target);

/** @brief Visit every mount point.  Pass NULL to start; keep calling until NULL.
 *
 *  @code
 *  struct ark_vfs_mnt *m = NULL;
 *  while ((m = ark_vfs_next_mount(m)) != NULL) { ... }
 *  @endcode
 *  Safe against concurrent umount of the visited entry: the walker keeps an
 *  index, not a pointer. */
struct ark_vfs_mnt *ark_vfs_next_mount(struct ark_vfs_mnt *prev);

int32_t ark_vfs_open(const char *path, int flags);
int32_t ark_vfs_close(int fd);
ark_ssize_t ark_vfs_read(int fd, void *buf, ark_size_t n);
ark_ssize_t ark_vfs_write(int fd, const void *buf, ark_size_t n);
int32_t ark_vfs_lseek(int fd, ark_off_t off, int whence);

int32_t ark_vfs_stat(const char *path, struct ark_vfs_stat *st);
int32_t ark_vfs_mkdir(const char *path);
int32_t ark_vfs_unlink(const char *path);
int32_t ark_vfs_rename(const char *old_path, const char *new_path);

int32_t ark_vfs_opendir(const char *path);
int32_t ark_vfs_readdir(int fd, struct ark_vfs_dirent *ent);
int32_t ark_vfs_closedir(int fd);

/** @brief Read a whole file into @p buf (size @p max).  Writes the size read. */
int32_t ark_vfs_read_all(const char *path, void *buf, ark_size_t max,
                         ark_size_t *out_len);
/** @brief Write a whole file, creating or truncating it. */
int32_t ark_vfs_write_all(const char *path, const void *buf, ark_size_t len);
/** @brief 1 if the path exists (file or directory), 0 if not, <0 on a real error. */
int32_t ark_vfs_exists(const char *path);

/**
* @brief Normalise @p path into @p out.
*
* Handles "//", ".", "..", and a missing leading '/'.  ".." that would walk
* above the root is ARK_E_LOOP, not silently ignored - a path that escapes
* its own root is a caller bug worth reporting.
* @return 0 or a negative error code.  ARK_E_NAMETOOLONG if it does not fit.
*/
int32_t ark_vfs_normalize(const char *path, char *out, ark_size_t out_size);

/**
* @brief Resolve @p path to a mount point and the path relative to it.
*
* @param out_mnt  receives the mount point, or NULL if you only want the path.
* @param rel      buffer of at least ARK_VFS_PATH_MAX for the relative path
*                 ("sub/dir/file", "" at the mount root).
* @return 0 or a negative error code.  ARK_E_NOENT when no mount covers it.
*
* Longest-prefix wins, so a "/mnt/nor" mount shadows "/" for paths under it,
* which is what everyone expects from a mount point.
*/
int32_t ark_vfs_resolve(const char *path, struct ark_vfs_mnt **out_mnt,
                        char *rel);

/** @brief Static information, for diagnostics and test assertions. */
const char *ark_vfs_version(void);
const char *ark_vfs_error_name(int32_t err);
ark_size_t  ark_vfs_open_count(void);

#ifdef __cplusplus
}
#endif

#endif /* __ARK_VFS_H__ */
