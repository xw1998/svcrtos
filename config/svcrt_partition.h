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

/* 槽位策略：驱动池与 App 区都被等分为若干槽位，一个镜像占一个槽位。
 * 两个数量都必须 <= SVCRT_SLOT_ARRAY_MAX（8，见 svcrt_share.h 的固定数组），
 * 越界配置由下面的编译期断言拦住。 */
#define DRIVER_MAX_COUNT     4               /* 驱动槽位数量 */
#define APP_MAX_COUNT        4               /* App 槽位数量 */

/* RAM 分区（驱动/App 的 RAM 也按槽等分，见下方派生宏） */
#define SHARE_RAM_SIZE       (8  * 1024)     /* 共享内存：分区表 + 内核/App 数据交换 */
#define DRIVER_RAM_SIZE      (32 * 1024)     /* 独立驱动固件 RAM（4 槽 x 8K） */
#define APP_RAM_SIZE         (32 * 1024)     /* App 固件 RAM（4 槽 x 8K，含 4K 栈） */

/* App 镜像运行参数（由内核在启动 App 任务时使用） */
#define APP_TASK_PRIORITY    10              /* App 任务优先级 */
#define APP_TASK_STACK_SIZE  (1024 * 4)      /* App 任务栈大小（字节） */
#define APP_TASK_PERIOD_MS   1000            /* App 任务周期（ms） */
#define APP_AUTO_START       1               /* 上电扫描到有效 App 镜像后是否自动启动（1=自动） */
#define APP_CRASH_RESTART_MAX 3              /* 连续故障重启上限：达到后禁用该 App 槽位（0=不限次自动重启） */

/* 驱动区：驱动任务运行参数（栈同样从 DRIVER_RAM 区顶部切出，由内核推导） */
#define DRIVER_TASK_PRIORITY    9                /* 驱动任务优先级（默认高于 App） */
#define DRIVER_TASK_STACK_SIZE  (1024 * 1)       /* 驱动任务栈大小（字节） */
#define DRIVER_TASK_PERIOD_MS   1000             /* 驱动任务周期（ms） */
#define DRIVER_AUTO_START       1                /* 扫描到有效驱动镜像后是否自动启动 */

/* ============================================================
 * 九、任务容量策略（唯一入口：只改这三个数字）
 * @details 任务表是一块静态 TCB 数组，容量在编译期定死。为了让
 *          “很多用户驱动 + 很多用户应用”的场景可以直接扩到大规模，
 *          容量按 内核 / 驱动 / 应用 三类分层：
 *
 *            SVCRT_TASK_MAX_NUM   任务表总容量（静态 TCB 数组元素个数）
 *            DRIVER_MAX_COUNT     驱动槽位数（每个驱动镜像占 PER_DRIVER 个任务）
 *            APP_MAX_COUNT        App 槽位数（每个 App 镜像占 PER_APP 个任务）
 *
 *          其余全部由本文件派生：
 *            SVCRT_TASK_NEED_MIN       三类需求之和（分区能装下所需的最小容量）
 *            SVCRT_TASK_TABLE_RAM_MAX  静态 TCB 数组的 RAM 预算上限（字节）
 *          两者都在下方编译期断言里校验：容量配小了直接编译报错，
 *          而不是运行期静默注册失败。
 * @note 运行时仍可继续用 svcrt_task_register() 动态创建任务，上限就是
 *       SVCRT_TASK_MAX_NUM；上面两个 *_MAX_COUNT 只约束固定占用的那部分。
 * ============================================================ */
#ifndef SVCRT_TASK_MAX_NUM
#define SVCRT_TASK_MAX_NUM        (48)   /* 任务表总容量（静态 TCB 数组元素个数） */
#endif

#define SVCRT_TASK_MAX_KERNEL     (8)    /* 内核自带任务 + 示例静态任务的预留 */
#define SVCRT_TASK_PER_DRIVER     (1)    /* 每个驱动镜像占用的任务数 */
#define SVCRT_TASK_PER_APP        (1)    /* 每个 App 镜像占用的任务数 */

#define SVCRT_TASK_NEED_MIN       (SVCRT_TASK_MAX_KERNEL + \
                                   (DRIVER_MAX_COUNT * SVCRT_TASK_PER_DRIVER) + \
                                   (APP_MAX_COUNT * SVCRT_TASK_PER_APP))

/* 静态 TCB 数组的 RAM 预算（字节）。守护断言直接比较真实的
 * sizeof(svcrt_task_table)，不依赖「每个 TCB 多少字节」的估算：
 * Cortex-M4 上实测 sizeof(svcrt_task_t) = 76 字节（SVCRT_USE_MPU=0）
 * / 140 字节（SVCRT_USE_MPU=1），48 槽分别占 3648 / 6720 字节。
 * 预算固定 8KB，把 SVCRT_TASK_MAX_NUM 提到约 110（MPU 关）或约 58
 * （MPU 开）以上才会触发断言；确实要更大规模时把本宏显式改大即可，
 * 改多少就是显式占用多少内核 RAM。 */
