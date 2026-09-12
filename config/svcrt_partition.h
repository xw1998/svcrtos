/**
* @file svcrt_partition.h
* @brief SVCrtOS 全工程唯一的分区配置源头（Flash / RAM 布局）
* @details 设计原则：
*          1) 本文件是唯一允许出现物理地址的地方，任何 .sct / C 文件 / 脚本
*             都不得硬编码 Flash 或 RAM 地址；
*          2) 移植新芯片只改「芯片物理参数」+「分区大小策略」两组宏，
*             其余地址全部由本文件自动推导；
*          3) 各工程 .sct 由 tools/gen_scatter.py 读取本文件自动生成，
*             禁止手工编辑分散加载文件；
*          4) Loader / App 工程禁止包含本文件，只能通过 svcrt_share.h 定义的
*             共享内存分区表在运行期获取布局信息（编译期解耦）。
*
* @note 修改本文件后必须重新编译所有工程（.sct 在编译前由脚本重新生成）。
*/

#ifndef SVCRT_PARTITION_H
#define SVCRT_PARTITION_H

/* ============================================================
 * 一、芯片物理参数（移植新芯片只改这 4 行）
 * ============================================================ */
#define CHIP_FLASH_BASE      0x08000000
#define CHIP_FLASH_SIZE      (1024 * 1024)   /* 1MB   */
#define CHIP_RAM_BASE        0x20000000
#define CHIP_RAM_SIZE        (128 * 1024)    /* 128KB（SRAM1 112K + SRAM2 16K） */

/* 内核独享的 CCM / TCM RAM（无此内存的芯片填 0） */
#define CHIP_CCM_BASE        0x10000000
#define CHIP_CCM_SIZE        (64 * 1024)

/* ============================================================
 * 二、分区大小策略（按项目需求调整）
 * ============================================================ */
/* Flash 分区 */
#define BOOT_SIZE            (0)             /* Bootloader 区大小；0 表示暂不划分（内核直接从 Flash 起始运行） */
#define KERNEL_SIZE          (256 * 1024)    /* 内核固件区 */
#define DRIVER_POOL_SIZE     (256 * 1024)    /* 独立驱动固件池 */

/* App 区：区大小与槽位策略 */
#define APP_MAX_COUNT        1               /* App 槽位数量；最小闭环先用 1（单区间），后续扩多槽位 */

/* RAM 分区 */
#define SHARE_RAM_SIZE       (8  * 1024)     /* 共享内存：分区表 + 内核/App 数据交换 */
#define DRIVER_RAM_SIZE      (16 * 1024)     /* 独立驱动固件 RAM */
#define APP_RAM_SIZE         (16 * 1024)     /* App 固件 RAM（.data/.bss/栈） */

/* App 镜像运行参数（由内核在启动 App 任务时使用） */
#define APP_TASK_PRIORITY    10              /* App 任务优先级 */
#define APP_TASK_STACK_SIZE  (1024 * 4)      /* App 任务栈大小（字节） */
#define APP_TASK_PERIOD_MS   1000            /* App 任务周期（ms） */
#define APP_AUTO_START       1               /* 上电扫描到有效 App 镜像后是否自动启动（1=自动） */

/* 驱动区：驱动任务运行参数（栈同样从 DRIVER_RAM 区顶部切出，由内核推导） */
#define DRIVER_TASK_PRIORITY    9                /* 驱动任务优先级（默认高于 App） */
#define DRIVER_TASK_STACK_SIZE  (1024 * 1)       /* 驱动任务栈大小（字节） */
#define DRIVER_TASK_PERIOD_MS   1000             /* 驱动任务周期（ms） */

/* ============================================================
 * 三、以下全部自动推导，禁止手改
 * ============================================================ */

