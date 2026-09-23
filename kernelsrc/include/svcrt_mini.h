/**
* @file svcrt_mini.h
* @brief SVCrtOS 小程序（MiniApp）：文件系统里的负载按需 load 进 RAM 执行
* @details 小程序与 App 的关系，一句话说清：
*          - 容器相同：同一种镜像（256 字节头 + 重定位表 + 负载），同一种入口约定，
*            同一个 App SDK（oslib 直接复用）。差别只在镜像头里的 type 字段。
*          - 交付方式不同：App 要先「安装」——由加载器写进 Flash 镜像池、占一个
*            槽位、绑定一块 RAM；小程序**不安装**，它就是文件系统里的一个文件，
*            运行期现读出来、现借一块 RAM、跑完（或停掉）就把这块 RAM 还回池。
*          - 代价是明确的：每次运行都要重读一遍负载，代码只能从 RAM 执行（无法
*            XIP），所以比 App 多花「一次装载时间 + 一块等于 代码+RAM 的池内存」。
*
*          内存下限（这是数学下限，不是实现水平）：无 MMU 的机器上没有 ELF 那种
*          「按页映射、只映射真正用到的部分」；ELF 在 MCU 上的等价物是 XIP——
*          代码留在 Flash 里原地执行，而现有 App 模型已经在用 XIP。既然要求
*          「load 进内存再执行」，代码就必须占 RAM，于是占用是两块：
*              pow2_ceil(align8(负载长度))  +  pow2_ceil(镜像声明的 RAM 需求)
*          **分成两块而不是塞进一个 pow2 块**：池的分配粒度就是 2 的幂，把两者
*          合并成 pow2_ceil(代码 + RAM) 会在最常见的「代码远小于 RAM」时接近
*          翻倍——代码 1KB + RAM 8KB 会要一块 16KB，而两块只要 1KB + 8KB。
*          两块各自对齐自己的大小，正好各占一个 MPU 区域（代码窗 / 数据窗），
*          数据窗也就不用再迁就代码窗的「可执行」属性。
*          再往下只剩两处可省（合并两块的写法省不掉它们，见上）：
*            1. 头与重定位表不落 RAM（读完即丢；负载直接读进代码块基址）；
*            2. 不设第二份中转缓冲（读一段、打一段补丁）。
*
* @author xw
* @date 2026.09.23
*/
#ifndef __SVCRT_MINI_H__
#define __SVCRT_MINI_H__

#include "svcrt_def.h"

/* 只有这一个错误码是本路径独有的；其余全部沿用 SVCRT_LOADER_ERR_x
 * （MAGIC / COMPAT / SIZE / CRC / RELOC / ENTRY / TASK / STATE / NOSPACE / BUSY），
 * 免得同一种错误在两条装载路径上报出两个名字。 */
#define SVCRT_MINI_ERR_FS      (-18)   /* 读镜像文件失败：原因见 svcrt_fs_last_error() */

/* 这个小程序因「连续故障」已被内核禁用：不再装载，直到有人显式放行
 * （shell: mini forget <路径>）或换一份镜像。判定阈值与带槽位的 App 完全
 * 同一套（分区表里的 cfg_restart_max），不是小程序专用的另一个数字。 */
#define SVCRT_MINI_ERR_DISABLED (-19)

/* 镜像路径的最大字节数（含结尾 0）。这是「身份」的长度上限：小程序没有槽位，
 * 崩溃记账按路径的哈希归属，路径必须能被完整存进实例表里（否则两次不同的
 * 路径会截断成同一个身份，把 A 的崩溃算到 B 头上）。 */
#define SVCRT_MINI_PATH_MAX    (64u)

/* 开机自启清单：卷内一个纯文本文件，一行一个镜像路径（写法与 `mini run` 完全
 * 相同，必须以 '/' 开头），'#' 开头是注释，空行忽略。**不在清单里 = 开机不启动**
 * （默认不启动；小程序是被显式叫起来的，不是装上就跑）。
 *
 * 为什么是清单文件、而不是镜像头里的 AUTOSTART 标志：小程序的身份本来就是路径
 * （崩溃记账按路径哈希），清单用的是同一个身份，不是第二套；而且开关自启只要
 * 改一行文本，不必改写镜像——改镜像头会让它自己的 CRC 失效，为改一个位去重写
 * 整个镜像，代价与风险都不成比例（写坏了就是一份跑不起来的镜像）。 */
#define SVCRT_MINI_AUTOSTART_FILE   "/mini.autostart"

/* 清单文件的大小上限（字节）。一条 SVCRT_MINI_PATH_MAX-1 字节的路径约 15 行；
 * 超出的条目不会被读进来，新增时由 svcrt_mini_autostart_set() 明确报 NOSPACE，
 * 而不是把清单截断成一份看起来还在、实际少了几条的文件。 */