#ifndef SVCRT_TASK_TABLE_RAM_MAX
#define SVCRT_TASK_TABLE_RAM_MAX  (8u * 1024u)
#endif

/* ============================================================
 * 七、镜像在槽位内的布局（安装路径 vs 开发调试路径）
 * @details .svcapp 镜像 = 固定 256 字节头 + 负载，整块写入槽位：
 *
 *              槽位基址                          槽位基址+256
 *              +------------------+-------------------------------+
 *              | 镜像头 (256 字节) |      负载（代码 / 只读数据）   |
 *              +------------------+-------------------------------+
 *
 *          因此「负载的链接基址」= 槽位基址 + APP_IMAGE_HEADER_SIZE，
 *          各 App / 驱动工程的 .sct 必须按该基址生成
 *          （tools/gen_scatter.py --target app|driver 的默认布局）。
 *          entry_offset 是入口相对「负载起始」的偏移，内核计算入口地址时
 *          会先加上本值（见 svcrt_loader_entry_addr）。
 * @note 开发调试用的裸镜像路径（APP_ALLOW_RAW_IMAGE=1）没有镜像头，负载
 *       直接位于槽位基址，其 .sct 用 tools/gen_scatter.py --raw 单独生成，
 *       两条路径的二进制互不通用。
 * ============================================================ */
#define APP_IMAGE_HEADER_SIZE (256)          /* .svcapp 镜像头长度，必须与 svcrt_app_image.h 一致 */

/* ============================================================
 * 六、开发调试策略
 * @details 用 Keil 直接下载 App/驱动并下断点调试时，烧进 Flash 的是**裸镜像**
 *          （无 256 字节镜像头，也没有 CRC），与安装路径的 .svcapp 格式不同。
 *          打开本开关后，内核在扫描槽位时会把“非擦除状态且不带镜像头”的内容
 *          当作开发期裸镜像，入口取分区基址（|1 置 Thumb），从而保住
 *          “固定地址烧录 + MDK 调试”的开发闭环。
 *          发布固件时应关掉它，只接受带签名的 .svcapp。
 * ============================================================ */
#ifndef APP_ALLOW_RAW_IMAGE
#define APP_ALLOW_RAW_IMAGE     0                /* 1=允许槽位内直接烧录的裸镜像（开发调试用） */
#endif
/* 默认 0：发布固件只接受带 CRC 的 .svcapp。
 * 开发板在 svcrt_board_config.h 里显式置 1 打开裸镜像旁路，
 * 以保留“固定地址烧录 + MDK 断点调试”的开发闭环。 */

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

/* 驱动池按槽等分（与 App 槽位同构：一个镜像占一个槽位） */
#define DRIVER_SLOT_SIZE     (DRIVER_POOL_SIZE / DRIVER_MAX_COUNT)
#define DRIVER_SLOT_BASE(n)  (DRIVER_POOL_BASE + (n) * DRIVER_SLOT_SIZE)

/* 0 号 App 槽位（保留的名字，等价于 APP_USER_BASE/APP_SLOT_SIZE） */
#define APP_SLOT0_BASE       (APP_USER_BASE)
#define APP_SLOT0_SIZE       (APP_SLOT_SIZE)

#define SVCRT_FLASH_END      (CHIP_FLASH_BASE  + CHIP_FLASH_SIZE)   /* 避开 CMSIS 的 FLASH_END 宏 */

/* ---- RAM 布局（地址由低到高：SHARE → KERNEL → DRIVER → APP） ---- */
#define SHARE_RAM_BASE       (CHIP_RAM_BASE)
#define KERNEL_RAM_BASE      (SHARE_RAM_BASE   + SHARE_RAM_SIZE)
#define KERNEL_RAM_SIZE      (CHIP_RAM_SIZE    - SHARE_RAM_SIZE - DRIVER_RAM_SIZE - APP_RAM_SIZE)
#define DRIVER_RAM_BASE      (KERNEL_RAM_BASE  + KERNEL_RAM_SIZE)
#define APP_RAM_BASE         (DRIVER_RAM_BASE  + DRIVER_RAM_SIZE)

/* RAM 按槽等分：每个槽位的 .data/.bss/栈都落在自己的区间内，
 * 多个镜像并存时互不覆盖（栈从各自区间顶部向下生长）。 */
