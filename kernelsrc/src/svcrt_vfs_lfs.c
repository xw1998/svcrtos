/**
* @file svcrt_vfs_lfs.c
* @brief 把 SVCrtOS 的老门面 svcrt_fs（littlefs）接成 ark_vfs 的一个文件系统
* @details 目的只有一个：让 /mnt/<卷> 这条 Linux 风格路径真的能读写文件，
*          而不是再造一个 littlefs 绑定。这里不 include 任何 lfs 头，
*          只用 svcrt_fs.h 已经公开的那几个入口。
*
*          【继承的限制，不掩饰】
*          svcrt_fs 一个卷同时只允许一个写流（它自己的文件缓存只有一份）。
*          所以本驱动：
*            - 读用 svcrt_fs_read_at()，随机访问，可以多个 fd 同时读；
*            - 写用 svcrt_fs_open_write/write_next/close_write，同一时刻只
*              允许一个写句柄，第二个 open 写拿到 ARK_E_BUSY；
*            - 有写流在进行中时读也拒绝（ARK_E_BUSY），因为读要和写共享
*              同一份文件缓存，硬读会把在写的内容搅坏。
*          这些都是「说清楚做不到」，不是静默降级。
*
*          【错误怎么报】
*          不用 littlefs 的错误码做判断——那是 lfs 的实现细节，换了版本就会
*          变。能不能做全用 svcrt_fs_stat_path() 先问清楚；真失败了返回
*          ARK_E_IO，并把 svcrt_fs_last_error() 的原始数字写进日志，
*          由现场的人去看那个数字，而不是由这里猜一个原因。
*
* @author xw
* @date 2026.09.22
*/
#include "svcrt_features.h"

/* 本文件是「littlefs → ark_vfs」的桥。三个开关一起说话：
 *   SVCRT_USE_VFS     没有 VFS 就没什么可接的；
 *   SVCRT_USE_VFS_LFS 本模块自己的开关；
 *   SVCRT_USE_FS      没有 svcrt_fs 就没有 littlefs 可桥。
 * 任何一个不成立时整个单元不参与构建（features.h 里已有 #error 拦住组合）。 */
#if ((SVCRT_USE_VFS == 1) && (SVCRT_USE_VFS_LFS == 1) && (SVCRT_USE_FS == 1))

#include "ark_vfs.h"
#include "ark_vfs_priv.h"
#include "svcrt_fs.h"
#include "svcrt_log.h"
#include "svcrt_vfs.h"

#define LFS_PATH_MAX   (SVCRT_FS_PATH_MAX)

/* 核心当不透明指针用，形状由本驱动自己定 */
struct ark_vfs_file
{
    uint8_t  used;
    uint8_t  is_dir;
    uint8_t  writable;                  /* 本句柄持有写流 */
    uint32_t pos;
    char     path[LFS_PATH_MAX];        /* 卷内绝对路径，带前导 '/' */
};

static struct ark_vfs_file g_lfs_h[ARK_VFS_MAX_FDS];
static uint8_t             g_writer;    /* 1=有写流在进行中 */

/* ------------------------------------------------------------
 * 工具
 * ------------------------------------------------------------ */

/** @brief rel（相对挂载点、无前导 '/'）-> 卷内绝对路径 */
static int32_t p_abs(const char *rel, char *out, uint32_t cap)
{
    if (rel[0] == '\0')
    {
        if (cap < 2u) { return ARK_E_NAMETOOLONG; }
        out[0] = '/';
        out[1] = '\0';
        return ARK_VFS_OK;
    }
    if ((ark_p_strlen(rel) + 2u) > cap) { return ARK_E_NAMETOOLONG; }
    out[0] = '/';
    if (ark_p_strcpy(out + 1, cap - 1u, rel) < 0) { return ARK_E_NAMETOOLONG; }
    return ARK_VFS_OK;
}

static struct ark_vfs_file *p_alloc(const char *abs)
{
    uint32_t i;
    for (i = 0u; i < (uint32_t)ARK_VFS_MAX_FDS; i++)
    {
        if (g_lfs_h[i].used == 0u)
        {
            ark_p_memset(&g_lfs_h[i], 0, (ark_size_t)sizeof(g_lfs_h[i]));
            g_lfs_h[i].used = 1u;
            if (ark_p_strcpy(g_lfs_h[i].path, LFS_PATH_MAX, abs) < 0)
            {
                g_lfs_h[i].used = 0u;
                return NULL;
            }
            return &g_lfs_h[i];
        }
    }
    return NULL;
}

