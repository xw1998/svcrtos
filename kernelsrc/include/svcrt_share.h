/**
* @file svcrt_share.h
* @brief SVCrtOS 共享内存分区表（内核 / Loader / App 运行期接口）
* @details 分区布局在编译期由 config/svcrt_partition.h 唯一定义，但该头文件
*          **只允许内核工程包含**。Loader 与 App 工程在运行期通过共享内存中的
*          本结构体获取布局信息（SVC 0x18 子命令 1 返回其地址），
*          从而实现「编译期解耦、运行期可用」。
*
* @note 本文件不得包含任何地址宏，也不得包含 svcrt_partition.h。
*/

#ifndef __SVCRT_SHARE_H__
#define __SVCRT_SHARE_H__

#include "svcrt_types.h"

/** @brief 分区表魔数 "PART"，用于校验共享内存中的分区表是否有效 */
#define SVCRT_PARTITION_MAGIC     (0x50415254u)

/** @brief 分区表结构版本。
 *  v1 -> v2: driver 区状态由「单槽四个平铺字段」改为「与 App 同构的槽位数组」。
 *  v2 -> v3: 末尾追加每槽 autostart 标志。
 *  v3 -> v4: 驱动区与 App 区合并为**统一镜像池**，槽位表按「镜像」而非
 *            「固定分区槽」组织：每条记录带自己的类型（App/驱动）、基址与
 *            占用单元数。
 *  v4 -> v5: 池分配改为细粒度字节粒度（pool_alloc_unit），镜像可被搬移与
 *            压实（新增 pool_usable_size / pool_reserve_sectors）；记录的
 *            「占用单元数」换成「字节数」；镜像 RAM 由伙伴分配器按镜像
 *            声明的 ram_size 现算，因此每条记录新增 ram_base / ram_size，
 *            且槽位数组长度从 8 扩到 16。布局字段与记录字段再次全变，
 *            版本必须升位。
 *  v5 -> v6: 新增设备端布局配置区（安装模式 + 固定槽位表），分区表追加
 *            layout_mode / layout_source / config_base / config_size /
 *            cfg_slot_count 五个字段；Flash 布局变为
 *            BOOT -> KERNEL -> CONFIG -> IMAGE_POOL，池基址随之后移
 *            （旧镜像与旧工具必须同步升级），硬件兼容签名低 16 位升到 5。
 *  v6 -> v7: 设备端配置区里原本只能写 0 的两个旋钮开始生效——
 *            boot_delay_ms（自启前的调试器挂接窗口）与 flags 的
 *            RAW_ALLOW 位（是否接受池内直接烧录的裸镜像）。分区表末尾
 *            追加 cfg_boot_delay_ms / cfg_raw_allow 两个生效值，
 *            让上位机能一眼看出设备到底按哪套配置在跑。
 *  v7 -> v8: 小程序（MiniApp，见 svcrt_mini.h）需要一个**瞬态** RAM 块：
 *            运行时从镜像 RAM 池借、退出即归还，因此它不属于任何镜像槽位，
 *            没法记在 slot_ram_* 里。末尾追加 mini_ram_base / mini_ram_size
 *            两个字段专门给这个块记账，伙伴分配器把非零的它当作一个占用
 *            区间（同一时刻最多一个小程序，所以一组字段就够）。
 *            纯追加，已有字段偏移不变；硬件兼容签名不受影响（那是镜像侧的
 *            代号，与分区表 ABI 无关）。
 *  v8 -> v9: 小程序的内存布局由「一个 pow2 块」改成「代码块 + RAM 块」两块。
 *            原来把代码与 RW/ZI+栈塞进同一块、块大小取
 *            pow2_ceil(代码 + RAM)：pow2 是池的分配粒度，于是最常见的
 *            「代码远小于 RAM」会被放大到接近两倍——代码 1KB + RAM 8KB 要
 *            一块 16KB，分两块只要 1KB + 8KB。两块各自取 pow2、各自对齐
 *            自己的大小，正好各占一个 MPU 区域（代码窗 / 数据窗），
 *            数据窗也就不必再与代码窗共用「可执行」属性。
 *            末尾追加 mini_code_base / mini_code_size；mini_ram_* 含义不变。
 *            纯追加，已有字段偏移不变。
 */
