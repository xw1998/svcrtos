/**
* @file svcrt_fs.c
* @brief littlefs port + SVCrtOS file system facade - see svcrt_fs.h
* @details Structure of this file:
*            1) the three board callbacks littlefs needs (read/prog/erase),
*               each one a translation from "block + offset" into a call on a
*               registered svcrt_blk device;
*            2) mount/unmount/format, which build the lfs_config from the
*               device's own numbers instead of hard-coded constants;
*            3) the thin file API used by the shell.
*
*          There is no caching layer of our own anywhere: littlefs already
*          caches, and a second cache would only add a way for the two to
*          disagree. One erase unit = one littlefs block, so a "block erase"
*          is exactly one device erase.
*
* @author xw
* @date 2026.09.21
*/

#include "svcrt_fs.h"
#include "svcrt_blk.h"
#include "svcrt_partition.h"

#include "lfs.h"
#include "lfs_svcrt_config.h"   /* asserts the LFS_NO_* build switches */

/* ============================================================
 * Volume + static buffers
 *
 * All of this is .bss: littlefs is built with LFS_NO_MALLOC, so every buffer
 * it uses has to exist before it is mounted.
 * ============================================================ */
#define SVCRT_FS_LOOKAHEAD_SIZE  (16u)
#define SVCRT_FS_NAME_MAX        (32u)
#define SVCRT_FS_FILE_MAX        (8u * 1024u * 1024u)
#define SVCRT_FS_ATTR_MAX        (64u)
#define SVCRT_FS_BLOCK_CYCLES    (500)   /* wear levelling period, in erases */

/* Error codes of our own. Anything at or below -1000 belongs to SVCrtOS, and
 * anything above it is a value littlefs returned as-is - that split is what
 * lets a reader tell "the file system said this" from "the facade said this"
 * without looking anything up. Do NOT dress our own conditions in littlefs
 * numbers: a "-1" claiming to be LFS_ERR_IO would be read as a device error,
 * while the real LFS_ERR_IO is -5. */
#define SVCRT_FS_ERR_ARGS        (-1001)
#define SVCRT_FS_ERR_NO_DEV      (-1002)
#define SVCRT_FS_ERR_NO_VOLUME   (-1003)
#define SVCRT_FS_ERR_MOUNTED     (-1004)
#define SVCRT_FS_ERR_NOT_MOUNTED (-1005)
#define SVCRT_FS_ERR_BLOCK       (-1006)
#define SVCRT_FS_ERR_SHORT       (-1007)
#define SVCRT_FS_ERR_BUSY        (-1008)

/* Values of g_fs.stream_mode */
#define SVCRT_FS_STREAM_NONE     (0u)
#define SVCRT_FS_STREAM_READ     (1u)
#define SVCRT_FS_STREAM_WRITE    (2u)

static struct
{
    const svcrt_blk_dev_t *dev;
    uint32                 offset;
    uint32                 size;
    uint32                 block_size;
    uint32                 block_count;

    lfs_t                  lfs;
    struct lfs_config      cfg;

    /* read and prog caches must be distinct: littlefs keeps both live at
     * once (it reads a block while building the next one to program), so
     * sharing one buffer would corrupt the block being written. */
    uint8                  read_cache[SVCRT_FS_CACHE_SIZE];
    uint8                  prog_cache[SVCRT_FS_CACHE_SIZE];
    uint8                  lookahead[SVCRT_FS_LOOKAHEAD_SIZE];
    uint8                  file_cache[SVCRT_FS_CACHE_SIZE];

    uint8                  mounted;
    int32                  last_error;

    /* Streaming file (svcrt_fs_open_read / svcrt_fs_open_write): littlefs
     * needs the object to stay alive between calls, and the facade allows
     * one stream at a time. */
    lfs_file_t             stream;
    uint8                  stream_mode;
} g_fs;

/* Forward declaration: unmount drops a stream before it forgets the
 * volume, and the streaming block is written further down this file. */
static int32 fs_stream_close(void);

int32 svcrt_fs_last_error(void)
{
    return g_fs.last_error;
}