/* ---- Flash 布局（地址由低到高：BOOT → KERNEL → DRIVER_POOL → APP） ---- */
#define BOOT_BASE            (CHIP_FLASH_BASE)
#define KERNEL_BASE          (BOOT_BASE        + BOOT_SIZE)
#define DRIVER_POOL_BASE     (KERNEL_BASE      + KERNEL_SIZE)
#define APP_USER_BASE        (DRIVER_POOL_BASE + DRIVER_POOL_SIZE)

#define APP_USER_SIZE        (CHIP_FLASH_SIZE  - BOOT_SIZE - KERNEL_SIZE - DRIVER_POOL_SIZE)
#define APP_SLOT_SIZE        (APP_USER_SIZE    / APP_MAX_COUNT)

/* 单区间最小闭环：0 号槽位（也是当前唯一的 App 区） */
#define APP_SLOT0_BASE       (APP_USER_BASE)
#define APP_SLOT0_SIZE       (APP_SLOT_SIZE)

#define SVCRT_FLASH_END      (CHIP_FLASH_BASE  + CHIP_FLASH_SIZE)   /* 避开 CMSIS 的 FLASH_END 宏 */

/* ---- RAM 布局（地址由低到高：SHARE → KERNEL → DRIVER → APP） ---- */
#define SHARE_RAM_BASE       (CHIP_RAM_BASE)
#define KERNEL_RAM_BASE      (SHARE_RAM_BASE   + SHARE_RAM_SIZE)
#define KERNEL_RAM_SIZE      (CHIP_RAM_SIZE    - SHARE_RAM_SIZE - DRIVER_RAM_SIZE - APP_RAM_SIZE)
#define DRIVER_RAM_BASE      (KERNEL_RAM_BASE  + KERNEL_RAM_SIZE)
#define APP_RAM_BASE         (DRIVER_RAM_BASE  + DRIVER_RAM_SIZE)

/* ---- 硬件兼容签名（App 与内核 ABI 匹配校验用） ----
 * 高 16 位：芯片型号标识；低 16 位：内核接口版本。
 * App 打包时应写入相同值，内核加载时校验，不匹配则拒绝加载。 */
#define SVCRT_HW_COMPAT_ID   (0x42700001u)   /* 0x4270 = STM32F427，0x0001 = ABI v1 */

/* ============================================================
 * 五、安装器策略（方案A：内核内安装任务）
 * @details 由内核常驻任务从设备流式接收 .svcapp 并安装到空闲槽位，
 *          使闭环从“烧录器刷固件”变为“设备自己安装”。
 * ============================================================ */
#define INSTALLER_ENABLE         1               /* 1=启用内核内安装任务 */
#define INSTALLER_DEV_NAME       "COM1"          /* 镜像接收设备名 */
#define INSTALLER_DEV_ARG        115200          /* 设备打开参数（波特率） */
#define INSTALLER_TASK_PRIORITY  12              /* 安装任务优先级（低于 App，不抢 CPU） */
#define INSTALLER_TASK_STACK_SIZE (1024 * 2)     /* 安装任务栈大小（字节，取自内核 RAM） */
#define INSTALLER_TASK_PERIOD_MS 50              /* 无数据时的轮询间隔（ms） */
#define INSTALLER_AUTO_START     1               /* 安装完成后是否自动启动该 App */

/* ---- 一致性自检（编译期，配置错误在编译阶段就暴露） ---- */
#if (KERNEL_RAM_SIZE <= 0)
#error "RAM 配置过小：SHARE + DRIVER + APP 已超过 CHIP_RAM_SIZE"
#endif

#if (APP_SLOT_SIZE <= 0)
#error "Flash 配置过小：BOOT + KERNEL + DRIVER_POOL 已超过 CHIP_FLASH_SIZE"
#endif

#if (DRIVER_POOL_BASE < KERNEL_BASE) || (APP_USER_BASE < DRIVER_POOL_BASE)
#error "Flash 分区顺序错误"
#endif

#endif /* SVCRT_PARTITION_H */
