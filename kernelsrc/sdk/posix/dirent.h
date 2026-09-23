/**
* @file dirent.h
* @brief opendir()/readdir()/closedir() for SVCrtOS Apps.
* @details DIR is opaque and comes out of a small fixed pool: the App has no
*          way to know how much RAM it may spend on one directory stream, and
*          a bounded number is the honest answer. When the pool is empty,
*          opendir returns NULL with errno = ENOMEM - it does not hand back a
*          handle that quietly points at another stream.
*
*          readdir returns NULL both at the end of the directory and on an
*          error; the two are told apart by errno, which is set to 0 on the
*          end of the stream - the same convention the C library uses, and
*          the only one that does not turn "there is no error" into a lie.
*
*          d_type is a DT_* value. Only DT_DIR, DT_REG and DT_CHR can occur
*          here; the rest of the standard set is named so a switch over it
*          still compiles, but it will never be produced.
* @author xw
*/
#ifndef __SVCRT_DIRENT_H__
#define __SVCRT_DIRENT_H__

#include "svcrt_posix_types.h"

/** Longest entry name a dirent can carry, NUL included. One path component,
 *  not a path. */
#define SVCRT_POSIX_NAME_MAX  (32u)

struct dirent
{
    char         d_name[SVCRT_POSIX_NAME_MAX];
    uint32       d_type;    /**< DT_* */
    svcrt_off_t  d_size;    /**< bytes; 0 for a directory */
};

#define DT_UNKNOWN  0
#define DT_FIFO     1
#define DT_CHR      2
#define DT_DIR      4
#define DT_BLK      6
#define DT_REG      8
#define DT_LNK      10

#define IFTODT(m)   (((m) & 0xF000) >> 12)
#define DTTOIF(d)   ((d) << 12)

typedef struct svcrt_posix_dir DIR;

/** How many directory streams can be open at once. Tunable before including. */
#ifndef SVCRT_POSIX_DIR_MAX
#define SVCRT_POSIX_DIR_MAX  (2u)
#endif

DIR *svcrt_posix_opendir(const char *path);
struct dirent *svcrt_posix_readdir(DIR *d);
int svcrt_posix_closedir(DIR *d);

#define opendir(path)   svcrt_posix_opendir(path)
#define readdir(d)      svcrt_posix_readdir(d)
#define closedir(d)     svcrt_posix_closedir(d)

#endif /* __SVCRT_DIRENT_H__ */