/** @brief 出错时把原始错误码落到日志，再返回统一的 ARK_E_IO */
static int32_t p_io(const char *what, const char *path)
{
    int32 raw = svcrt_fs_last_error();
    SVCRT_LOGW("ark_vfs", "%s failed on %s: svcrt_fs last_error=%d (%s)",
               what, path, (int)raw, svcrt_fs_error_name(raw));
    return ARK_E_IO;
}

/* ------------------------------------------------------------
 * fsdrv
 * ------------------------------------------------------------ */

static int32_t lfs_mount(struct ark_vfs_mnt *mnt)
{
    uint32_t i;

    (void)mnt;
    for (i = 0u; i < (uint32_t)ARK_VFS_MAX_FDS; i++)
    {
        g_lfs_h[i].used = 0u;
    }
    g_writer = 0u;
    if (svcrt_fs_mounted() == 0u)
    {
        /* 不自己挂：卷是哪一个设备、从哪开始、多长，是板级知识。
         * 走到这里说明调用顺序错了，明说比猜一个卷要好。 */
        SVCRT_LOGW("ark_vfs", "lfs mount: no volume mounted by svcrt_fs");
        return ARK_E_IO;
    }
    return ARK_VFS_OK;
}

static int32_t lfs_umount(struct ark_vfs_mnt *mnt)
{
    (void)mnt;
    /* 卷的挂载/卸载由 svcrt_vfs_mount_volume()/svcrt_vfs_umount() 成对负责，
     * 这里只清句柄表，避免「ark_vfs 卸了但 littlefs 还挂着」的错位。 */
    g_writer = 0u;
    return ARK_VFS_OK;
}

static int32_t lfs_open(struct ark_vfs_mnt *mnt, const char *rel, int flags,
                        struct ark_vfs_file **out)
{
    char abs[LFS_PATH_MAX];
    uint32 size = 0u;
    uint32 is_dir = 0u;
    int32_t  rc;
    int      want_write;
    struct ark_vfs_file *h;

    rc = p_abs(rel, abs, (uint32_t)sizeof(abs));
    if (rc != ARK_VFS_OK) { return rc; }
    if (abs[1] == '\0') { return ARK_E_ISDIR; }       /* 挂载根本身 */

    want_write = (((flags & ARK_O_ACCMODE) != ARK_O_RDONLY) ||
                  ((flags & (ARK_O_CREAT | ARK_O_TRUNC)) != 0)) ? 1 : 0;

    rc = svcrt_fs_stat_path(abs, &size, &is_dir);
    if (rc != 0)
    {
        if (want_write == 0)
        {
            return ARK_E_NOENT;                 /* 只读打开且不存在 */
        }
        if (svcrt_fs_last_error() != 0)
        {
            /* stat 真的报错了，而不是「没有这个条目」；具体是什么错门面
             * 没有给出可判别的信息，于是按 IO 报，并留下原始数字。 */
            return p_io("open/stat", abs);
        }
        is_dir = 0u;
        size = 0u;
    }
    else if (is_dir != 0u)
    {
        if ((flags & ARK_O_DIRECTORY) != 0u) { want_write = 0; }
        else { return ARK_E_ISDIR; }
    }

    h = p_alloc(abs);
    if (h == NULL) { return ARK_E_FULL; }
    h->is_dir = 0u;

    if (want_write != 0)
    {
        if ((mnt->flags & ARK_MNT_RO) != 0u) { h->used = 0u; return ARK_E_ROFS; }
        if (g_writer != 0u) { h->used = 0u; return ARK_E_BUSY; }
        if (svcrt_fs_open_write(abs) != 0)
        {
            h->used = 0u;
            return p_io("open_write", abs);
        }
        g_writer = 1u;
        h->writable = 1u;
    }

    if ((flags & ARK_O_APPEND) != 0u) { h->pos = size; }
    *out = h;
    return ARK_VFS_OK;
}

static int32_t lfs_close(struct ark_vfs_mnt *mnt, struct ark_vfs_file *f)
{
    int32_t rc = ARK_VFS_OK;

    (void)mnt;
    if (f == NULL) { return ARK_E_INVAL; }
    if (f->writable != 0u)
    {
        /* 关闭写流才是提交：littlefs 到这里才真正落盘 */
        if (svcrt_fs_close_write() != 0) { rc = p_io("close_write", f->path); }
        f->writable = 0u;
        g_writer = 0u;
    }
    f->used = 0u;
    return rc;
}

