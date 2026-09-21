/**
* @file lfs_svcrt_config.h
* @brief Compile-time guard for the littlefs build switches used by SVCrtOS
* @details littlefs is vendored verbatim in kernelsrc/fs/littlefs/. Its knobs
*          are NOT set here: upstream lfs_util.h, when LFS_CONFIG is defined,
*          emits none of its default utilities at all (LFS_ASSERT, LFS_MALLOC,
*          LFS_MIN, ... all live in the #else branch), so a small override
*          header cannot work without duplicating upstream - the supported
*          way, and the one the upstream Makefile uses, is to pass the
*          switches on the command line.
*
*          They are therefore set in the Keil project (Options for Target ->
*          C/C++ -> Define):
*            LFS_NO_MALLOC, LFS_NO_ASSERT, LFS_NO_DEBUG, LFS_NO_WARN,
*            LFS_NO_ERROR
*
*          Keeping the list in a header lets a translation unit assert that
*          it really got them. That matters most for LFS_NO_MALLOC: without
*          it, lfs_util.h includes <stdlib.h> and lfs.c calls malloc(), which
*          does not exist in this firmware - the failure would otherwise only
*          show up at link time, or worse, pull in a heap nobody manages.
*          LFS_NO_INTRINSICS is deliberately left alone: lfs_util.h then
*          picks the faster branch-free endian helpers for both AC5 and AC6.
*
* @author xw
* @date 2026.09.21
*/
#ifndef __LFS_SVCRT_CONFIG_H__
#define __LFS_SVCRT_CONFIG_H__

/* lfs.h must be included before this header (it is what pulls in lfs_util.h,
 * which is what acts on the switches). */
#ifndef LFS_UTIL_H
#error "include lfs.h before lfs_svcrt_config.h"
#endif

#ifndef LFS_NO_MALLOC
#error "LFS_NO_MALLOC is not defined: add it to the project Define list. Without it littlefs will call malloc(), which the kernel does not have."
#endif

#ifndef LFS_NO_ASSERT
#error "LFS_NO_ASSERT is not defined: add it to the project Define list."
#endif

#ifndef LFS_NO_DEBUG
#error "LFS_NO_DEBUG is not defined: add it to the project Define list."
#endif

#ifndef LFS_NO_WARN
#error "LFS_NO_WARN is not defined: add it to the project Define list."
#endif

#ifndef LFS_NO_ERROR
#error "LFS_NO_ERROR is not defined: add it to the project Define list."
#endif

#endif /* __LFS_SVCRT_CONFIG_H__ */