#define DRIVER_SLOT_RAM_SIZE    (DRIVER_RAM_SIZE / DRIVER_MAX_COUNT)
#define DRIVER_SLOT_RAM_BASE(n) (DRIVER_RAM_BASE + (n) * DRIVER_SLOT_RAM_SIZE)
#define APP_SLOT_RAM_SIZE       (APP_RAM_SIZE    / APP_MAX_COUNT)
#define APP_SLOT_RAM_BASE(n)    (APP_RAM_BASE    + (n) * APP_SLOT_RAM_SIZE)

/* ---- 硬件兼容签名（App 与内核 ABI 匹配校验用） ----
 * 高 16 位：芯片型号标识；低 16 位：内核接口版本。
 * App 打包时应写入相同值，内核加载时校验，不匹配则拒绝加载。 */
#define SVCRT_HW_COMPAT_ID   (0x42700002u)   /* 0x4270 = STM32F427, 0x0002 = ABI v2（驱动槽位数组化） */

/* ============================================================
 * 八、MPU isolation windows (derived; do not edit by hand)
 * @details svcrt_mpu.c builds every task's MPU context from these windows
 *          only, so the project still has exactly one place where addresses
 *          are defined.
 *          An MPU region must be a power of two in size and aligned to that
 *          size; the compile-time check below enforces both for all windows.
 * ============================================================ */
#define SVCRT_MPU_KERNEL_ROM_BASE   (KERNEL_BASE)
#define SVCRT_MPU_KERNEL_ROM_SIZE   (KERNEL_SIZE)
#define SVCRT_MPU_DRIVER_ROM_BASE   (DRIVER_POOL_BASE)
#define SVCRT_MPU_DRIVER_ROM_SIZE   (DRIVER_POOL_SIZE)
#define SVCRT_MPU_APP_ROM_BASE      (APP_USER_BASE)
#define SVCRT_MPU_APP_ROM_SIZE      (APP_USER_SIZE)
#define SVCRT_MPU_DRIVER_RAM_BASE   (DRIVER_RAM_BASE)
#define SVCRT_MPU_DRIVER_RAM_SIZE   (DRIVER_RAM_SIZE)
#define SVCRT_MPU_APP_RAM_BASE      (APP_RAM_BASE)
#define SVCRT_MPU_APP_RAM_SIZE      (APP_RAM_SIZE)

/* Per-slot windows: a user task is granted exactly its own slot, so two
 * images can never see each other's code or data. All four sizes must be
 * powers of two and the bases aligned to them (checked below). */
#define SVCRT_MPU_DRIVER_SLOT_SIZE      (DRIVER_SLOT_SIZE)
#define SVCRT_MPU_APP_SLOT_SIZE         (APP_SLOT_SIZE)
#define SVCRT_MPU_DRIVER_SLOT_RAM_SIZE  (DRIVER_SLOT_RAM_SIZE)
#define SVCRT_MPU_APP_SLOT_RAM_SIZE     (APP_SLOT_RAM_SIZE)
#define SVCRT_MPU_SHARE_RAM_BASE    (SHARE_RAM_BASE)
#define SVCRT_MPU_SHARE_RAM_SIZE    (SHARE_RAM_SIZE)

/* Power-of-two and alignment self-check: the array size is the product of the
 * individual results, so a single failure makes it negative and the compiler
 * refuses the file. */
#define SVCRT_IS_POW2(x)            (((x) != 0) && (((x) & ((x) - 1)) == 0))
typedef char svcrt_mpu_window_check[
      (SVCRT_IS_POW2(KERNEL_SIZE)                ? 1 : -1)
    * (SVCRT_IS_POW2(DRIVER_POOL_SIZE)           ? 1 : -1)
    * (SVCRT_IS_POW2(APP_SLOT_SIZE)              ? 1 : -1)
    * (SVCRT_IS_POW2(APP_RAM_SIZE)               ? 1 : -1)
    * (SVCRT_IS_POW2(DRIVER_RAM_SIZE)            ? 1 : -1)
    * (SVCRT_IS_POW2(DRIVER_SLOT_SIZE)           ? 1 : -1)
    * (SVCRT_IS_POW2(APP_SLOT_SIZE)              ? 1 : -1)
    * (SVCRT_IS_POW2(DRIVER_SLOT_RAM_SIZE)       ? 1 : -1)
    * (SVCRT_IS_POW2(APP_SLOT_RAM_SIZE)          ? 1 : -1)
    * (SVCRT_IS_POW2(SHARE_RAM_SIZE)             ? 1 : -1)
    * (((KERNEL_BASE      % KERNEL_SIZE)      == 0) ? 1 : -1)
    * (((DRIVER_POOL_BASE % DRIVER_POOL_SIZE) == 0) ? 1 : -1)
    * (((APP_USER_BASE    % APP_SLOT_SIZE)    == 0) ? 1 : -1)
    * (((APP_RAM_BASE     % APP_RAM_SIZE)     == 0) ? 1 : -1)
    * (((DRIVER_RAM_BASE  % DRIVER_RAM_SIZE)  == 0) ? 1 : -1)
    * (((SHARE_RAM_BASE   % SHARE_RAM_SIZE)   == 0) ? 1 : -1)
    * (((DRIVER_POOL_BASE % DRIVER_SLOT_SIZE) == 0) ? 1 : -1)
    * (((APP_USER_BASE    % APP_SLOT_SIZE)    == 0) ? 1 : -1)
    * (((DRIVER_RAM_BASE  % DRIVER_SLOT_RAM_SIZE) == 0) ? 1 : -1)
    * (((APP_RAM_BASE     % APP_SLOT_RAM_SIZE)    == 0) ? 1 : -1)
    * 1];

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