const char *svcrt_fs_error_name(int32 e)
{
    /* Names come from the enum in lfs.h, never from a numbering of our own:
     * littlefs uses the POSIX errno values (LFS_ERR_CORRUPT is -84, not -2),
     * so a hand-written table would have confidently renamed every error the
     * day a value moved. */
    switch(e)
    {
        case 0:                       return "ok";
        case LFS_ERR_IO:              return "LFS_ERR_IO";
        case LFS_ERR_CORRUPT:         return "LFS_ERR_CORRUPT";
        case LFS_ERR_NOENT:           return "LFS_ERR_NOENT";
        case LFS_ERR_EXIST:           return "LFS_ERR_EXIST";
        case LFS_ERR_NOTDIR:          return "LFS_ERR_NOTDIR";
        case LFS_ERR_ISDIR:           return "LFS_ERR_ISDIR";
        case LFS_ERR_NOTEMPTY:        return "LFS_ERR_NOTEMPTY";
        case LFS_ERR_BADF:            return "LFS_ERR_BADF";
        case LFS_ERR_FBIG:            return "LFS_ERR_FBIG";
        case LFS_ERR_INVAL:           return "LFS_ERR_INVAL";
        case LFS_ERR_NOSPC:           return "LFS_ERR_NOSPC";
        case LFS_ERR_NOMEM:           return "LFS_ERR_NOMEM";
        case LFS_ERR_NOATTR:          return "LFS_ERR_NOATTR";
        case LFS_ERR_NAMETOOLONG:     return "LFS_ERR_NAMETOOLONG";

        /* SVCrtOS's own, see the block at the top of this file */
        case SVCRT_FS_ERR_ARGS:        return "SVCrtOS: bad arguments or volume geometry";
        case SVCRT_FS_ERR_NO_DEV:      return "SVCrtOS: no such block device";
        case SVCRT_FS_ERR_NO_VOLUME:   return "SVCrtOS: no volume in this board layout";
        case SVCRT_FS_ERR_MOUNTED:     return "SVCrtOS: a volume is already mounted";
        case SVCRT_FS_ERR_NOT_MOUNTED: return "SVCrtOS: nothing is mounted";
        case SVCRT_FS_ERR_BLOCK:       return "SVCrtOS: the block device refused it";
        case SVCRT_FS_ERR_SHORT:       return "SVCrtOS: short read/write";
        case SVCRT_FS_ERR_BUSY:        return "SVCrtOS: a file stream is already open";

        default:                      return "unknown";
    }
}

/* ============================================================
 * Board callbacks
 *
 * Note what is NOT here: no retry loop. A failing block read means the SPI
 * bus or the chip is in trouble, and littlefs handles it by reporting the
 * error upward. Retrying inside the callback would hide exactly the signal
 * the caller needs.
 * ============================================================ */
static int fs_block_read(const struct lfs_config *c, lfs_block_t block,
                         lfs_off_t off, void *buffer, lfs_size_t size)
{
    uint32 addr;

    (void)c;

    if(block >= g_fs.block_count)
    {
        g_fs.last_error = SVCRT_FS_ERR_BLOCK;
        return LFS_ERR_IO;
    }

    addr = g_fs.offset + ((uint32)block * g_fs.block_size) + (uint32)off;

    if(svcrt_blk_read(g_fs.dev, addr, (uint8 *)buffer, (uint32)size) != 0)
    {
        g_fs.last_error = SVCRT_FS_ERR_BLOCK;
        return LFS_ERR_IO;
    }

    return 0;
}

static int fs_block_prog(const struct lfs_config *c, lfs_block_t block,
                         lfs_off_t off, const void *buffer, lfs_size_t size)
{
    uint32 addr;

    (void)c;

    if(block >= g_fs.block_count)
    {
        g_fs.last_error = SVCRT_FS_ERR_BLOCK;
        return LFS_ERR_IO;
    }

    addr = g_fs.offset + ((uint32)block * g_fs.block_size) + (uint32)off;

    /* The target range has already been erased by littlefs; svcrt_blk_write
     * cannot check that, so a programming failure here means the device
     * refused (range, timeout) and is reported as-is. */
    if(svcrt_blk_write(g_fs.dev, addr, (const uint8 *)buffer, (uint32)size) != 0)
    {
        g_fs.last_error = SVCRT_FS_ERR_BLOCK;
        return LFS_ERR_IO;
    }

    return 0;
}

static int fs_block_erase(const struct lfs_config *c, lfs_block_t block)
{
    uint32 addr;

    (void)c;

    if(block >= g_fs.block_count)
    {
        g_fs.last_error = SVCRT_FS_ERR_BLOCK;
        return LFS_ERR_IO;
    }

    addr = g_fs.offset + ((uint32)block * g_fs.block_size);

    if(svcrt_blk_erase(g_fs.dev, addr, g_fs.block_size) != 0)
    {
        g_fs.last_error = SVCRT_FS_ERR_BLOCK;
        return LFS_ERR_IO;
    }

    return 0;
}

