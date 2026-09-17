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
 */
#define SVCRT_PARTITION_VERSION   (5u)

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
} svcrt_partition_table_t;

#endif /* __SVCRT_SHARE_H__ */
