/**
* @brief SVCrtOS 功能选配表（feature gate）
* @details 这个文件只回答一个问题：**这次构建要哪些功能**。
*          它与另外三层正交，写新功能前先确认自己属于哪一层：
*            - 能力层 board/<chip>/svcrt_board_config.h：这块板/芯片**有什么**
*              （FPU / MPU / 特权级 / 时钟 / 看门狗）。硬件事实，内核只给兜底默认值。
*            - 契约层 kernelsrc/include/svcrt_hal.h：内核要求硬件**提供什么接口**。
*            - 布局层 config/svcrt_partition.h：东西**放在哪个地址**。
*          能力层的三个开关（SVCRT_USE_FPU / SVCRT_USE_MPU / SVCRT_USE_PRIV）
*          不在本文件，它们紧跟架构派生留在 svcrt_config.h 与板级配置里。
*
*          四条规则（新增功能时必须遵守）：
*            1. 任何"不是每个用户都想要"的复杂功能，必须在这里有开关；
*            2. 默认值写在 #ifndef 里，板级或工程可用 -D 覆盖；
*            3. 关掉一个开关后 kernelsrc 必须仍能编译通过（CI 门禁会做裁剪构建）；
*            4. 开关之间的依赖用 #error 显式报出，不要让它烂成链接错误。
*
* @note 时序：svcrt_config.h 先 include SVCRT_BOARD_CONFIG，再 include 本文件，
*       所以板级的 #undef/#define 对这里生效；本文件在 svcrt_arch.h 之后被包含。
*/

#ifndef __SVCRT_FEATURES_H__
#define __SVCRT_FEATURES_H__

/* ============================================================
 * 一、内核基础机制
 * ============================================================ */

/* 内核自旋锁（svcrt_spin.h）。关掉后不是没有锁，而是换成纯关中断的
 * 降级实现：单核下够用，省掉原子 CAS 那套 SMP 预留。
 * 不要把它当纯预留开关——对象表、设备表都用它，置 0 仍有锁语义。 */
#ifndef SVCRT_USE_SPINLOCK
#define SVCRT_USE_SPINLOCK            1
#endif

/* 用户态调度器锁：禁止任务切换的轻量临界区（SVC 0x1B 的 lock/unlock）。
 * 这不是可选项：锁语义没有诚实的降级方式——要么真的不切任务，要么整个
 * 调用作废——内核侧的调用点也就没有关闭分支。置 0 直接报错，别让它
 * 静默退化成一个「永远成功」的空锁。 */
#ifndef SVCRT_USE_SCHED_LOCK
#define SVCRT_USE_SCHED_LOCK          1
#endif
#if (SVCRT_USE_SCHED_LOCK != 1)
#error "SVCRT_USE_SCHED_LOCK must stay 1: the scheduler lock has no honest fallback."
#endif

/* 任务栈用量统计：填充图案 + 最低栈指针双手段，算每任务峰值 */
#ifndef SVCRT_USE_STACK_USAGE
#define SVCRT_USE_STACK_USAGE         1
#endif

/* 栈溢出检测：靠栈尾标志被踩判定 */
#ifndef SVCRT_USE_STACK_CHECK
#define SVCRT_USE_STACK_CHECK         1
#endif

/* 调度器统计：每任务切换次数与耗时 */
#ifndef SVCRT_USE_SCHED_STAT
#define SVCRT_USE_SCHED_STAT          1
#endif

/* CPU 负载统计：实时空闲率 */
#ifndef SVCRT_USE_CPU_LOAD
#define SVCRT_USE_CPU_LOAD            1
#endif

/* 节拍按需拉 PendSV：没有更高优先级就绪任务时不触发切换中断 */
#ifndef SVCRT_USE_FAST_TICK_SWITCH
#define SVCRT_USE_FAST_TICK_SWITCH    1
#endif

/* 故障任务恢复：异常路径回收槽位与资源 */
#ifndef SVCRT_USE_FAULT_RECOVER
#define SVCRT_USE_FAULT_RECOVER       1
#endif

/* MPU 上下文差异应用缓存：同一任务重复进入时不重写 MPU 寄存器 */
#ifndef SVCRT_USE_MPU_APPLY_CACHE
#define SVCRT_USE_MPU_APPLY_CACHE     1
#endif

/* ============================================================
 * 二、同步原语与 IPC
 * @note 信号量 / 互斥锁 / 条件变量 / 事件组属于内核核心，不提供关断：
 *       设备框架、POSIX 层、加载器都建立在它们之上，关掉会连锁。
 * ============================================================ */

/* 消息队列（SVC 0x16） */
#ifndef SVCRT_USE_MQ
#define SVCRT_USE_MQ                  1
#endif

/* 软件定时器（SVC 0x17） */
#ifndef SVCRT_USE_TIMER
#define SVCRT_USE_TIMER               1
#endif

/* ============================================================
 * 三、服务与组件
 * ============================================================ */

/* 内核日志（svcrt_log.c）。Shell 的 log 命令依赖它，关掉后该命令一起消失 */
#ifndef SVCRT_USE_LOG
#define SVCRT_USE_LOG                 1
#endif

/* 块设备层（svcrt_blk.c）：设备注册表与文件系统之间的薄层 */
#ifndef SVCRT_USE_BLK
#define SVCRT_USE_BLK                 1
#endif

/* 文件系统门面 + littlefs（svcrt_fs.c，SVC 0x1C）。依赖 SVCRT_USE_BLK */
#ifndef SVCRT_USE_FS
#define SVCRT_USE_FS                  1
#endif