/* littlefs calls cfg->sync unconditionally from lfs_bd_sync(), so this hook
 * must never be NULL: a NULL here becomes a tail call to address 0 and raises
 * UsageFault (INVSTATE). The NOR sits on a blocking SPI link, so a completed
 * program is already on the medium and there is nothing left to flush. */
static int fs_block_sync(const struct lfs_config *c)
{
    (void)c;
    return 0;
}

/* ============================================================
 * Volume setup
 *
 * Fills g_fs.cfg from the device itself. Nothing about the medium is
 * hard-coded: block size comes from the device's erase unit, block count from
 * its capacity. A board with a bigger NOR or a different internal sector size
 * needs no change here.
 * ============================================================ */
static int32 fs_build_config(const char *dev_name, uint32 offset, uint32 size,
                             struct lfs_config *cfg,
                             uint8 *read_cache, uint8 *prog_cache,
                             uint8 *lookahead)
{
    const svcrt_blk_dev_t *dev = svcrt_blk_find(dev_name);
    uint32 dev_size;

    if(dev == 0)
    {
        g_fs.last_error = SVCRT_FS_ERR_NO_DEV;   /* no block device with that name */
        return -1;
    }

    if(svcrt_blk_init(dev) != 0)
    {
        g_fs.last_error = SVCRT_FS_ERR_BLOCK;
        return -1;
    }

    dev_size = svcrt_blk_size(dev);

    if(size == 0u)
    {
        size = dev_size - offset;
    }

    if((dev->erase_unit == 0u) || (size < dev->erase_unit) ||
       ((offset % dev->erase_unit) != 0u) || ((size % dev->erase_unit) != 0u) ||
       ((offset + size) > dev_size))
    {
        g_fs.last_error = SVCRT_FS_ERR_ARGS;   /* volume does not fit the device */
        return -1;
    }

    cfg->context         = (void *)dev;
    cfg->read            = fs_block_read;
    cfg->prog            = fs_block_prog;
    cfg->erase           = fs_block_erase;
    cfg->sync            = fs_block_sync;           /* must not be NULL, see fs_block_sync */
    cfg->read_size       = 16u;                     /* one SPI transaction */
    cfg->prog_size       = (dev->erase_unit >= 256u) ? 256u : dev->erase_unit;
    cfg->block_size      = dev->erase_unit;
    cfg->block_count     = size / dev->erase_unit;
    cfg->block_cycles    = SVCRT_FS_BLOCK_CYCLES;
    cfg->cache_size      = SVCRT_FS_CACHE_SIZE;
    cfg->lookahead_size  = SVCRT_FS_LOOKAHEAD_SIZE;
    cfg->read_buffer     = read_cache;
    cfg->prog_buffer     = prog_cache;
    cfg->lookahead_buffer = lookahead;
    cfg->name_max        = SVCRT_FS_NAME_MAX;
    cfg->file_max        = SVCRT_FS_FILE_MAX;
    cfg->attr_max        = SVCRT_FS_ATTR_MAX;

    /* littlefs requires the counts to be usable; refuse here rather than
     * letting it fail later with a less obvious error. */
    if((cfg->block_count < 3u) ||
       ((cfg->cache_size % cfg->prog_size) != 0) ||
       (cfg->prog_size > cfg->block_size))
    {
        g_fs.last_error = SVCRT_FS_ERR_ARGS;   /* geometry littlefs cannot use */
        return -1;
    }

    g_fs.dev         = dev;
    g_fs.offset      = offset;
    g_fs.size        = size;
    g_fs.block_size  = cfg->block_size;
    g_fs.block_count = cfg->block_count;

    return 0;
}

/* ============================================================
 * Mount / unmount / format
 * ============================================================ */
static int32 fs_mount_with(struct lfs_config *cfg)
{
    int err;

    err = lfs_mount(&g_fs.lfs, cfg);

    if(err != 0)
    {
        g_fs.last_error = err;
        return -1;
    }

    g_fs.mounted = 1u;
    g_fs.last_error = 0;

    return 0;
}

