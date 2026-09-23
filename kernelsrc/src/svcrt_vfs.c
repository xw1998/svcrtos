/**
* @file svcrt_vfs.c
* @brief SVCrtOS 的 Linux 风格路径命名空间（ark_vfs 门面）
* @details 这里做三件事，每件都只有一处：
*            1) 挂载点：/ 用 ramfs（易失便签区），/dev 用 devfs（设备镜像），
*               /mnt/<名字> 用 littlefs（持久卷，显式挂）；
*            2) 门面：shell 与 App 只需要 svcrt_vfs.h，不直接碰 ark_vfs，
*               组件换版本时改的是这一层；
*            3) 错误：一律走 ark_vfs 的负错误码，符号名由
*               svcrt_vfs_error_name() 附加，数字永远保留。
*
*          【为什么 / 用 ramfs 而不是把 NOR 直接挂在 /】
*          挂载点是「谁知道」的问题：内核知道设备注册表（所以 /dev 归它），
*          不知道哪块设备上的哪个偏移放着文件系统（那是板级布局知识）。
*          所以 / 给一块不依赖硬件的易失区，持久卷留一个显式的入口。
*
* @author xw
* @date 2026.09.22
*/
#include "svcrt_vfs.h"
#include "svcrt_features.h"

/* ============================================================
 * 关掉 SVCRT_USE_VFS 时的诚实空实现
 *
 * 与「把文件从工程里移出去」的区别：符号还在，所以调用者（shell、App）
 * 不用也跟着写条件编译；行为是明确的 NOSYS，而不是链接时的 undefined。
 * 不做「假装成功」：初始化成功、挂载成功而实际上什么都没发生，是这一行
 * 最不能接受的一种错。
 * ============================================================ */
#if (SVCRT_USE_VFS != 1)

#include <stddef.h>         /* NULL；开启分支是从 svcrt_log.h 间接得到的 */

int32 svcrt_vfs_init(void) { return SVCRT_VFS_ENOSYS; }
uint8 svcrt_vfs_ready(void) { return 0u; }

int32 svcrt_vfs_mount_volume(const char *dev, uint32 offset, uint32 size,
                             const char *target)
{
    (void)dev; (void)offset; (void)size; (void)target;
    return SVCRT_VFS_ENOSYS;
}

int32 svcrt_vfs_umount(const char *target)
{
    (void)target;
    return SVCRT_VFS_ENOSYS;
}

int32 svcrt_vfs_mount_info(uint32 idx, svcrt_vfs_mount_info_t *out)
{
    (void)idx;
    if (out != NULL)
    {
        out->target[0] = '\0';
        out->fs     = NULL;
        out->source = NULL;
        out->ro     = 0u;
    }
    return -1;
}

int32 svcrt_vfs_mount_count(void) { return 0; }

int32 svcrt_vfs_read_file(const char *path, uint8 *buf, uint32 max,
                          uint32 *out_len)
{
    (void)path; (void)buf; (void)max;
    if (out_len != NULL) { *out_len = 0u; }
    return SVCRT_VFS_ENOSYS;
}

int32 svcrt_vfs_write_file(const char *path, const uint8 *buf, uint32 len)
{
    (void)path; (void)buf; (void)len;
    return SVCRT_VFS_ENOSYS;
}

int32 svcrt_vfs_open_read(const char *path)
{
    (void)path;
    return SVCRT_VFS_ENOSYS;
}

int32 svcrt_vfs_read(int32 fd, uint8 *buf, uint32 len)
{
    (void)fd; (void)buf; (void)len;
    return SVCRT_VFS_ENOSYS;
}

int32 svcrt_vfs_close(int32 fd)
{
    (void)fd;
    return SVCRT_VFS_ENOSYS;
}

int32 svcrt_vfs_remove(const char *path)
{
    (void)path;
    return SVCRT_VFS_ENOSYS;
}

int32 svcrt_vfs_list(const char *dir, svcrt_vfs_list_cb_t cb, void *arg)
{
    (void)dir; (void)cb; (void)arg;
    return SVCRT_VFS_ENOSYS;
}

const char *svcrt_vfs_error_name(int32 rc)
{
    return (rc == SVCRT_VFS_ENOSYS) ? "ENOSYS(no VFS in this build)" : "";
}

/* App 面：这个构建里句柄一个也发不出去。不发假句柄——发了就会有人拿着它
 * 去 read，然后在别的地方报一个看不出真因的错。 */
