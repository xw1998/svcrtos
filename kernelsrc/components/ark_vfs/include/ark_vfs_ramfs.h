/**
* @file ark_vfs_ramfs.h
* @brief The filesystem that ships with ark_vfs: RAM only, zero dependencies.
*
* It exists for two reasons: as a usable scratch filesystem (a shell's /tmp,
* a config area, a place to stage an image before flashing), and as a worked
* example of the fsdrv contract - about 750 lines, no allocator, no backend.
*
* One instance per build.  Everything lives in static arrays sized by the
* ARK_RAMFS_* macros in ark_vfs_ramfs.c.
*/
#ifndef __ARK_VFS_RAMFS_H__
#define __ARK_VFS_RAMFS_H__

#include "ark_vfs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The ramfs driver, ready to hand to ark_vfs_mount(). */
const ark_vfs_fsdrv_t *ark_ramfs_fs(void);

/** @brief Drop every file and directory.  Mounting again keeps old contents;
 *
 *  this is the explicit way to get a clean slate. */
void ark_ramfs_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* __ARK_VFS_RAMFS_H__ */