int32 svcrt_fs_mount(const char *dev, uint32 offset, uint32 size)
{
    if(dev == 0)
    {
        g_fs.last_error = SVCRT_FS_ERR_ARGS;
        return -1;
    }

    /* Mounting on top of a live mount would lose the old lfs_t and leak its
     * state; require an explicit unmount so the caller's intent is clear. */
    if(g_fs.mounted != 0u)
    {
        g_fs.last_error = SVCRT_FS_ERR_MOUNTED;   /* unmount first */
        return -1;
    }

    if(fs_build_config(dev, offset, size, &g_fs.cfg,
                       g_fs.read_cache, g_fs.prog_cache,
                       g_fs.lookahead) != 0)
    {
        return -1;
    }

    return fs_mount_with(&g_fs.cfg);
}

int32 svcrt_fs_mount_default(void)
{
    /* A board with no external NOR defines SVCRT_FS_SIZE as 0. "No volume
     * configured" is the honest answer there, and asking the preprocessor
     * keeps the call from being compiled at all: a run-time check would leave
     * a mount attempt against a device the board never registered sitting in
     * the image, which is both unreachable (AC5 #111-D) and misleading to
     * read. */
#if (SVCRT_FS_SIZE == 0u)
    g_fs.last_error = SVCRT_FS_ERR_NO_VOLUME;   /* see the partition header */
    return -1;
#else
    return svcrt_fs_mount(SVCRT_FS_DEV_NAME, (uint32)SVCRT_FS_BASE,
                          (uint32)SVCRT_FS_SIZE);
#endif
}

int32 svcrt_fs_unmount(void)
{
    int err;

    if(g_fs.mounted == 0u)
    {
        return 0;
    }

    /* An open stream describes blocks this unmount is about to forget.
     * Drop it first: refusing here would strand a caller that has no way
     * left to close it. */
    (void)fs_stream_close();

    err = lfs_unmount(&g_fs.lfs);

    if(err != 0)
    {
        g_fs.last_error = err;   /* report what littlefs said, not "failed" */
        return -1;
    }

    g_fs.mounted = 0u;

    return 0;
}

uint8 svcrt_fs_mounted(void)
{
    return g_fs.mounted;
}

int32 svcrt_fs_format(const char *dev, uint32 offset, uint32 size)
{
    struct lfs_config fmt_cfg;
    lfs_t              fmt_lfs;
    int                err;

    if(dev == 0)
    {
        g_fs.last_error = SVCRT_FS_ERR_ARGS;
        return -1;
    }

    /* Formatting a mounted volume would leave the mounted lfs_t describing
     * blocks that no longer exist; unmount first. */
    if(g_fs.mounted != 0u)
    {
        g_fs.last_error = SVCRT_FS_ERR_MOUNTED;   /* unmount first */
        return -1;
    }

    /* Safe to reuse the mount buffers: formatting is refused while a volume
     * is mounted, so nothing else is holding them. */
    if(fs_build_config(dev, offset, size, &fmt_cfg,
                       g_fs.read_cache, g_fs.prog_cache,
                       g_fs.lookahead) != 0)
    {
        return -1;
    }

    err = lfs_format(&fmt_lfs, &fmt_cfg);

    if(err != 0)
    {
        g_fs.last_error = err;
        return -1;
    }

    /* A freshly formatted volume is what the caller almost always wants to
     * use next, so mount it - through the normal path, which keeps the
     * g_fs state consistent whether or not this call fails. */
    return svcrt_fs_mount(dev, offset, size);
}

/* ============================================================
 * File API
 * ============================================================ */
static int32 fs_check_path(const char *path)
{
    if((path == 0) || (path[0] != '/'))
    {
        g_fs.last_error = SVCRT_FS_ERR_ARGS;
        return -1;
    }

    return 0;
}

int32 svcrt_fs_stat(uint32 *total, uint32 *used)
{
    lfs_ssize_t blocks;

    if(g_fs.mounted == 0u)
    {
        g_fs.last_error = SVCRT_FS_ERR_NOT_MOUNTED;
        return -1;
    }

    blocks = lfs_fs_size(&g_fs.lfs);

    if(blocks < 0)
    {
        g_fs.last_error = (int32)blocks;
        return -1;
    }

    if(total != 0)
    {
        *total = g_fs.block_count * g_fs.block_size;
    }

    if(used != 0)
    {
        *used = (uint32)blocks * g_fs.block_size;
    }

    return 0;
}

/**
 * @brief Size and type of one entry (file or directory).
 * @param path   absolute path inside the mounted volume
 * @param size   receives the size in bytes, 0 for a directory (may be NULL)
 * @param is_dir receives 1 for a directory, 0 for a file (may be NULL)
 * @return 0 on success, -1 on failure (nothing mounted, bad path, not found)
 */