#define SVCRT_PARTITION_VERSION   (9u)

/** @brief 槽位数组的固定长度（ABI 形状常量）。
 *  实际使用的槽位数由运行期字段 slot_max 决定，必须 <= 本值；
 *  数组长度写死是为了让结构体尺寸与偏移跨版本稳定。 */
#define SVCRT_SLOT_ARRAY_MAX      (16u)

/** @brief 槽位类型（与 svcrt_app_image.h 的 SVCRT_APP_TYPE_x 同值） */
#define SVCRT_SLOT_FREE           (0u)   /* 记录未使用，对应 Flash 区间可被分配 */
#define SVCRT_SLOT_APP            (1u)   /* 用户应用镜像 */
#define SVCRT_SLOT_DRIVER         (2u)   /* 用户驱动镜像 */

/** @brief 槽位状态 */
#define SVCRT_APP_SLOT_EMPTY      (0u)    /* 空槽位 */
#define SVCRT_APP_SLOT_LOADED     (1u)    /* 已写入镜像并通过校验 */
#define SVCRT_APP_SLOT_RUNNING    (2u)    /* 已注册为任务并运行 */
#define SVCRT_APP_SLOT_INVALID    (3u)    /* 槽位有内容但校验失败（魔数/兼容签名/CRC 不符） */
#define SVCRT_APP_SLOT_INSTALLING (4u)    /* 正在安装：写入未完成，不可启动（掉电后由 CRC 判定为 INVALID） */
#define SVCRT_APP_SLOT_RAW        (5u)    /* 开发期裸镜像（无镜像头）：占用区间不参与分配，也不参与压实 */

