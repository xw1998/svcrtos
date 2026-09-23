/**
* @file svcrt_vfs.h
* @brief SVCrtOS 的 Linux 风格路径命名空间（基于 ark_vfs）
* @details 内核里原本只有「设备名」这一种名字（svcrt_dev_open("uart0")）和
*          littlefs 的「卷内路径」这一种路径，两者互不相通。本模块把
*          ark_vfs 挂到内核上，让所有资源都落在同一棵目录树里：
*
*            /            易失便签区（ramfs，掉电即失，适合临时数据）
*            /dev         设备注册表的镜像，/dev/uart0 可直接按路径打开
*            /mnt/<名字>  持久卷（littlefs），由板级/命令显式挂载
*
*          路径语义与 Linux 一致：`//` 合并、`.` 去掉、`..` 按前面的段消化，
*          越过根的 `..` 报错而不是悄悄折回根。挂载点用最长前缀匹配，
*          所以 /mnt/nor 会遮住 / 覆盖的那一段。
*
*          【为什么不让内核到处直接 include ark_vfs.h】
*          ark_vfs 是外部组件、版本独立演进；内核只经本文件这一层门面使用
*          它。将来 ark_vfs 换了 API，改的是这一层，不是 shell、不是 App。
*
*          【与 svcrt_fs 的关系】
*          svcrt_fs 是「一个卷 + littlefs」的老门面，仍然可用；本模块把它
*          接成 /mnt 下的一个文件系统，不重写 littlefs、不复制它的缓存。
*          两者的限制也一并继承：svcrt_fs 同时只允许一个写流，所以 /mnt
*          下的写入同一时刻只能有一路（第二个 open 写会拿到 ARK_E_BUSY）。
*
* @author xw
* @date 2026.09.22
*/
#ifndef SVCRT_VFS_H
#define SVCRT_VFS_H

#include "svcrt_types.h"

/* 整个模块由 SVCRT_USE_VFS 开关（见 svcrt_features.h）。关掉后本文件里的
 * 符号仍然存在，但全部返回 SVCRT_VFS_ENOSYS——所以调用方不需要跟着写
 * 条件编译，而「这个构建没有 VFS」与「你的路径不存在」也永远不会被混淆。 */

