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

/** @brief 分区表结构版本 */
#define SVCRT_PARTITION_VERSION   (1u)

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
    uint32 app_ram_base;
    uint32 app_ram_size;

    /* ---- 运行期状态（槽位表） ---- */
    uint32 slot_state[8];       /* 每个槽位：SVCRT_APP_SLOT_x */
    uint32 slot_entry[8];       /* 每个槽位 App 的入口地址（0 表示无效） */
    uint32 slot_task_id[8];     /* 每个槽位 App 对应的任务号（0 表示未启动） */
    uint32 slot_crash_cnt[8];   /* 每个槽位的连续故障重启次数（达上限则禁用该 App） */
} svcrt_partition_table_t;

#endif /* __SVCRT_SHARE_H__ */