int32 svcrt_vfs_app_open(const char *path, uint32 flags)
{
    (void)path; (void)flags;
    return SVCRT_VFS_ENOSYS;
}

int32 svcrt_vfs_app_read(int32 handle, uint8 *buf, uint32 len)
{
    (void)handle; (void)buf; (void)len;
    return SVCRT_VFS_ENOSYS;
}

int32 svcrt_vfs_app_write(int32 handle, const uint8 *buf, uint32 len)
{
    (void)handle; (void)buf; (void)len;
    return SVCRT_VFS_ENOSYS;
}

int32 svcrt_vfs_app_seek(int32 handle, int32 off, uint32 whence)
{
    (void)handle; (void)off; (void)whence;
    return SVCRT_VFS_ENOSYS;
}

int32 svcrt_vfs_app_readdir(int32 handle, char *name, uint32 cap,
                           uint32 *mode, uint32 *size)
{
    (void)handle; (void)name; (void)cap; (void)mode; (void)size;
    return SVCRT_VFS_ENOSYS;
}

int32 svcrt_vfs_app_close(int32 handle)
{
    (void)handle;
    return SVCRT_VFS_ENOSYS;
}

int32 svcrt_vfs_app_stat(const char *path, uint32 *size, uint32 *mode)
{
    (void)path;
    if (size != NULL) { *size = 0u; }
    if (mode != NULL) { *mode = 0u; }
    return SVCRT_VFS_ENOSYS;
}

int32 svcrt_vfs_app_unlink(const char *path)
{
    (void)path;
    return SVCRT_VFS_ENOSYS;
}

void svcrt_vfs_app_task_exit(uint32 task_id)
{
    (void)task_id;
}

#else   /* SVCRT_USE_VFS == 1 */

#include "svcrt_log.h"
#include "ark_vfs.h"
#include "ark_vfs_ramfs.h"
#include "ark_vfs_port_svcrtos.h"

#if (SVCRT_USE_VFS_LFS == 1)
#include "svcrt_fs.h"
/* 在 svcrt_vfs_lfs.c 里定义；关掉 littlefs 桥时那个文件不参与编译 */
extern const ark_vfs_fsdrv_t svcrt_vfs_lfs_fsdrv;
#endif

/* svcrt_current_task_id（句柄归属）与 svcrt_task_status_get_internal
 * （清理已死任务留下的句柄） */
#include "svcrt_task.h"

/* 两个头文件对「路径最长多少」必须说同一句话，否则会在边界上错一位 */
typedef char svcrt_vfs_pathmax_check[(SVCRT_VFS_PATH_MAX == ARK_VFS_PATH_MAX) ? 1 : -1];

/* 关闭构建里门面返回的那个数字，与 ark_vfs 的 NOSYS 必须是同一个 */
typedef char svcrt_vfs_enosys_check[(SVCRT_VFS_ENOSYS == ARK_E_NOSYS) ? 1 : -1];

/* stat 的类型位是直接透传的（不做映射），所以两边必须同值；否则 App 会把
 * 目录当普通文件，或者反过来。 */
typedef char svcrt_vfs_sif_check[((SVCRT_VFS_S_IFDIR == ARK_S_IFDIR) &&
                                  (SVCRT_VFS_S_IFREG == ARK_S_IFREG) &&
                                  (SVCRT_VFS_S_IFCHR == ARK_S_IFCHR)) ? 1 : -1];

/* 打开标志与 seek 起点是跨 SVC 的 ABI，App 那边（svcrt.h 的 SVCRT_PATH_*、
 * POSIX 层的 O_* / SEEK_*）就是把这些数字原样传下来。它们声称自己就是
 * POSIX 的值，所以在这里对着 POSIX 的字面值钉死：两边谁漂了都会先在这里
 * 变成编译错误，而不是板上表现成「O_CREAT 没生效」。 */
typedef char svcrt_vfs_o_abi_check[((SVCRT_VFS_O_RDONLY    == 0x0000u) &&
                                    (SVCRT_VFS_O_WRONLY    == 0x0001u) &&
                                    (SVCRT_VFS_O_RDWR      == 0x0002u) &&
                                    (SVCRT_VFS_O_ACCMODE   == 0x0003u) &&
                                    (SVCRT_VFS_O_CREAT     == 0x0004u) &&
                                    (SVCRT_VFS_O_TRUNC     == 0x0008u) &&
                                    (SVCRT_VFS_O_APPEND    == 0x0010u) &&
                                    (SVCRT_VFS_O_DIRECTORY == 0x0020u)) ? 1 : -1];