#define SVCRT_MINI_AUTOSTART_MAX    (1024u)

/** @brief 一次小程序装载的结果（svcrt_mini_info() 的输出） */
typedef struct {
    uint32 task_id;      /* 主任务号（1 起）；0 = 没有在跑 */
    uint32 code_base;    /* 代码块基址（2 的幂对齐） */
    uint32 code_block;   /* 代码块字节数（2 的幂，>= code_size） */
    uint32 code_size;    /* 负载长度（镜像头里的 image_size） */
    uint32 ram_base;     /* RAM 块基址 = 镜像 RW/ZI 与栈的运行起点 */
    uint32 ram_size;     /* RAM 块字节数（2 的幂，含 RW/ZI + 栈） */
    uint32 entry;        /* 入口地址（已按 Cortex-M 约定带上 Thumb 位） */
    uint32 crc;          /* 本次装载算出的镜像 CRC（与镜像头里的值一致） */
} svcrt_mini_info_t;

/**
* @brief 从文件系统装载并启动一个小程序
* @param path 串口侧路径：文件系统卷内的绝对路径，如 "/mini/hello.svcm"
* @return 0=已启动；负值为 SVCRT_LOADER_ERR_x、SVCRT_MINI_ERR_FS 或
*         SVCRT_MINI_ERR_DISABLED（这个程序崩溃到上限，已被禁用）
* @note 同一时刻能跑几个由 SVCRT_MINI_MAX（编译期上限）与运行期收紧值共同
*       决定；项满了报 BUSY，**不会**挤掉正在跑的那个、也不会静默共用一个块。
* @note 装载本身是串行的：装载期间持有调度器锁（与 SVC 层访问文件系统同一
*       套约定），文件系统只有一份缓存，两个任务交错读镜像会把彼此的窗口
*       顶偏。所以「多个」是**同时运行**多个，不是同时装载多个。
*/
int32 svcrt_mini_run(const char *path);

/**
* @brief 停止**全部**在跑的小程序并归还它们的 RAM 块
* @return 0=至少停掉了一个；SVCRT_LOADER_ERR_STATE=本来就没有在跑
* @note 逐个停（shell 想只停某一个用 svcrt_mini_stop_at）
* @details 停止顺序不能反：先把镜像（含它自己创建的线程）踢出调度，再归还块。
*          反过来的话，块可能立刻被下一个装载者拿去写代码，而原任务还在跑。
*/
int32 svcrt_mini_stop(void);

/**
* @brief 读出第一个在跑的小程序的状态（兼容旧调用）
* @return 0=有在跑的（*p_out 有效）；-1=没有在跑（*p_out 已清零）
* @note 要列出全部用 svcrt_mini_running() + svcrt_mini_info_at()。
*/
int32 svcrt_mini_info(svcrt_mini_info_t *p_out);

/**
* @brief 当前在跑的小程序个数（0 = 没有）
* @return 运行中的实例数
* @note 这是「密集编号」的上界：编号 0 .. 返回值-1 都可用 svcrt_mini_info_at 查到
*       （编号按实例池顺序排列，中间不会因为停下来而空号）。
*/
uint32 svcrt_mini_running(void);

/**
* @brief 按下标读一个小程序的状态
* @param index 0 .. svcrt_mini_running()-1（密集编号，不是实例池下标）
* @return 0=*p_out 有效；-1=没有这么多在跑的
*/
int32 svcrt_mini_info_at(uint32 index, svcrt_mini_info_t *p_out);



/**
* @brief 停止其中一个（按 svcrt_mini_info_at 的同一个编号）
* @return 0=已停止；SVCRT_LOADER_ERR_STATE=这个编号没有在跑的
*/
int32 svcrt_mini_stop_at(uint32 index);

/**
* @brief 读 / 写「同一时刻最多跑几个」的运行期上限
* @return svcrt_mini_set_max_count: 0=已生效，SVCRT_LOADER_ERR_PARAM=超出 [1, SVCRT_MINI_MAX]
* @details 上限不能在运行期放宽到超过编译期的 SVCRT_MINI_MAX：那是个会把
*          「说好了 8 个」变成一句看不出来的谎话的设置（结构体、共享表数组、
*          每次崩溃记账的容量都是按编译期值算的），所以超范围直接报错。
*/
uint32 svcrt_mini_max_count(void);
int32  svcrt_mini_set_max_count(uint32 n);

/**
* @brief 清除某个路径的崩溃记忆（让被禁用的小程序重新可跑）
* @param path 与 svcrt_mini_run 同一个路径
* @return 0=已清；-1=路径为空
*/
int32 svcrt_mini_forget(const char *path);