int32 svcrt_fs_stat_path(const char *path, uint32 *size, uint32 *is_dir)
{
    struct lfs_info info;
    int             err;

    if((g_fs.mounted == 0u) || (fs_check_path(path) != 0))
    {
        return -1;
    }

    err = lfs_stat(&g_fs.lfs, path, &info);

    if(err != 0)
    {
        g_fs.last_error = err;
        return -1;
    }

    if(size != 0)
    {
        *size = (uint32)info.size;
    }

    if(is_dir != 0)
    {
        *is_dir = (info.type == LFS_TYPE_DIR) ? 1u : 0u;
    }

    return 0;
}

int32 svcrt_fs_write_file(const char *path, const uint8 *data, uint32 len)
{
    struct lfs_file_config fcfg;
    lfs_file_t              file;
    int                     err;

    if((g_fs.mounted == 0u) || (data == 0) || (fs_check_path(path) != 0))
    {
        return -1;
    }

    /* One file cache, one user at a time: see svcrt_fs_open_read. */
    if(g_fs.stream_mode != SVCRT_FS_STREAM_NONE)
    {
        g_fs.last_error = SVCRT_FS_ERR_BUSY;
        return -1;
    }

    fcfg.buffer = g_fs.file_cache;
    fcfg.attrs  = 0;
    fcfg.attr_count = 0;

    err = lfs_file_opencfg(&g_fs.lfs, &file, path,
                           LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC, &fcfg);

    if(err != 0)
    {
        g_fs.last_error = err;
        return -1;
    }

    if(lfs_file_write(&g_fs.lfs, &file, data, len) != (lfs_ssize_t)len)
    {
        g_fs.last_error = SVCRT_FS_ERR_SHORT;   /* short write */
        (void)lfs_file_close(&g_fs.lfs, &file);
        return -1;
    }

    err = lfs_file_close(&g_fs.lfs, &file);

    if(err != 0)
    {
        g_fs.last_error = err;
        return -1;
    }

    return 0;
}

int32 svcrt_fs_read_file(const char *path, uint8 *buf, uint32 max, uint32 *out_len)
{
    struct lfs_file_config fcfg;
    lfs_file_t              file;
    lfs_ssize_t             n;
    int                     err;

    if((g_fs.mounted == 0u) || (buf == 0) || (fs_check_path(path) != 0))
    {
        return -1;
    }

    /* A stream in flight owns the file cache: see svcrt_fs_open_read. */
    if(g_fs.stream_mode != SVCRT_FS_STREAM_NONE)
    {
        g_fs.last_error = SVCRT_FS_ERR_BUSY;
        return -1;
    }

    fcfg.buffer = g_fs.file_cache;
    fcfg.attrs  = 0;
    fcfg.attr_count = 0;

    err = lfs_file_opencfg(&g_fs.lfs, &file, path, LFS_O_RDONLY, &fcfg);

    if(err != 0)
    {
        g_fs.last_error = err;
        return -1;
    }

    n = lfs_file_read(&g_fs.lfs, &file, buf, max);

    if(n < 0)
    {
        g_fs.last_error = (int32)n;
        (void)lfs_file_close(&g_fs.lfs, &file);
        return -1;
    }

    if(out_len != 0)
    {
        *out_len = (uint32)n;
    }

    err = lfs_file_close(&g_fs.lfs, &file);

    if(err != 0)
    {
        g_fs.last_error = err;
        return -1;
    }

    return 0;
}

/* ============================================================
 * Streaming file access
 *
 * A .svcapp image is far larger than the shell's 512-byte scratch, and
 * staging one in kernel RAM would cost more than the kernel has to spare. So
 * one file stays open across calls and the caller drives it in chunks.
 *
 * One stream at a time, on purpose: littlefs is single threaded and the
 * facade owns exactly one file cache. A second stream would either share that
 * cache (corrupting the first) or grow the RAM budget, and no caller in the
 * kernel needs two at once.
 * ============================================================ */
static int32 fs_stream_close(void)
{
    int err;

    if(g_fs.stream_mode == SVCRT_FS_STREAM_NONE)
    {
        return 0;
    }

    err = lfs_file_close(&g_fs.lfs, &g_fs.stream);
    g_fs.stream_mode = SVCRT_FS_STREAM_NONE;

    if(err != 0)
    {
        g_fs.last_error = err;
        return -1;
    }

    return 0;
}

