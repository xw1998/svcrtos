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
 *  v1 -> v2: driver 区状态由「单槽四个平铺字段」改为「与 App 同构的槽位数组」，
 *  结构尺寸与字段偏移都变了，因此版本必须升位，避免新旧镜像错配。
 *  v2 -> v3: 末尾追加每槽 autostart 标志（App / 驱动各一组）。新字段追加在
 *  结构尾部，v2 读者读到的布局字段偏移不变，但为了让「结构形状」有明确边界，
 *  版本仍然升位。 */
#define SVCRT_PARTITION_VERSION   (3u)

/** @brief 槽位数组的固定长度（ABI 形状常量）。
 *  实际使用的槽位数由运行期字段 driver_max_count / app_max_count 决定，
 *  两者都必须 <= 本值；数组长度写死是为了让结构体尺寸与偏移跨版本稳定。 */
#define SVCRT_SLOT_ARRAY_MAX      (8u)

/** @brief App 槽位状态 */
#define SVCRT_APP_SLOT_EMPTY      (0u)    /* 空槽位 */
#define SVCRT_APP_SLOT_LOADED     (1u)    /* 已写入镜像并通过校验 */
#define SVCRT_APP_SLOT_RUNNING    (2u)    /* 已注册为任务并运行 */
#define SVCRT_APP_SLOT_INVALID    (3u)    /* 槽位有内容但校验失败（魔数/兼容签名/CRC 不符） */
#define SVCRT_APP_SLOT_INSTALLING (4u)    /* 正在安装：写入未完成，不可启动（掉电后由 CRC 判定为 INVALID） */

/**
* @brief 分区表（存放于共享内存起始处）
* @details 由内核在启动时填充静态布局，运行期由 Loader 更新槽位状态。
*          Loader / App 只读布局字段，写状态字段前须确认自己拥有该槽位。
*/
typedef struct {
    uint32 magic;               /* SVCRT_PARTITION_MAGIC */
    uint32 version;             /* SVCRT_PARTITION_VERSION */
    uint32 hw_compat_id;        /* 硬件兼容签名（与 App 镜像头比对） */

    /* ---- Flash 分区 ---- */
    uint32 kernel_base;
    uint32 kernel_size;
    uint32 driver_pool_base;
    uint32 driver_pool_size;
    uint32 driver_slot_size;    /* 驱动池被等分后的单槽大小 = driver_pool_size / driver_max_count */
    uint32 driver_max_count;    /* 驱动槽位数量（<= SVCRT_SLOT_ARRAY_MAX） */
    uint32 app_user_base;
    uint32 app_user_size;
    uint32 app_slot_size;
    uint32 app_max_count;

    /* ---- RAM 分区 ---- */
    uint32 share_ram_base;
    uint32 share_ram_size;
    uint32 kernel_ram_base;
    uint32 kernel_ram_size;
    uint32 driver_ram_base;
    uint32 driver_ram_size;
    uint32 driver_slot_ram_size; /* 驱动 RAM 被等分后的单槽大小 */
    uint32 app_ram_base;
    uint32 app_ram_size;
    uint32 app_slot_ram_size;   /* App RAM 被等分后的单槽大小（按槽顶切栈用） */

    /* ---- 运行期状态（App 槽位表） ---- */
    uint32 slot_state[SVCRT_SLOT_ARRAY_MAX];     /* 每个槽位：SVCRT_APP_SLOT_x */
    uint32 slot_entry[SVCRT_SLOT_ARRAY_MAX];     /* 每个槽位 App 的入口地址（0 表示无效） */
    uint32 slot_task_id[SVCRT_SLOT_ARRAY_MAX];   /* 每个槽位 App 对应的任务号（0 表示未启动） */
    uint32 slot_crash_cnt[SVCRT_SLOT_ARRAY_MAX]; /* 每个槽位的连续故障重启次数（达上限则禁用该 App） */

    /* ---- 驱动区运行期状态（与 App 槽位表同构：一个驱动镜像占一个槽位） ---- */
    uint32 driver_slot_state[SVCRT_SLOT_ARRAY_MAX];     /* 每个驱动槽位：SVCRT_APP_SLOT_x */
    uint32 driver_slot_entry[SVCRT_SLOT_ARRAY_MAX];     /* 每个驱动槽位入口地址（0 表示无效） */
    uint32 driver_slot_task_id[SVCRT_SLOT_ARRAY_MAX];   /* 每个驱动槽位对应任务号（0 表示未启动） */
    uint32 driver_slot_crash_cnt[SVCRT_SLOT_ARRAY_MAX]; /* 每个驱动槽位的连续故障重启次数 */

    /* ---- 每槽自启标志（来自镜像头 flags，打包时决定；裸镜像取编译期默认） ---- */
    uint32 slot_autostart[SVCRT_SLOT_ARRAY_MAX];        /* 非 0 = 开机扫描后自动启动 */
    uint32 driver_slot_autostart[SVCRT_SLOT_ARRAY_MAX]; /* 驱动槽位同上 */
} svcrt_partition_table_t;

#endif /* __SVCRT_SHARE_H__ */
