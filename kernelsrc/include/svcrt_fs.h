/**
* @file svcrt_fs.h
* @brief SVCrtOS file system facade - littlefs over a registered block device
* @details One volume at a time, mounted explicitly. That is a deliberate
*          limit, not an oversight: the kernel has one caller (the shell today,
*          an SVC handler later) and a single mount point keeps the RAM cost
*          (two caches + lookahead, all static) and the failure modes small.
*          Multiple volumes can be added later by turning g_fs into an array.
*
*          The volume is described by three numbers - device name, offset,
*          length - so the same code mounts a file system on the external NOR,
*          on the internal flash pool, or on anything a future board registers
*          with svcrt_blk. Offset 0 of the volume is offset `offset` of the
*          device: the FS never sees absolute addresses.
*
*          littlefs is asked for one block = one erase unit of the underlying
*          device. That keeps "erase" a single device erase and avoids the
*          classic bug of programming a 4 KiB file block inside a 128 KiB
*          sector that a later erase wipes.
*
*          Every entry point returns 0 on success and -1 on failure. The raw
*          littlefs error is kept in svcrt_fs_last_error() so the caller can
*          report what the file system actually said instead of a guess.
*
* @author xw
* @date 2026.09.21
*/
#ifndef SVCRT_FS_H
#define SVCRT_FS_H

#include "svcrt_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Longest path accepted, including the leading '/' and the terminator */
#define SVCRT_FS_PATH_MAX   (64u)

/** Bytes buffered for metadata/reads and for one open file */
#define SVCRT_FS_CACHE_SIZE (256u)

/**
 * @brief Mount the volume described by (dev, offset, size).
 * @param dev    name of a registered block device, e.g. "nor0"
 * @param offset byte offset of the volume inside that device
 * @param size   volume length in bytes; 0 = to the end of the device
 * @return 0 on success, -1 on failure (not mounted, nothing in flash, or a
 *         block device that does not exist / is not up)
 * @note Mounting does NOT format. An unformatted volume fails here, which is
 *       the honest answer - use svcrt_fs_format() first, on purpose.
 */
int32 svcrt_fs_mount(const char *dev, uint32 offset, uint32 size);

/**
 * @brief Mount the board's default volume (see SVCRT_FS_* in the partition
 *        header). Returns -1 with last_error = -2 when no volume is configured.
 */
int32 svcrt_fs_mount_default(void);

/** @brief Drop the mount. Safe to call when nothing is mounted. */
int32 svcrt_fs_unmount(void);

/** @brief 1 while a volume is mounted */
uint8 svcrt_fs_mounted(void);

/**
 * @brief Erase and create an empty file system on the volume.
 * @note  Destructive: everything on the volume is lost. Works on a volume that
 *        is not mounted yet, and mounts it on success.
 */
int32 svcrt_fs_format(const char *dev, uint32 offset, uint32 size);

/** @brief Total and used bytes of the mounted volume (used = blocks in use) */
int32 svcrt_fs_stat(uint32 *total, uint32 *used);

/** @brief Create/truncate path and write len bytes */
/**
 * @brief Size and type of one entry (file or directory).
 * @param path   absolute path inside the mounted volume
 * @param size   receives the size in bytes, 0 for a directory (may be NULL)
 * @param is_dir receives 1 for a directory, 0 for a file (may be NULL)
 * @return 0 on success, -1 on failure (nothing mounted, bad path, not found)
 */
int32 svcrt_fs_stat_path(const char *path, uint32 *size, uint32 *is_dir);

int32 svcrt_fs_write_file(const char *path, const uint8 *data, uint32 len);

/** @brief Read at most max bytes; *out_len gets the real length */
int32 svcrt_fs_read_file(const char *path, uint8 *buf, uint32 max, uint32 *out_len);

/**
 * @brief Open one file for chunked (streaming) reading.
 * @details A 20 KiB image does not fit a fixed scratch buffer, so the file
 *          stays open across calls and the caller drives it. While a stream
 *          is open, the one-shot entry points that share the single file cache
 *          (svcrt_fs_read_file / svcrt_fs_write_file) refuse to run instead of
 *          silently corrupting whichever is in flight - that refusal is
 *          SVCRT_FS_ERR_BUSY.
 * @return 0 on success, -1 on failure (nothing mounted, bad path, not found,
 *         a directory, or another stream already open)
 */
int32 svcrt_fs_open_read(const char *path);

/**
 * @brief Read the next chunk of the open read stream.
 * @param buf destination, at least max bytes
 * @param max largest chunk to ask for
 * @return bytes read (1..max), 0 at end of file, -1 on failure
 * @note 0 means "the file ended", never "nothing right now": the call is
 *       synchronous, so a caller loop may stop on it.
 */
int32 svcrt_fs_read_next(uint8 *buf, uint32 max);

/** @brief Close the read stream. Closing when none is open is not an error. */
int32 svcrt_fs_close_read(void);

/**
 * @brief Open one file for chunked writing (creates or truncates).
 * @return 0 on success, -1 on failure; the rules of svcrt_fs_open_read apply
 */
int32 svcrt_fs_open_write(const char *path);

/** @brief Append one chunk to the open write stream. @return 0 on success */
int32 svcrt_fs_write_next(const uint8 *buf, uint32 len);

/**
 * @brief Close the write stream, which is what commits the file.
 * @return 0 on success, -1 on failure (littlefs reports the reason); closing
 *         when none is open is not an error
 */
int32 svcrt_fs_close_write(void);

/** @brief Remove a file. @return 0 on success, -1 (not found / is a dir) */
int32 svcrt_fs_remove(const char *path);

/** @brief Callback for svcrt_fs_list(); returns 0 to continue, -1 to stop */
typedef int (*svcrt_fs_list_cb_t)(const char *name, uint32 size, uint8 is_dir, void *arg);

/** @brief List one directory ("/" for the root) */
int32 svcrt_fs_list(const char *dir, svcrt_fs_list_cb_t cb, void *arg);

/**
 * @brief Error of the last failed call (0 when none)
 * @details Whatever littlefs returned is passed through unchanged; failures
 *          raised by this facade itself use codes <= -1000 (SVCRT_FS_ERR_*).
 *          Print the number, and svcrt_fs_error_name() adds the symbol.
 */
/** @brief Read len bytes starting at byte offset off (random access read).
 *  A partial read at end of file returns fewer bytes than len, with that
 *  number in *out_len - never an error. */
int32 svcrt_fs_read_at(const char *path, uint8 *buf, uint32 len, uint32 off,
                      uint32 *out_len);

/** @brief Move/rename one entry. Both paths are inside the volume. */
int32 svcrt_fs_rename(const char *old_path, const char *new_path);

/** @brief Copy the names of one directory into a caller buffer, one per line.
 *  Only complete entries are copied: when the buffer runs out, the last
 *  partial name is dropped and *count counts what actually fits, so the
 *  caller can tell a full listing from a truncated one. */
int32 svcrt_fs_list_names(const char *dir, char *out, uint32 out_size,
                         uint32 *count);

int32 svcrt_fs_last_error(void);

/**
 * @brief Human readable name of an error code, for reports.
 * @details Callers must print the number the file system returned, not a
 *          guessed cause; this only adds the short symbol on top of it.
 */
const char *svcrt_fs_error_name(int32 lfs_err);

#ifdef __cplusplus
}
#endif

#endif /* SVCRT_FS_H */