/* Shared body of open_read / open_write: only the littlefs flags differ. */
static int32 fs_stream_open(const char *path, uint32 mode, uint32 flags)
{
    struct lfs_file_config fcfg;
    int err;

    if((g_fs.mounted == 0u) || (fs_check_path(path) != 0))
    {
        return -1;
    }

    if(g_fs.stream_mode != SVCRT_FS_STREAM_NONE)
    {
        g_fs.last_error = SVCRT_FS_ERR_BUSY;   /* close the other one first */
        return -1;
    }

    fcfg.buffer     = g_fs.file_cache;
    fcfg.attrs      = 0;
    fcfg.attr_count = 0;

    err = lfs_file_opencfg(&g_fs.lfs, &g_fs.stream, path, (int)flags, &fcfg);

    if(err != 0)
    {
        g_fs.last_error = err;
        return -1;
    }

    g_fs.stream_mode = (uint8)mode;
    return 0;
}

int32 svcrt_fs_open_read(const char *path)
{
    return fs_stream_open(path, SVCRT_FS_STREAM_READ, LFS_O_RDONLY);
}

int32 svcrt_fs_read_next(uint8 *buf, uint32 max)
{
    lfs_ssize_t n;

    if((g_fs.stream_mode != SVCRT_FS_STREAM_READ) || (buf == 0) || (max == 0u))
    {
        g_fs.last_error = SVCRT_FS_ERR_ARGS;
        return -1;
    }

    n = lfs_file_read(&g_fs.lfs, &g_fs.stream, buf, max);

    if(n < 0)
    {
        g_fs.last_error = (int32)n;   /* pass littlefs's own code through */
        return -1;
    }

    return (int32)n;
}

int32 svcrt_fs_close_read(void)
{
    if(g_fs.stream_mode != SVCRT_FS_STREAM_READ)
    {
        return 0;
    }

    return fs_stream_close();
}

int32 svcrt_fs_open_write(const char *path)
{
    return fs_stream_open(path, SVCRT_FS_STREAM_WRITE,
                          LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
}

int32 svcrt_fs_write_next(const uint8 *buf, uint32 len)
{
    if((g_fs.stream_mode != SVCRT_FS_STREAM_WRITE) || (buf == 0))
    {
        g_fs.last_error = SVCRT_FS_ERR_ARGS;
        return -1;
    }

    if(lfs_file_write(&g_fs.lfs, &g_fs.stream, buf, len) != (lfs_ssize_t)len)
    {
        g_fs.last_error = SVCRT_FS_ERR_SHORT;
        return -1;
    }

    return 0;
}

int32 svcrt_fs_close_write(void)
{
    if(g_fs.stream_mode != SVCRT_FS_STREAM_WRITE)
    {
        return 0;
    }

    return fs_stream_close();
}

int32 svcrt_fs_remove(const char *path)
{
    int err;

    if((g_fs.mounted == 0u) || (fs_check_path(path) != 0))
    {
        return -1;
    }

    err = lfs_remove(&g_fs.lfs, path);

    if(err != 0)
    {
        g_fs.last_error = err;
        return -1;
    }

    return 0;
}

int32 svcrt_fs_list(const char *dir, svcrt_fs_list_cb_t cb, void *arg)
{
    lfs_dir_t ditem;
    struct lfs_info info;
    int err;

    if((g_fs.mounted == 0u) || (cb == 0))
    {
        g_fs.last_error = SVCRT_FS_ERR_NOT_MOUNTED;
        return -1;
    }

    if(dir == 0)
    {
        dir = "/";
    }

    if(fs_check_path(dir) != 0)
    {
        return -1;
    }

    err = lfs_dir_open(&g_fs.lfs, &ditem, dir);

    if(err != 0)
    {
        g_fs.last_error = err;
        return -1;
    }

    for(;;)
    {
        err = lfs_dir_read(&g_fs.lfs, &ditem, &info);

        if(err < 0)
        {
            g_fs.last_error = err;
            (void)lfs_dir_close(&g_fs.lfs, &ditem);
            return -1;
        }

        if(err == 0)
        {
            break;      /* end of directory */
        }

        if(cb(info.name, (uint32)info.size,
              (info.type == LFS_TYPE_DIR) ? 1u : 0u, arg) != 0)
        {
            break;      /* caller asked to stop - not an error */
        }
    }

    err = lfs_dir_close(&g_fs.lfs, &ditem);

    if(err != 0)
    {
        g_fs.last_error = err;
        return -1;
    }

    return 0;
}