/**
* @brief 分区表（存放于共享内存起始处）
* @details 由内核在启动时填充静态布局，运行期由 Loader 更新槽位状态。
*          Loader / App 只读布局字段，写状态字段前须确认自己拥有该槽位。
*
*          Flash 统一镜像池：pool_base 起、pool_size 字节，按
*          pool_alloc_unit 粒度做首适配分配（镜像向后紧邻排列），
*          尾部 pool_reserve_sectors 个扇区始终留空作为压实的搬移余量。
*          pool_sector 只是芯片的物理擦除单位，与分配粒度无关。
*
*          RAM：镜像的 .data/.bss/栈由伙伴分配器从 slot_ram_base 起、
*          slot_ram_total 字节的池里按镜像头声明的 ram_size 切块，
*          块大小落在 [slot_ram_min_block, slot_ram_max_block]。
*          每条记录带自己的 ram_base / ram_size，两块镜像永不共用 RAM。
*/
typedef struct {
    uint32 magic;               /* SVCRT_PARTITION_MAGIC */
    uint32 version;             /* SVCRT_PARTITION_VERSION */
    uint32 hw_compat_id;        /* 硬件兼容签名（与 App 镜像头比对） */

    /* ---- Flash 分区 ---- */
    uint32 kernel_base;
    uint32 kernel_size;
    uint32 pool_base;           /* 统一镜像池（App 与驱动共用）起始地址 */
    uint32 pool_size;           /* 池总字节数 */
    uint32 pool_usable_size;    /* 允许分配给镜像的字节数（池总长 - 压实余量） */
    uint32 pool_alloc_unit;     /* 分配粒度 / 落点对齐单位（字节） */
    uint32 pool_sector;         /* 芯片物理擦除单位（字节） */
    uint32 pool_units;          /* 池包含的物理扇区个数 = pool_size / pool_sector */
    uint32 pool_reserve;        /* 尾部保留的扇区数（压实余量） */
    uint32 slot_max;            /* 槽位记录条数（<= SVCRT_SLOT_ARRAY_MAX） */
    /* ---- 安装策略与设备端布局配置（v6 新增） ---- */
    uint32 layout_mode;         /* SVCRT_LAYOUT_MODE_x（当前生效：固定槽位 / 自动选址） */
    uint32 layout_source;       /* SVCRT_LAYOUT_SOURCE_x（编译期默认 / 设备端配置区） */
    uint32 config_base;         /* 布局配置区起始地址（0 = 未划分） */
    uint32 config_size;         /* 布局配置区字节数（0 = 未划分） */
    uint32 cfg_slot_count;      /* 生效的固定槽表条目数（自动选址模式为 0） */
    uint32 reclaim_mode;        /* SVCRT_CFG_RECLAIM_x：卸载回收力度（AUTO 模式有效） */
    uint32 cfg_log_level;       /* 运行期日志级别生效值 */
    uint32 cfg_restart_max;     /* 连续故障重启上限生效值 */

    /* ---- RAM 分区 ---- */
    uint32 share_ram_base;
    uint32 share_ram_size;
    uint32 kernel_ram_base;
    uint32 kernel_ram_size;
    uint32 image_ram_base;      /* 镜像 RAM 池起始地址（按 2 的幂分配） */
    uint32 image_ram_total;     /* 镜像 RAM 池总字节数 */
    uint32 image_ram_min_block; /* 最小分配块（字节，2 的幂） */
    uint32 image_ram_max_block; /* 最大分配块（字节，2 的幂） */

    /* ---- 运行期状态：统一槽位表（App 与驱动同表） ---- */
    uint32 slot_type[SVCRT_SLOT_ARRAY_MAX];     /* SVCRT_SLOT_x；FREE 表示该记录可复用 */
    uint32 slot_state[SVCRT_SLOT_ARRAY_MAX];    /* SVCRT_APP_SLOT_x */
    uint32 slot_base[SVCRT_SLOT_ARRAY_MAX];     /* 镜像在池内的起始地址（0 = 未占用） */
    uint32 slot_size[SVCRT_SLOT_ARRAY_MAX];     /* 镜像在 Flash 内占用的字节数（含头与重定位表） */
    uint32 slot_ram_base[SVCRT_SLOT_ARRAY_MAX]; /* 分配到的 RAM 块基址（0 = 未分配） */
    uint32 slot_ram_size[SVCRT_SLOT_ARRAY_MAX]; /* 分配到的 RAM 块大小（2 的幂字节数） */
    uint32 slot_entry[SVCRT_SLOT_ARRAY_MAX];    /* 镜像入口地址（0 表示无效） */
    uint32 slot_task_id[SVCRT_SLOT_ARRAY_MAX];  /* 对应任务号（0 表示未启动） */
    uint32 slot_crash_cnt[SVCRT_SLOT_ARRAY_MAX];/* 连续故障重启次数 */
    uint32 slot_autostart[SVCRT_SLOT_ARRAY_MAX];/* 非 0 = 开机扫描后自动启动 */

    /* ---- 开发期裸镜像表（仅 APP_ALLOW_RAW_IMAGE=1 时由内核回填，供上位机查看） ---- */
    uint32 dev_slot_count;                      /* 回填的裸镜像条目数 */

    /* ---- 设备端配置区里两个新旋钮的生效值（v7 新增，追加在末尾以免
     *      移动已有字段的偏移） ---- */
    uint32 cfg_boot_delay_ms;                   /* 自启前等待的毫秒数（0 = 不等待） */
    uint32 cfg_raw_allow;                       /* 非 0 = 接受池内直接烧录的裸镜像 */

    /* ---- 小程序瞬态块（v8 起，追加在末尾以免移动已有字段的偏移）
     *      这些字段只在**一个小程序正在运行时**非零：内核装载小程序时向
     *      镜像 RAM 池借两块记在这里，任务退出时清回 0 归还。
     *      它们不挂任何槽位——小程序没有槽位记录（它在文件系统里，不在
     *      镜像池里），这正是需要单独记账的原因。
     *      v9 起是两块：代码块（可执行）与 RAM 块（RW/ZI + 栈）。两块都是
     *      2 的幂、各自对齐自己的大小，因此各能精确落进一个 MPU 区域。 */
    uint32 mini_code_base;                      /* 小程序代码块基址（0 = 无） */
    uint32 mini_code_size;                      /* 小程序代码块字节数（0 = 无） */
    uint32 mini_ram_base;                       /* 小程序 RAM 块基址（0 = 无） */
    uint32 mini_ram_size;                       /* 小程序 RAM 块字节数（0 = 无） */
} svcrt_partition_table_t;

#endif /* __SVCRT_SHARE_H__ */