/* Linux 风格路径命名空间（svcrt_vfs.c + components/ark_vfs）：/ 用 ramfs、
 * /dev 是设备注册表的镜像、/mnt/<名字> 是持久卷。shell 的 mount/ls/cat
 * 与 App 的按路径访问共用这一层。关掉后门面本身还在（返回 NOSYS），
 * 但内核里不再有任何资源能按路径找到——所以实际上就是没有 VFS。
 * 依赖 SVCRT_USE_BLK：持久卷后端是把 svcrt_blk 的一段窗口包成 ark_vfs
 * 的 backend，本构建不提供「无块设备的 VFS」这种变体。 */
#ifndef SVCRT_USE_VFS
#define SVCRT_USE_VFS                 1
#endif

/* VFS 的 littlefs 后端（svcrt_vfs_lfs.c）：把 /mnt/<名字> 接到 svcrt_fs。
 * 关掉后 /mnt 下仍可挂调用者自己实现的 fsdrv，只是内核不再自带持久卷桥 */
#ifndef SVCRT_USE_VFS_LFS
#define SVCRT_USE_VFS_LFS             1
#endif

/* 内核 Shell 控制台（SVC 0x1A）的开关不在这里，而是沿用既有的
 * SHELL_ENABLE（config/svcrt_partition.h）；svcrt_shell.c 的整文件门控
 * 与关闭时的空实现早已就位。不要在此再定义第二个 shell 开关：两个名字
 * 管一件事，迟早会出现「看着关了其实还开着」。 */

/* 小程序（svcrt_mini.c）：把文件系统里的一份负载按需 load 进 RAM 执行，
 * 退出或停止即把整块 RAM 还回档位池。它复用 App 的镜像容器与 App SDK，
 * 但**不进镜像池、不占槽位**：每次运行现从文件系统读、现借一块 RAM、
 * 跑完即还——牺牲效率换「放上就能跑、不用安装」。
 * 依赖 SVCRT_USE_FS：镜像就放在文件系统里，没有文件系统就没有小程序。 */
#ifndef SVCRT_USE_MINIAPP
#define SVCRT_USE_MINIAPP             1
#endif

/* POSIX / Windows 兼容层（kernelsrc/sdk/posix）。关掉后 App 只能用
 * 原生 oslib，不能 include <pthread.h> 那一套 */
#ifndef SVCRT_USE_POSIX
#define SVCRT_USE_POSIX               1
#endif

/* POSIX 层把 malloc/free 映射到自己的堆 */
#ifndef SVCRT_POSIX_WRAP_STDLIB
#define SVCRT_POSIX_WRAP_STDLIB       0
#endif

/* MDK trace 采集（components/mdk_trace，依赖 DWT/ITM 调试硬件） */
#ifndef SVCRT_USE_MDK_TRACE
#define SVCRT_USE_MDK_TRACE           1
#endif

/* cm_backtrace 故障回溯（components/cm_backtrace，依赖 DWT/NVIC） */
#ifndef SVCRT_USE_CM_BACKTRACE
#define SVCRT_USE_CM_BACKTRACE        1
#endif

/* ============================================================
 * 四、可靠性与运维
 * ============================================================ */

/* 跨复位崩溃日记（svcrt_crash.c）：落在共享内存尾部的 .noinit 区 */
#ifndef SVCRT_USE_CRASH_LOG
#define SVCRT_USE_CRASH_LOG           1
#endif

/* 心跳合同与看门狗监督（svcrt_guard.c）。需要板级 SVCRT_WDG_ENABLE 才真咬人 */
#ifndef SVCRT_USE_KERNEL_GUARD
#define SVCRT_USE_KERNEL_GUARD        1
#endif

/* 结构自检（svcrt_audit.c）：分区表 / 槽位 / 镜像头的一致性检查与审计记录。
 * 关掉后加载器仍会做最低限度的合法性判断，只是不再逐项记账 */
#ifndef SVCRT_USE_AUDIT
#define SVCRT_USE_AUDIT               1
#endif

/* ============================================================
 * 五、依赖校验
 * @brief 关错了开关要在这里报出来，而不是等到链接期一个 undefined symbol
 * ============================================================ */

#if SVCRT_USE_FS && !SVCRT_USE_BLK
#error "SVCRT_USE_FS=1 requires SVCRT_USE_BLK=1 (file system sits on the block layer)"
#endif

#if SVCRT_USE_MINIAPP && !SVCRT_USE_FS
#error "SVCRT_USE_MINIAPP=1 requires SVCRT_USE_FS=1 (a MiniApp is loaded from the file system)"
#endif

#if SVCRT_USE_VFS && !SVCRT_USE_BLK
#error "SVCRT_USE_VFS=1 requires SVCRT_USE_BLK=1 (the port wraps an svcrt_blk window as the ark_vfs backend)"
#endif

#if SVCRT_USE_VFS_LFS && !SVCRT_USE_VFS
#error "SVCRT_USE_VFS_LFS=1 requires SVCRT_USE_VFS=1 (the bridge plugs into the VFS namespace)"
#endif

#if SVCRT_USE_VFS_LFS && !SVCRT_USE_FS
#error "SVCRT_USE_VFS_LFS=1 requires SVCRT_USE_FS=1 (the bridge drives the svcrt_fs volume)"
#endif

#endif /* __SVCRT_FEATURES_H__ */