#ifdef __cplusplus
extern "C" {
#endif

/** 路径缓冲长度，含结尾 0。与 ark_vfs 的 ARK_VFS_PATH_MAX 必须一致 */
#define SVCRT_VFS_PATH_MAX   (64u)

/**
* @brief 本构建未启用 SVCRT_USE_VFS 时，全部门面调用返回此码。
* @note  值与 ark_vfs 的 ARK_E_NOSYS 相同，svcrt_vfs.c 里有一条静态断言
*        盯着这个等式（开启构建里会检查），所以两个数字不会走偏。
*        用法上和 ARK_E_NOSYS 一样：这是「本构建没这个功能」，不是
*        「你写的路径不存在」，报告时不要混为一谈。
*/
#define SVCRT_VFS_ENOSYS     (-12)

/**
* @brief 参数不合法（路径指针/长度/缓冲越出调用者自己的 RAM，或标志组合
*        本身矛盾）。值与 ark_vfs 的 ARK_E_INVAL 相同。
* @note  内核侧的参数校验失败必须回这个码，不能回 -1：-1 是 ARK_E_NOENT，
*        回 -1 会让 App 把「你的路径太长」读成「文件不存在」——那正是
*        「看似权威的错答案」。所以 11..18 各分支的校验失败一律用它。
*/
#define SVCRT_VFS_EINVAL     (-2)

/**
* @brief 挂载点的一行信息，用于 shell 的 mount 命令
* @note  fs/source 两个指针指向只读常量，调用者不得释放或改写。
*/
typedef struct
{
    char        target[SVCRT_VFS_PATH_MAX];   /**< 规范化后的挂载点，如 "/dev" */
    const char *fs;                           /**< 文件系统名，如 "devfs"     */
    const char *source;                       /**< 来源名，如 "nor0"，可为 NULL */
    uint8       ro;                           /**< 1=只读                    */
} svcrt_vfs_mount_info_t;

/** @brief 目录列举回调，返回 0 继续、-1 停止 */
typedef int (*svcrt_vfs_list_cb_t)(const char *name, uint32 is_dir,
                                   uint32 size, void *arg);

/**
* @brief 启用 VFS：装钩子、挂 / 与 /dev。
* @return 0=成功；负值为 ark_vfs 错误码（见 svcrt_vfs_error_name）
* @note  幂等：重复调用不会重复挂载。卷（/mnt/...）不在这里挂——哪块设备
*        上放着文件系统是板级知识，内核不该猜。
*/
int32 svcrt_vfs_init(void);

/** @brief 1=svcrt_vfs_init() 已经成功过 */
uint8 svcrt_vfs_ready(void);

/**
* @brief 把一块设备窗口作为持久卷挂到 target，并同时挂上 littlefs。
* @param dev    已注册的块设备名，如 "nor0"
* @param offset 卷在设备内的起始字节
* @param size   卷长度，0=到设备末尾
* @param target 挂载点，如 "/mnt/nor"
* @return 0=成功；ARK_E_INVAL 参数/窗口非法；ARK_E_NOENT 设备不存在；
*         ARK_E_IO 设备起不来；ARK_E_EXIST 该点已挂或已有卷占用；
*         ARK_E_NOSYS 本构建未启用文件系统或 littlefs 桥（见 svcrt_features.h）
* @note  必须先 svcrt_vfs_init()。卷格式不对（未 format）会返回 ARK_E_IO：
*        挂载不格式化，这是有意的——格式化是破坏性动作，得有人明确下令。
*/
int32 svcrt_vfs_mount_volume(const char *dev, uint32 offset, uint32 size,
                             const char *target);

/** @brief 卸载 target 处的挂载点，并在它是持久卷时同步卸载 littlefs */
int32 svcrt_vfs_umount(const char *target);

/**
* @brief 取第 idx 个挂载点信息。
* @return 0=成功，-1=越界
*/
int32 svcrt_vfs_mount_info(uint32 idx, svcrt_vfs_mount_info_t *out);

/** @brief 挂载点数量 */
int32 svcrt_vfs_mount_count(void);

/** @brief 读整个文件；*out_len 收实际长度（可为 NULL）
 * @note  缓冲区必须装得下整个文件，否则报错而不是给你一半——
 *        要看大小不确定的文件（如 shell 的 cat）用下面三个流式入口。 */
int32 svcrt_vfs_read_file(const char *path, uint8 *buf, uint32 max,
                          uint32 *out_len);

/**
* @brief 按路径只读打开一个文件。
* @return >=0 文件句柄；负值为 ark_vfs 错误码
* @note  只放出只读打开这一种：写清一色走 svcrt_vfs_write_file()，
*        多一个半成品标志集只会让人以为那几种组合都试过了。
*        句柄用 svcrt_vfs_close() 归还，不要跨挂载卸载一直拿着。
*/
int32 svcrt_vfs_open_read(const char *path);

/**
* @brief 从当前偏移读取。
* @return >0 实际读到的字节数；0 文件尾；负值为 ark_vfs 错误码
*/
int32 svcrt_vfs_read(int32 fd, uint8 *buf, uint32 len);

/** @brief 归还句柄；负值为 ark_vfs 错误码 */
int32 svcrt_vfs_close(int32 fd);

/** @brief 写整个文件（创建或截断） */
int32 svcrt_vfs_write_file(const char *path, const uint8 *buf, uint32 len);

/** @brief 删除一个文件 */
int32 svcrt_vfs_remove(const char *path);

/** @brief 列一层目录 */
int32 svcrt_vfs_list(const char *dir, svcrt_vfs_list_cb_t cb, void *arg);

/**
* @brief 把 ark_vfs 错误码翻成短符号名，用于报告。
* @note  报告里必须同时打印数字：符号只是附加说明，不能代替设备/文件系统
*        自己给出的原因。
*/
const char *svcrt_vfs_error_name(int32 rc);

/* ============================================================
 * App 面：带句柄的流式打开（POSIX 的 open / read / write / close）
 *
 * 上面那组是「内核自己用」的入口：要么一次吞掉整个文件，要么拿一个裸的
 * ark_vfs fd。POSIX 程序要的是 open → read/write × N → close，句柄必须在
 * 调用之间活着。
 *
 * 句柄号不直接放 ark_vfs 的 fd 号，而是内核自己发的 token：
 *   1) 归属：token 只对打开它的任务有效，别的任务猜到号码也只会拿到 BADF；
 *   2) 回收：任务退出/崩溃时按任务号把它的句柄全部关掉——ark_vfs 的 fd 表
 *      全机器只有 ARK_VFS_MAX_FDS 张，被一个死掉的 App 占着就是别人打不开
 *      文件。
 * 这两件事是这张表存在的全部理由，所以句柄一律经 svcrt_vfs_app_* 使用，
 * 不要拿它去调上面那些裸 fd 的入口。
 * ============================================================ */

/** @brief open 标志。数值是 SVCrtOS 自己的，svcrt_vfs.c 里逐位映射到
 *         ark_vfs 的 ARK_O_*——App 不 include ark_vfs 的头，所以上游的值
 *         不能当 ABI 用。 */
#define SVCRT_VFS_O_RDONLY     (0x0000u)
#define SVCRT_VFS_O_WRONLY     (0x0001u)
#define SVCRT_VFS_O_RDWR       (0x0002u)
#define SVCRT_VFS_O_ACCMODE    (0x0003u)
#define SVCRT_VFS_O_CREAT      (0x0004u)
#define SVCRT_VFS_O_TRUNC      (0x0008u)
#define SVCRT_VFS_O_APPEND     (0x0010u)
#define SVCRT_VFS_O_DIRECTORY  (0x0020u)

/** @brief stat 的文件类型位，与 POSIX 的 S_IFMT 同形，映射到 ARK_S_IF*。 */
#define SVCRT_VFS_S_IFCHR      (0x2000u)
#define SVCRT_VFS_S_IFDIR      (0x4000u)
#define SVCRT_VFS_S_IFREG      (0x8000u)
#define SVCRT_VFS_S_IFMT       (0xF000u)

/** @brief seek 起点，与 POSIX 的 SEEK_* 同值 */
#define SVCRT_VFS_SEEK_SET     (0u)
#define SVCRT_VFS_SEEK_CUR     (1u)
#define SVCRT_VFS_SEEK_END     (2u)

/** @brief 句柄号的高位标志：句柄 = 标志 | (序号 << 8) | 槽号。
 *         序号 1..255（0 留给「从没用过」），槽号 < 256；槽被回收后重新
 *         发出去时序号加一，所以旧号码在新持有者那里一律 BADF。
 *         与设备句柄的 SVCRT_DEV_HANDLE_FLAG 不同值，两者不会互相误认。 */
#define SVCRT_VFS_HANDLE_FLAG  (0x01300000)

/** @brief 同时打开的 VFS 句柄数。注意底层 ark_vfs 的 fd 表全机器只有
 *         ARK_VFS_MAX_FDS 张，与内核自身（shell 的 cat/ls 等）共用，
 *         所以打满时 open 回 ARK_E_FULL(-3) 而不是静默成功。 */
#define SVCRT_VFS_HANDLE_MAX   (8u)

/**
* @brief 按路径打开一个文件或目录。
* @param path  命名空间路径，绝对路径（"/mnt/nor/a.txt"、"/dev/uart0"、"/tmp"）
* @param flags SVCRT_VFS_O_* 的组合
* @return 0 以上是句柄；负值为 ark_vfs 错误码
* @note  带 SVCRT_VFS_O_DIRECTORY 时打开的是目录（用于 readdir）。
*        目录只能只读打开。
*/
int32 svcrt_vfs_app_open(const char *path, uint32 flags);

/** @brief 从当前偏移读。返回值 = 读到的字节数，0 = 文件尾，负值 = 错误 */
int32 svcrt_vfs_app_read(int32 handle, uint8 *buf, uint32 len);

/** @brief 从当前偏移写。返回值 = 写入的字节数，负值 = 错误 */
int32 svcrt_vfs_app_write(int32 handle, const uint8 *buf, uint32 len);

/**
* @brief 移动读写偏移。whence 用 SVCRT_VFS_SEEK_*。
* @return 新的绝对偏移（>= 0）；负值为错误码，且失败时偏移不动。
* @note  返回的是内核自己记账的位置，不是下层文件系统的返回值——
*        ramfs 与 littlefs 桥对 seek 的返回约定不一样（前者回新偏移、
*        后者回 0），直接用下层的会让 lseek 在某个卷上静默回错值。
*        允许 seek 越过文件尾（POSIX 语义）；能不能写在那里由下层决定。
*        越界或非法的移动由下层拒绝，此时位置保持不变。
*/
int32 svcrt_vfs_app_seek(int32 handle, int32 off, uint32 whence);

/**
* @brief 取目录里的下一条。
* @param name  输出名字的缓冲区（不含路径，只一层）
* @param mode  收文件类型位（可 NULL）
* @param size  收文件大小（可 NULL）
* @return 0 = 取到一条；1 = 目录已列完（不是错误，POSIX 的 readdir 在这里回 NULL）；
*         负值 = 错误
*/
int32 svcrt_vfs_app_readdir(int32 handle, char *name, uint32 cap,
                           uint32 *mode, uint32 *size);

/** @brief 归还句柄。目录句柄与文件句柄走同一个入口 */
int32 svcrt_vfs_app_close(int32 handle);

/** @brief 按路径取类型与大小（不打开）。mode 收 SVCRT_VFS_S_IF* */
int32 svcrt_vfs_app_stat(const char *path, uint32 *size, uint32 *mode);

/** @brief 删除一个文件（目录不可用这个入口删） */
int32 svcrt_vfs_app_unlink(const char *path);

/**
* @brief 关掉 task_id 名下全部句柄。任务退出/崩溃路径调用。
* @note  必须在任务还被标记为有效时调用（表里靠任务号比对），且对没有
*        句柄的任务是无副作用的空操作。
*/
void svcrt_vfs_app_task_exit(uint32 task_id);

#ifdef __cplusplus
}
#endif

#endif /* SVCRT_VFS_H */