/**
* @brief 故障钩子（由 svcrt_loader_on_fault 对「没有槽位的任务」调用）
* @return 1=这是某个小程序的主任务，已按小程序的规则记账（可能已禁用）；
*         0=与本模块无关，调用方按原来的默认故障处理走
*/
int32 svcrt_mini_on_fault(int32 task_id);

/**
* @brief 读出「单个小程序能占用的最大块字节数」这个运行期上限
* @return 当前上限（字节，2 的幂）
* @details 默认值就是编译期的 SLOT_RAM_MAX_BLOCK。这个上限**可以在运行期收紧**
*          （shell: mini limit <字节>），但**不能放宽到超过 SLOT_RAM_MAX_BLOCK**：
*          那个常量同时还是池单块物理上限、App 镜像校验上限与 MPU 能给出的数据
*          窗口上限（SVCRT_MPU_RAM_BLOCK_MAX）。往上放会掉进 svcrt_mpu.c 的
*          「内核 RAM 兜底窗口」分支——那是一种会静默把任务放进错误窗口的失败，
*          不应该由一次调参触发。要真正放开天花板，得改 SLOT_RAM_MAX_BLOCK
*          并同时满足池与 MPU 的静态断言。
*/
uint32 svcrt_mini_max_bytes(void);

/**
* @brief 设置单个小程序的体积上限（块的字节数）
* @param bytes 2 的幂，范围 [SLOT_RAM_MIN_BLOCK, SLOT_RAM_MAX_BLOCK]；0 = 恢复编译期默认
* @return 0=已生效；SVCRT_LOADER_ERR_PARAM=不在范围内或不是 2 的幂（不静默夹取）
* @note 只影响**下一次**装载；已经在跑的小程序不受影响（它的块已经借出去了）。
*/
int32 svcrt_mini_set_max_bytes(uint32 bytes);

/**
* @brief 任务退出路径的回收钩子（由 svcrt_task_kill_internal 调用）
* @param task_id 正在退出的任务号
* @return 1=这是小程序的主任务（本函数已停掉它的线程并归还块）；
*         0=与本模块无关（不是小程序，或退出的是小程序内部的子线程）
* @details 主任务退出 = 整个小程序结束。子线程退出不动块：小程序可能只是
*          收掉了一个 worker，主任务还要继续跑。
*/
int32 svcrt_mini_on_task_exit(uint32 task_id);

/**
* @brief 遍历自启清单（只读）；callback 返回 0 继续、-1 提前结束
* @param cb 每个条目调一次，path 是已去掉首尾空白的路径（'#' 注释与空行不回调）
* @return 0=遍历完成；-1=清单读不了（不存在也是 -1，见 svcrt_mini_autostart_now）
* @details 超长（> SVCRT_MINI_PATH_MAX-1）的条目**不截断也不回调**：截断会让两个
*          不同路径共享同一个身份，崩溃记账会串号。该条目会被记一条错误日志后跳过。
*/
typedef int (*svcrt_mini_auto_cb_t)(const char *path, void *arg);
int32 svcrt_mini_autostart_foreach(svcrt_mini_auto_cb_t cb, void *arg);

/**
* @brief 按自启清单启动一遍
* @return >=0=成功启动的个数；SVCRT_MINI_ERR_FS=清单读不了
* @note 逐条调 svcrt_mini_run()，所以并发上限、崩溃禁用、体积上限这些判据
*       与手工 `mini run` 完全同一条路径，不另立一套。
*/
int32 svcrt_mini_autostart_now(void);

/**
* @brief 卷刚挂上时的自启入口（内核在挂载成功后调用）
* @return 本次真正启动的个数；本次开机已经做过（或清单读不到）则为 0
* @details 只在本次开机的**第一次成功挂载**上做一次。小程序的可执行字节就在这个
*          卷里，所以「卷可用」是它唯一能起来的时刻；内核自己不去猜哪个设备上有
*          文件系统（那是板级知识），挂载由上层发起——谁把它挂上，就在这里被带上。
*/
uint32 svcrt_mini_boot_autostart(void);

/**
* @brief 把 path 加进（on!=0）/移出（on==0）自启清单
* @param path 与 `mini run` 同一种写法（必须以 '/' 开头的卷内路径）
* @return 0=已生效（含「本来就是这样、未改动」）；SVCRT_LOADER_ERR_PARAM=路径不合法；
*         SVCRT_LOADER_ERR_NOSPACE=清单文件装不下；SVCRT_MINI_ERR_FS=写盘失败
* @note 空改动不落盘：已经在清单里再加、或本来就不在清单里再删，都不重写文件。
*/
int32 svcrt_mini_autostart_set(const char *path, uint32 on);

#endif /* __SVCRT_MINI_H__ */
