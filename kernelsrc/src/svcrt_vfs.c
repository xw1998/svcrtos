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

/* 两个头文件对「路径最长多少」必须说同一句话，否则会在边界上错一位 */
typedef char svcrt_vfs_pathmax_check[(SVCRT_VFS_PATH_MAX == ARK_VFS_PATH_MAX) ? 1 : -1];

/* 关闭构建里门面返回的那个数字，与 ark_vfs 的 NOSYS 必须是同一个 */
typedef char svcrt_vfs_enosys_check[(SVCRT_VFS_ENOSYS == ARK_E_NOSYS) ? 1 : -1];

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

    if (svcrt_fs_mounted() == 0u)
    {
        if (svcrt_fs_mount(dev, offset, size) != 0)
        {
            int32 raw = svcrt_fs_last_error();
            /* 挂载不格式化：卷没格式化就该报错，不该顺手擦掉别人的数据 */
            SVCRT_LOGE("vfs", "svcrt_fs_mount(%s) failed: last_error=%d (%s)",
                       dev, (int)raw, svcrt_fs_error_name(raw));
            return ARK_E_IO;
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

#endif  /* SVCRT_USE_VFS */