/* ============================================================
 * 四、内核 Shell 控制台（ark-shell 移植）
 * @details 内核跑一个低优先级控制台任务，独占串口提供人机命令：
 *          App / 驱动的启停与查询、任务与内存概览、故障读数、串口安装。
 *          开启 shell 后不再注册常驻安装任务（见 svcrt_installer.c），
 *          改为 `install` 命令打开一次安装窗口：否则两个任务会同时读同一个
 *          串口 FIFO，把镜像字节流随机分掉，谁都装不成。
 *          「是否开机自启」由镜像头 flags 决定（tools/pack_app.py --autostart），
 *          本节的 APP_AUTO_START / DRIVER_AUTO_START 只影响「裸镜像」这条
 *          开发调试路径。
 * ============================================================ */
#ifndef SHELL_ENABLE
#define SHELL_ENABLE             1               /* 1=启用内核 shell 控制台任务 */
#endif
#define SHELL_DEV_NAME           "COM1"          /* 控制台串口设备名 */
#define SHELL_DEV_ARG            115200          /* 设备打开参数（波特率） */
#define SHELL_TASK_PRIORITY      14              /* 控制台优先级（最低：人机交互，不抢业务 CPU） */
#define SHELL_TASK_STACK_SIZE    (1024 * 3)      /* 控制台任务栈（字节，取自内核 RAM） */
#define SHELL_TASK_PERIOD_MS     2               /* 无按键时的轮询间隔（ms） */
#define SHELL_INSTALL_TIMEOUT_MS 120000          /* install 窗口最长等待（ms），超时回命令提示符 */

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

/* ---- 任务容量：分层需求必须装得进任务表 ---- */
typedef char svcrt_task_capacity_check[
    (SVCRT_TASK_MAX_NUM >= SVCRT_TASK_NEED_MIN) ? 1 : -1];

/* ---- 槽位数不能超过共享内存 ABI 的固定数组长度（8，见 svcrt_share.h） ---- */
typedef char svcrt_slot_count_check[
    ((DRIVER_MAX_COUNT <= 8) && (APP_MAX_COUNT <= 8)) ? 1 : -1];

/* ---- 驱动池 / 驱动 RAM 必须能被槽位数整除（否则槽位基址无处安放） ---- */
typedef char svcrt_driver_slot_divide_check[
    (((DRIVER_POOL_SIZE % DRIVER_MAX_COUNT) == 0) &&
     ((DRIVER_RAM_SIZE  % DRIVER_MAX_COUNT) == 0) &&
     (DRIVER_MAX_COUNT > 0)) ? 1 : -1];

/* ---- 每个槽位要装得下：镜像头 + 最小负载；RAM 要装得下栈 + 最小数据 ---- */
typedef char svcrt_driver_slot_fit_check[
    ((DRIVER_SLOT_SIZE >= (APP_IMAGE_HEADER_SIZE + 1024)) &&
     (DRIVER_SLOT_RAM_SIZE >= (DRIVER_TASK_STACK_SIZE + 512))) ? 1 : -1];
typedef char svcrt_app_slot_fit_check[
    ((APP_SLOT_SIZE >= (APP_IMAGE_HEADER_SIZE + 1024)) &&
     (APP_SLOT_RAM_SIZE >= (APP_TASK_STACK_SIZE + 512))) ? 1 : -1];

/* ---- App RAM 每槽至少能装下它的栈（loader 按槽顶切栈） ---- */
typedef char svcrt_app_slot_stack_check[
    (APP_TASK_STACK_SIZE <= APP_SLOT_RAM_SIZE) ? 1 : -1];
typedef char svcrt_driver_slot_stack_check[
    (DRIVER_TASK_STACK_SIZE <= DRIVER_SLOT_RAM_SIZE) ? 1 : -1];

#endif /* SVCRT_PARTITION_H */