static ark_ssize_t lfs_read(struct ark_vfs_file *f, void *buf, ark_size_t n)
{
    uint32 got = 0u;
    int32_t  rc;

    if ((f == NULL) || (f->is_dir != 0u)) { return ARK_E_BADF; }
    if (buf == NULL) { return ARK_E_INVAL; }
    if (f->writable != 0u) { return ARK_E_INVAL; }     /* 写句柄不读 */
    if (g_writer != 0u) { return ARK_E_BUSY; }         /* 一份缓存，别人在写 */
    if (n == 0u) { return 0; }

    rc = svcrt_fs_read_at(f->path, (uint8 *)buf, n, f->pos, &got);
    if (rc != 0) { return p_io("read_at", f->path); }
    f->pos += got;
    return (ark_ssize_t)got;      /* 文件尾就是 0，不是错误 */
}

static ark_ssize_t lfs_write(struct ark_vfs_file *f, const void *buf,
                             ark_size_t n)
{
    if ((f == NULL) || (f->is_dir != 0u)) { return ARK_E_BADF; }
    if (buf == NULL) { return ARK_E_INVAL; }
    if (f->writable == 0u) { return ARK_E_BADF; }
    if (n == 0u) { return 0; }
    if (svcrt_fs_write_next((const uint8 *)buf, n) != 0)
    {
        return p_io("write_next", f->path);
    }
    f->pos += n;
    /* svcrt_fs 的写是全有全无：成功即整块写进去了，没有部分写入这回事 */
    return (ark_ssize_t)n;
}

static int32_t lfs_seek(struct ark_vfs_file *f, ark_off_t off, int whence)
{
    if (f == NULL) { return ARK_E_BADF; }
    if (f->writable != 0u) { return ARK_E_NOSYS; }   /* 写流是顺序的 */
    if (whence == ARK_SEEK_SET) { f->pos = (uint32_t)off; return ARK_VFS_OK; }
    if (whence == ARK_SEEK_CUR) { f->pos += (uint32_t)off; return ARK_VFS_OK; }
    if (whence == ARK_SEEK_END)
    {
        uint32 size = 0u;
        if (svcrt_fs_stat_path(f->path, &size, NULL) != 0)
        {
            return p_io("seek/stat", f->path);
        }
        f->pos = size + (uint32_t)off;
        return ARK_VFS_OK;
    }
    return ARK_E_INVAL;
}

static int32_t lfs_stat(struct ark_vfs_mnt *mnt, const char *rel,
                        struct ark_vfs_stat *st)
{
    char abs[LFS_PATH_MAX];
    uint32 size = 0u;
    uint32 is_dir = 0u;
    int32_t  rc;

    (void)mnt;
    if (st == NULL) { return ARK_E_INVAL; }
    rc = p_abs(rel, abs, (uint32_t)sizeof(abs));
    if (rc != ARK_VFS_OK) { return rc; }
    if (svcrt_fs_stat_path(abs, &size, &is_dir) != 0) { return ARK_E_NOENT; }
    st->mode  = (is_dir != 0u) ? ARK_S_IFDIR : ARK_S_IFREG;
    st->size  = (ark_off_t)size;
    st->mtime = 0u;                     /* 卷里不存时间 */
    return ARK_VFS_OK;
}

static int32_t lfs_unlink(struct ark_vfs_mnt *mnt, const char *rel)
{
    char abs[LFS_PATH_MAX];
    uint32 size = 0u;
    uint32 is_dir = 0u;
    int32_t  rc;

    (void)mnt;
    rc = p_abs(rel, abs, (uint32_t)sizeof(abs));
    if (rc != ARK_VFS_OK) { return rc; }
    if (svcrt_fs_stat_path(abs, &size, &is_dir) != 0) { return ARK_E_NOENT; }
    if (is_dir != 0u) { return ARK_E_ISDIR; }
    if (svcrt_fs_remove(abs) != 0) { return p_io("remove", abs); }
    return ARK_VFS_OK;
}

static int32_t lfs_rename(struct ark_vfs_mnt *mnt, const char *old_rel,
                          const char *new_rel)
{
    char a[LFS_PATH_MAX];
    char b[LFS_PATH_MAX];
    int32_t rc;

    (void)mnt;
    rc = p_abs(old_rel, a, (uint32_t)sizeof(a));
    if (rc != ARK_VFS_OK) { return rc; }
    rc = p_abs(new_rel, b, (uint32_t)sizeof(b));
    if (rc != ARK_VFS_OK) { return rc; }
    if (svcrt_fs_stat_path(a, NULL, NULL) != 0) { return ARK_E_NOENT; }
    if (svcrt_fs_stat_path(b, NULL, NULL) == 0) { return ARK_E_EXIST; }
    if (svcrt_fs_rename(a, b) != 0) { return p_io("rename", a); }
    return ARK_VFS_OK;
}