typedef char svcrt_vfs_seek_abi_check[((SVCRT_VFS_SEEK_SET == 0u) &&
                                       (SVCRT_VFS_SEEK_CUR == 1u) &&
                                       (SVCRT_VFS_SEEK_END == 2u)) ? 1 : -1];

static uint8 g_vfs_ready;
static char  g_vol_target[SVCRT_VFS_PATH_MAX];   /* 空串=没有持久卷 */

/* ------------------------------------------------------------
 * 小工具
 * ------------------------------------------------------------ */

static int p_copy(char *dst, uint32 cap, const char *src)
{
    uint32 i = 0u;
    if ((dst == NULL) || (src == NULL) || (cap == 0u)) { return -1; }
    while ((src[i] != '\0') && ((i + 1u) < cap))
    {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
    return (src[i] == '\0') ? 0 : -1;
}

static int p_eq(const char *a, const char *b)
{
    uint32 i = 0u;
    if ((a == NULL) || (b == NULL)) { return 0; }
    while ((a[i] != '\0') && (a[i] == b[i])) { i++; }
    return (a[i] == b[i]) ? 1 : 0;
}

/* ------------------------------------------------------------
 * 命名空间
 * ------------------------------------------------------------ */

int32 svcrt_vfs_init(void)
{
    int32 rc;

    if (g_vfs_ready != 0u)
    {
        return 0;               /* 幂等：重复调用不重复挂 */
    }

    /* 钩子（调度器锁 + 毫秒时钟 + 日志）与 /dev 一起装好 */
    rc = ark_vfs_port_init();
    if (rc != 0)
    {
        SVCRT_LOGE("vfs", "dev mount failed: %d (%s)", (int)rc,
                   ark_vfs_error_name(rc));
        return rc;
    }

    /* / 是易失便签区；/dev 已在上面挂好，最长前缀匹配保证各走各的 */
    rc = ark_vfs_mount("ram", "/", ark_ramfs_fs(), NULL, 0u, NULL);
    if (rc != 0)
    {
        SVCRT_LOGE("vfs", "root mount failed: %d (%s)", (int)rc,
                   ark_vfs_error_name(rc));
        ark_vfs_deinit();
        return rc;
    }

    g_vol_target[0] = '\0';
    g_vfs_ready = 1u;
    SVCRT_LOGI("vfs", "namespace up: / (ramfs), /dev (devfs)");
    return 0;
}

uint8 svcrt_vfs_ready(void)
{
    return g_vfs_ready;
}

int32 svcrt_vfs_mount_volume(const char *dev, uint32 offset, uint32 size,
                             const char *target)
{
#if (SVCRT_USE_VFS_LFS == 0)
    /* 本构建没有 littlefs 桥，而门面又不提供「自带 fsdrv」的入口，
     * 所以这里确实没任何能做成的事——明说，不去假装挂上了一个空卷。 */
    (void)dev; (void)offset; (void)size; (void)target;
    return ARK_E_NOSYS;
#else
    ark_vfs_port_blk_t vol;
    int32 rc;

    if ((g_vfs_ready == 0u) || (dev == NULL) || (target == NULL) ||
        (target[0] == '\0'))
    {
        return ARK_E_INVAL;
    }
    if (g_vol_target[0] != '\0')
    {
        /* svcrt_fs 只有一个卷，这是它公开的限制，不是这里加的 */
        return ARK_E_EXIST;
    }

    /* 先用块设备适配层校验窗口：它能把「没有这个设备」和「窗口不合法」
     * 分辨出来，而 svcrt_fs_mount 只会回一个 -1。 */
    rc = ark_vfs_port_blk_init(&vol, dev, offset, size, dev);
    if (rc != ARK_VFS_OK)
    {
        SVCRT_LOGE("vfs", "volume window rejected: %d (%s)", (int)rc,
                   ark_vfs_error_name(rc));
        return rc;
    }

    /* svcrt_fs 只有一个卷，所以「已经挂着」和「按你给的窗口挂」是两回事：
     * 直接复用会把 dev/offset/size 静默丢掉，同一句 mount 在「卷已挂/未挂」
     * 两种状态下就会给出不同的几何——那正是最难查的一类错答案。
     * size==0 已经由 vol.size 归一化成实际长度，比较一律用归一化后的值。 */
    if (svcrt_fs_mounted() == 0u)
    {
        if (svcrt_fs_mount(dev, offset, vol.size) != 0)
        {
            int32 raw = svcrt_fs_last_error();
            /* 挂载不格式化：卷没格式化就该报错，不该顺手擦掉别人的数据。
             * 请求的窗口一并打出来：littlefs 只回一个错误码，而「窗口和盘上
             * 的卷几何对不上」是最常见的原因，现场得能自己算。 */
            SVCRT_LOGE("vfs", "svcrt_fs_mount(%s, off=%u, size=%u) failed: "
                       "last_error=%d (%s)", dev, (unsigned)offset,
                       (unsigned)vol.size, (int)raw, svcrt_fs_error_name(raw));
            return ARK_E_IO;
        }
    }
    else
    {
        const char *cur_dev  = NULL;
        uint32      cur_off  = 0u;
        uint32      cur_size = 0u;

        if ((svcrt_fs_volume(&cur_dev, &cur_off, &cur_size) != 0) ||
            (cur_dev == NULL) || (p_eq(cur_dev, dev) == 0) ||
            (cur_off != offset) || (cur_size != vol.size))
        {
            SVCRT_LOGE("vfs", "a different volume is already mounted: "
                       "%s off=%u size=%u (asked for %s off=%u size=%u)",
                       (cur_dev != NULL) ? cur_dev : "?", (unsigned)cur_off,
                       (unsigned)cur_size, dev, (unsigned)offset,
                       (unsigned)vol.size);
            return ARK_E_EXIST;
        }
    }

    rc = ark_vfs_mount(dev, target, &svcrt_vfs_lfs_fsdrv, NULL, 0u, NULL);
    if (rc != ARK_VFS_OK)
    {
        SVCRT_LOGE("vfs", "mount %s at %s failed: %d (%s)", dev, target,
                   (int)rc, ark_vfs_error_name(rc));
        (void)svcrt_fs_unmount();       /* 别留下「挂了卷但没挂点」 */
        return rc;
    }

    if (p_copy(g_vol_target, (uint32)sizeof(g_vol_target), target) != 0)
    {
        (void)ark_vfs_umount(target);
        (void)svcrt_fs_unmount();
        g_vol_target[0] = '\0';
        return ARK_E_NAMETOOLONG;
    }
    SVCRT_LOGI("vfs", "volume %s mounted at %s", dev, target);
    return ARK_VFS_OK;
#endif
}

int32 svcrt_vfs_umount(const char *target)
{
    int32 rc;

    if ((g_vfs_ready == 0u) || (target == NULL))
    {
        return ARK_E_INVAL;
    }
    rc = ark_vfs_umount(target);
    if (rc != ARK_VFS_OK)
    {
        return rc;      /* 有文件还开着等情形，原样返回原因 */
    }
    if ((g_vol_target[0] != '\0') && (p_eq(g_vol_target, target) != 0))
    {
        g_vol_target[0] = '\0';
#if (SVCRT_USE_VFS_LFS == 1)
        (void)svcrt_fs_unmount();
#endif
    }
    return ARK_VFS_OK;
}

int32 svcrt_vfs_mount_count(void)
{
    struct ark_vfs_mnt *m = NULL;
    int32 n = 0;

    if (g_vfs_ready == 0u) { return 0; }
    while ((m = ark_vfs_next_mount(m)) != NULL) { n++; }
    return n;
}

int32 svcrt_vfs_mount_info(uint32 idx, svcrt_vfs_mount_info_t *out)
{
    struct ark_vfs_mnt *m = NULL;
    uint32 i;

    if ((out == NULL) || (g_vfs_ready == 0u)) { return -1; }
    for (i = 0u; i <= idx; i++)
    {
        m = ark_vfs_next_mount(m);
        if (m == NULL) { return -1; }
    }
    if (p_copy(out->target, (uint32)sizeof(out->target), m->target) != 0)
    {
        return -1;
    }
    out->fs     = (m->fs != NULL) ? m->fs->name : "?";
    out->source = m->be != NULL ? m->be->name : NULL;
    out->ro     = (uint8)(((m->flags & ARK_MNT_RO) != 0u) ? 1u : 0u);
    return 0;
}

/* ------------------------------------------------------------
 * 门面：这些只是把参数透下去，不做多余的解释
 * ------------------------------------------------------------ */

int32 svcrt_vfs_read_file(const char *path, uint8 *buf, uint32 max,
                          uint32 *out_len)
{
    ark_size_t got = 0u;
    int32 rc;

    if (out_len != NULL) { *out_len = 0u; }
    if ((path == NULL) || (buf == NULL)) { return ARK_E_INVAL; }
    rc = ark_vfs_read_all(path, buf, max, &got);
    if (out_len != NULL) { *out_len = got; }
    return rc;
}

int32 svcrt_vfs_open_read(const char *path)
{
    if (path == NULL) { return ARK_E_INVAL; }
    return ark_vfs_open(path, (int)ARK_O_RDONLY);
}

int32 svcrt_vfs_read(int32 fd, uint8 *buf, uint32 len)
{
    ark_ssize_t n;

    if (buf == NULL) { return ARK_E_INVAL; }
    if (len == 0u) { return 0; }
    n = ark_vfs_read(fd, (void *)buf, (ark_size_t)len);
    return (int32)n;        /* ark_ssize_t 就是 int32_t，不在这里做窄化 */
}

int32 svcrt_vfs_close(int32 fd)
{
    return ark_vfs_close(fd);
}

int32 svcrt_vfs_write_file(const char *path, const uint8 *buf, uint32 len)
{
    if ((path == NULL) || ((buf == NULL) && (len != 0u))) { return ARK_E_INVAL; }
    return ark_vfs_write_all(path, buf, len);
}

int32 svcrt_vfs_remove(const char *path)
{
    if (path == NULL) { return ARK_E_INVAL; }
    return ark_vfs_unlink(path);
}

int32 svcrt_vfs_list(const char *dir, svcrt_vfs_list_cb_t cb, void *arg)
{
    struct ark_vfs_dirent ent;
    int32 fd;
    int32 rc;

    if ((dir == NULL) || (cb == NULL)) { return ARK_E_INVAL; }
    fd = ark_vfs_opendir(dir);
    if (fd < 0) { return fd; }
    while ((rc = ark_vfs_readdir(fd, &ent)) == ARK_VFS_OK)
    {
        uint32 size = (ent.size > 0) ? (uint32)ent.size : 0u;
        if (cb(ent.name, (uint32)((ent.mode & ARK_S_IFDIR) != 0u ? 1u : 0u),
               size, arg) != 0)
        {
            break;
        }
    }
    (void)ark_vfs_closedir(fd);
    /* 走到底的 ARK_E_NOENT 不是错误，是「列完了」 */
    return (rc == ARK_VFS_OK) ? ARK_VFS_OK : ((rc == ARK_E_NOENT) ? ARK_VFS_OK : rc);
}

const char *svcrt_vfs_error_name(int32 rc)
{
    return ark_vfs_error_name(rc);
}

/* ============================================================
 * App 面：带句柄的流式打开
 *
 * 句柄 = SVCRT_VFS_HANDLE_FLAG | (序号 << 8) | 槽号。
 * 序号每复用一个槽就加一（0 留给「从没用过」），所以一个槽被回收后再
 * 发出去的新句柄，和上一个持有者手里的旧号码不会撞上——旧号码在新
 * 持有者那里是 BADF，而不是「悄悄打开了别人的文件」。
 * ============================================================ */

typedef struct
{
    int32  fd;        /* ark_vfs 的 fd；-1 = 空槽 */
    uint32 owner;     /* 打开它的任务号（1 基）；无意义当 fd<0 */
    uint8  seq;       /* 发给这个槽的最后一个序号，1..255 */
    uint8  is_dir;
    uint8  append;    /* 打开时带了 O_APPEND */
    /* 内核侧镜像的读写位置与大小。为什么要镜像：两个文件系统对
     * seek 的返回约定不一样——ramfs 回「新的偏移」，littlefs 桥回 0。
     * 直接用下层的返回值当 POSIX lseek 的返回值，在 littlefs 卷上会
     * 永远回 0，那是个「看着成功、答案是错的」结果。所以位置由这里
     * 记账，seek 一律回我们自己算出来的新偏移。 */
    uint32 pos;
    uint32 size;
} svcrt_vfs_app_slot_t;

static svcrt_vfs_app_slot_t g_app_fds[SVCRT_VFS_HANDLE_MAX];
static uint8 g_app_fds_ready;

/* ark_vfs 的 fd 是 0 基的，所以「空槽」不能靠静态零初始化表达（0 是一个
 * 合法的 fd），必须显式写成 -1。 */
static void app_fd_init(void)
{
    uint32 i;

    if (g_app_fds_ready != 0u) { return; }
    for (i = 0u; i < SVCRT_VFS_HANDLE_MAX; i++)
    {
        g_app_fds[i].fd     = -1;
        g_app_fds[i].owner  = 0u;
        g_app_fds[i].seq    = 0u;
        g_app_fds[i].is_dir = 0u;
        g_app_fds[i].append = 0u;
        g_app_fds[i].pos    = 0u;
        g_app_fds[i].size   = 0u;
    }
    g_app_fds_ready = 1u;
}

static int app_flags_to_ark(uint32 flags)
{
    int a;

    switch (flags & SVCRT_VFS_O_ACCMODE)
    {
    case SVCRT_VFS_O_WRONLY: a = ARK_O_WRONLY; break;
    case SVCRT_VFS_O_RDWR:   a = ARK_O_RDWR;   break;
    default:                 a = ARK_O_RDONLY; break;
    }
    if ((flags & SVCRT_VFS_O_CREAT) != 0u)     { a |= ARK_O_CREAT; }
    if ((flags & SVCRT_VFS_O_TRUNC) != 0u)     { a |= ARK_O_TRUNC; }
    if ((flags & SVCRT_VFS_O_APPEND) != 0u)    { a |= ARK_O_APPEND; }
    if ((flags & SVCRT_VFS_O_DIRECTORY) != 0u) { a |= ARK_O_DIRECTORY; }
    return a;
}

/* 租一个槽；句子取走后调用方负责归还（失败时由调用方关 fd） */
static int32 app_slot_take(int32 fd, uint32 flags)
{
    uint32 i;

    app_fd_init();
    for (i = 0u; i < SVCRT_VFS_HANDLE_MAX; i++)
    {
        if (g_app_fds[i].fd < 0)
        {
            uint8 seq = (uint8)(g_app_fds[i].seq + 1u);

            if (seq == 0u) { seq = 1u; }    /* 0 留给「从没用过」 */
            g_app_fds[i].seq    = seq;
            g_app_fds[i].fd     = fd;
            g_app_fds[i].owner  = (uint32)svcrt_current_task_id;
            g_app_fds[i].is_dir = (uint8)(((flags & SVCRT_VFS_O_DIRECTORY) != 0u) ? 1u : 0u);
            g_app_fds[i].append = (uint8)(((flags & SVCRT_VFS_O_APPEND) != 0u) ? 1u : 0u);
            g_app_fds[i].pos    = 0u;
            g_app_fds[i].size   = 0u;
            return (int32)((uint32)SVCRT_VFS_HANDLE_FLAG |
                           ((uint32)seq << 8) | i);
        }
    }
    return ARK_E_FULL;
}

static void app_slot_release(uint32 i)
{
    (void)ark_vfs_close(g_app_fds[i].fd);
    g_app_fds[i].fd     = -1;
    g_app_fds[i].owner  = 0u;
    g_app_fds[i].is_dir = 0u;
    /* seq 留着：旧句柄就靠它失效 */
}

static int32 app_fd_lookup(int32 handle, uint32 *out_slot)
{
    uint32 h    = (uint32)handle;
    uint32 slot = h & 0xFFu;
    uint32 seq  = (h >> 8) & 0xFFu;

    app_fd_init();
    if ((h & 0xFFF00000u) != SVCRT_VFS_HANDLE_FLAG)      /* 只看高 12 位的 tag */
    {
        return ARK_E_BADF;
    }
    if (slot >= SVCRT_VFS_HANDLE_MAX)
    {
        return ARK_E_BADF;
    }
    /* 归属一次说清：序号不对、槽空、或不是本任务的，一律 BADF。
     * 不给「猜对了号码就能用」留任何余地。 */
    if ((seq == 0u) || ((uint32)g_app_fds[slot].seq != seq) ||
        (g_app_fds[slot].fd < 0) ||
        (g_app_fds[slot].owner != (uint32)svcrt_current_task_id))
    {
        return ARK_E_BADF;
    }
    *out_slot = slot;
    return ARK_VFS_OK;
}

/* 把已经不在的任务留下的槽清掉。
 * 故障/崩溃路径跑在中断上下文（HardFault / PendSV），那里不能做文件 I/O，
 * 所以那里不做清理，改成「下次有人要句柄时顺手清」——见 svcrt_vfs_app_open。
 * 序号使这样回收是安全的：被清掉的槽再发出去，号码与旧的一样不了。 */
static void app_reclaim_dead(void)
{
    uint32 i;

    app_fd_init();
    for (i = 0u; i < SVCRT_VFS_HANDLE_MAX; i++)
    {
        if (g_app_fds[i].fd < 0) { continue; }
        /* 越界的任务号也回 -1，一并当死掉处理 */
        if (svcrt_task_status_get_internal((int32)g_app_fds[i].owner) < SVCRT_TASK_READY)
        {
            app_slot_release(i);
        }
    }
}

static int32 app_open_once(const char *path, uint32 flags)
{
    int32 fd = ark_vfs_open(path, app_flags_to_ark(flags));
    int32 handle;
    struct ark_vfs_stat st;

    if (fd < 0) { return fd; }
    handle = app_slot_take(fd, flags);
    if (handle < 0)
    {
        /* 没槽了：把刚拿到的 ark fd 还回去，别让一个调用者永远关不到的
         * 句柄占着那张只有 ARK_VFS_MAX_FDS 张的表 */
        (void)ark_vfs_close(fd);
        return handle;
    }
    /* 取一次大小：SEEK_END 与 O_APPEND 的写入位置全靠它。取不到就当 0，
     * 不把「没量到」说成一个数。 */
    if (ark_vfs_stat(path, &st) == ARK_VFS_OK)
    {
        uint32 slot = (uint32)handle & 0xFFu;
        g_app_fds[slot].size = (st.size > 0) ? (uint32)st.size : 0u;
    }
    /* O_APPEND 在两个文件系统里都只在 open 时定位一次（ramfs 与 littlefs 桥
     * 都是「打开时把位置置为文件尾，之后顺序走」），所以镜像也照这个来；
     * 否则第一个 write 的位置就会与设备真实位置分叉。 */
    if (g_app_fds[(uint32)handle & 0xFFu].append != 0u)
    {
        g_app_fds[(uint32)handle & 0xFFu].pos =
            g_app_fds[(uint32)handle & 0xFFu].size;
    }
    return handle;
}

int32 svcrt_vfs_app_open(const char *path, uint32 flags)
{
    int32 rc;

    if ((g_vfs_ready == 0u) || (path == NULL) || (path[0] == '\0'))
    {
        return ARK_E_INVAL;
    }
    if (((flags & SVCRT_VFS_O_DIRECTORY) != 0u) &&
        ((flags & SVCRT_VFS_O_ACCMODE) != SVCRT_VFS_O_RDONLY))
    {
        return ARK_E_INVAL;     /* 目录只读打开：写目录没有意义 */
    }

    rc = app_open_once(path, flags);
    if (rc == ARK_E_FULL)
    {
        /* 满可能只是「已经不在的任务留下的句柄」。先清一次再试一次；
         * 真的满，重试会得到同样一个 FULL。 */
        app_reclaim_dead();
        rc = app_open_once(path, flags);
    }
    return rc;
}

int32 svcrt_vfs_app_read(int32 handle, uint8 *buf, uint32 len)
{
    uint32 slot;
    int32 rc;
    ark_ssize_t n;

    if (buf == NULL) { return ARK_E_INVAL; }
    if (len == 0u) { return 0; }
    rc = app_fd_lookup(handle, &slot);
    if (rc != ARK_VFS_OK) { return rc; }
    if (g_app_fds[slot].is_dir != 0u) { return ARK_E_ISDIR; }
    n = ark_vfs_read(g_app_fds[slot].fd, (void *)buf, (ark_size_t)len);
    if (n > 0) { g_app_fds[slot].pos += (uint32)n; }
    return (int32)n;
}

int32 svcrt_vfs_app_write(int32 handle, const uint8 *buf, uint32 len)
{
    uint32 slot;
    int32 rc;
    ark_ssize_t n;

    if ((buf == NULL) && (len != 0u)) { return ARK_E_INVAL; }
    if (len == 0u) { return 0; }
    rc = app_fd_lookup(handle, &slot);
    if (rc != ARK_VFS_OK) { return rc; }
    if (g_app_fds[slot].is_dir != 0u) { return ARK_E_ISDIR; }
    n = ark_vfs_write(g_app_fds[slot].fd, (const void *)buf, (ark_size_t)len);
    if (n > 0)
    {
        /* 位置顺写；追加已经在 open 时定位过了。大小只增不减——写短了
         * 不会把文件截短。 */
        g_app_fds[slot].pos += (uint32)n;
        if (g_app_fds[slot].pos > g_app_fds[slot].size)
        {
            g_app_fds[slot].size = g_app_fds[slot].pos;
        }
    }
    return (int32)n;
}

int32 svcrt_vfs_app_seek(int32 handle, int32 off, uint32 whence)
{
    uint32 slot;
    int32 rc;
    int w;

    if (whence > SVCRT_VFS_SEEK_END) { return ARK_E_INVAL; }
    rc = app_fd_lookup(handle, &slot);
    if (rc != ARK_VFS_OK) { return rc; }
    if (g_app_fds[slot].is_dir != 0u) { return ARK_E_ISDIR; }
    switch (whence)
    {
    case SVCRT_VFS_SEEK_CUR: w = ARK_SEEK_CUR; break;
    case SVCRT_VFS_SEEK_END: w = ARK_SEEK_END; break;
    default:                 w = ARK_SEEK_SET; break;
    }
    rc = ark_vfs_lseek(g_app_fds[slot].fd, (ark_off_t)off, w);
    if (rc < 0) { return rc; }         /* 下层拒绝，位置不动 */
    /* 新偏移自己算：下层回什么（新偏移还是 0）都不影响这里的答案。 */
    {
        int32 base = 0;

        if (w == ARK_SEEK_CUR) { base = (int32)g_app_fds[slot].pos; }
        else if (w == ARK_SEEK_END) { base = (int32)g_app_fds[slot].size; }
        if (((off < 0) && (base < -off)) ||
            ((off > 0) && (base > (int32)0x7FFFFFFF - off)))
        {
            return ARK_E_INVAL;
        }
        g_app_fds[slot].pos = (uint32)(base + off);
        return (int32)g_app_fds[slot].pos;
    }
}

int32 svcrt_vfs_app_readdir(int32 handle, char *name, uint32 cap,
                           uint32 *mode, uint32 *size)
{
    uint32 slot;
    int32 rc;
    struct ark_vfs_dirent ent;
    uint32 n = 0u;

    if ((name == NULL) || (cap == 0u)) { return ARK_E_INVAL; }
    name[0] = '\0';
    rc = app_fd_lookup(handle, &slot);
    if (rc != ARK_VFS_OK) { return rc; }
    if (g_app_fds[slot].is_dir == 0u) { return ARK_E_NOTDIR; }

    rc = ark_vfs_readdir(g_app_fds[slot].fd, &ent);
    if (rc == ARK_E_NOENT) { return 1; }        /* 列完了：不是错误 */
    if (rc != ARK_VFS_OK)  { return rc; }

    while ((ent.name[n] != '\0') && ((n + 1u) < cap))
    {
        name[n] = ent.name[n];
        n++;
    }
    if (ent.name[n] != '\0')
    {
        name[0] = '\0';
        return ARK_E_NAMETOOLONG;   /* 缓冲区装不下整层名字，别回半截 */
    }
    name[n] = '\0';
    if (mode != NULL) { *mode = (uint32)ent.mode; }
    if (size != NULL) { *size = (ent.size > 0) ? (uint32)ent.size : 0u; }
    return 0;
}

int32 svcrt_vfs_app_close(int32 handle)
{
    uint32 slot;
    int32 rc;

    rc = app_fd_lookup(handle, &slot);
    if (rc != ARK_VFS_OK) { return rc; }
    app_slot_release(slot);
    return ARK_VFS_OK;
}

int32 svcrt_vfs_app_stat(const char *path, uint32 *size, uint32 *mode)
{
    struct ark_vfs_stat st;
    int32 rc;

    if (size != NULL) { *size = 0u; }
    if (mode != NULL) { *mode = 0u; }
    if ((g_vfs_ready == 0u) || (path == NULL) || (path[0] == '\0'))
    {
        return ARK_E_INVAL;
    }
    rc = ark_vfs_stat(path, &st);
    if (rc != ARK_VFS_OK) { return rc; }
    if (size != NULL) { *size = (st.size > 0) ? (uint32)st.size : 0u; }
    if (mode != NULL) { *mode = (uint32)st.mode & SVCRT_VFS_S_IFMT; }
    return ARK_VFS_OK;
}

int32 svcrt_vfs_app_unlink(const char *path)
{
    if ((g_vfs_ready == 0u) || (path == NULL) || (path[0] == '\0'))
    {
        return ARK_E_INVAL;
    }
    return ark_vfs_unlink(path);
}

void svcrt_vfs_app_task_exit(uint32 task_id)
{
    uint32 i;

    if (task_id == 0u) { return; }
    app_fd_init();
    for (i = 0u; i < SVCRT_VFS_HANDLE_MAX; i++)
    {
        if ((g_app_fds[i].fd >= 0) && (g_app_fds[i].owner == task_id))
        {
            app_slot_release(i);
        }
    }
}

#endif  /* SVCRT_USE_VFS */