static int32_t lfs_opendir(struct ark_vfs_mnt *mnt, const char *rel,
                           struct ark_vfs_file **out)
{
    char abs[LFS_PATH_MAX];
    uint32 is_dir = 0u;
    int32_t rc;
    struct ark_vfs_file *h;

    (void)mnt;
    rc = p_abs(rel, abs, (uint32_t)sizeof(abs));
    if (rc != ARK_VFS_OK) { return rc; }
    if (svcrt_fs_stat_path(abs, NULL, &is_dir) != 0) { return ARK_E_NOENT; }
    if (is_dir == 0u) { return ARK_E_NOTDIR; }

    h = p_alloc(abs);
    if (h == NULL) { return ARK_E_FULL; }
    h->is_dir = 1u;
    h->pos    = 0u;
    *out = h;
    return ARK_VFS_OK;
}

/* svcrt_fs_list() 一次走完整个目录，而 readdir 是一次一条：用「数到第 n 条
 * 再停下」的方式对接。代价是每条 O(n)，换来的是一份没有游标状态、不会因为
 * 目录被改动而错位的列举——目录里几条文件，这点开销无所谓。 */
typedef struct
{
    uint32_t want;
    uint32_t idx;
    struct ark_vfs_dirent *ent;
    int      found;
    int      too_long;
} p_list_ctx_t;

static int p_list_cb(const char *name, uint32 size, uint8 is_dir, void *arg)
{
    p_list_ctx_t *c = (p_list_ctx_t *)arg;
    ark_size_t nlen;

    if ((ark_p_strcmp(name, ".") == 0) || (ark_p_strcmp(name, "..") == 0))
    {
        return 0;       /* 不把 . / .. 当条目：它们在 VFS 里没有意义 */
    }
    if (c->idx != c->want)
    {
        c->idx++;
        return 0;
    }
    nlen = ark_p_strlen(name);
    if ((nlen + 1u) > (ark_size_t)ARK_VFS_NAME_MAX)
    {
        c->too_long = 1;
        c->found = 1;
        return -1;
    }
    ark_p_memcpy(c->ent->name, name, nlen + 1u);
    c->ent->mode = (is_dir != 0u) ? ARK_S_IFDIR : ARK_S_IFREG;
    c->ent->size = (ark_off_t)size;
    c->found = 1;
    return -1;
}

static int32_t lfs_readdir(struct ark_vfs_file *d, struct ark_vfs_dirent *ent)
{
    p_list_ctx_t ctx;

    if ((d == NULL) || (d->is_dir == 0u)) { return ARK_E_NOTDIR; }
    if (ent == NULL) { return ARK_E_INVAL; }

    ark_p_memset(&ctx, 0, (ark_size_t)sizeof(ctx));
    ctx.want = d->pos;
    ctx.ent  = ent;
    if (svcrt_fs_list(d->path, p_list_cb, &ctx) != 0)
    {
        return p_io("list", d->path);
    }
    if (ctx.found == 0) { return ARK_E_NOENT; }     /* 没有了 */
    if (ctx.too_long != 0) { return ARK_E_NAMETOOLONG; }
    d->pos++;
    return ARK_VFS_OK;
}

static int32_t lfs_closedir(struct ark_vfs_mnt *mnt, struct ark_vfs_file *d)
{
    (void)mnt;
    if (d != NULL) { d->used = 0u; }
    return ARK_VFS_OK;
}

const ark_vfs_fsdrv_t svcrt_vfs_lfs_fsdrv = {
    "lfs",
    ARK_FS_CAP_READ | ARK_FS_CAP_WRITE | ARK_FS_CAP_CREATE | ARK_FS_CAP_UNLINK |
    ARK_FS_CAP_RENAME | ARK_FS_CAP_DIR | ARK_FS_CAP_STAT | ARK_FS_CAP_TRUNC,
    lfs_mount,
    lfs_umount,
    lfs_open,
    lfs_close,
    lfs_read,
    lfs_write,
    lfs_seek,
    lfs_stat,
    lfs_unlink,
    NULL,                   /* mkdir：littlefs 的目录是隐式的，门面没有入口 */
    lfs_rename,
    lfs_opendir,
    lfs_readdir,
    lfs_closedir
};

#else   /* VFS / VFS_LFS / FS 三者不全开 */

/* 空翻译单元不是合法 C；留一个无副作用的声明让本文件仍然可编译 */
typedef int svcrt_vfs_lfs_disabled_tu;

#endif  /* SVCRT_USE_VFS && SVCRT_USE_VFS_LFS && SVCRT_USE_FS */
