/**
* @file svcrt_partition.h
* @brief SVCrtOS 全工程唯一的分区配置源头（Flash / RAM 布局）
* @note 本文件是 STM32F401xE 的实例：512KB Flash / 96KB SRAM、无 CCM；
*       统一镜像池 = 3 x 128KB（sector 5~7），内核 RAM = 96 - 8 - 32 = 56KB。
*       与 config/svcrt_partition.h（STM32F427）各自独立，互不影响；
*       工程通过 Include Path 指向本目录，脚本用 --header 指过来。
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
#define CHIP_FLASH_SIZE      (512 * 1024)    /* 512KB（F401xE；真机 Flash size 寄存器 0x1FFF7A22 = 0x0200 佐证） */
#define CHIP_RAM_BASE        0x20000000
#define CHIP_RAM_SIZE        (96 * 1024)     /* 96KB（F401xD/E 的 SRAM；F401 无 SRAM2） */

/* 内核独享的 CCM / TCM RAM（无此内存的芯片填 0） */
#define CHIP_CCM_BASE        0x10000000
#define CHIP_CCM_SIZE        (0)             /* F401 无 CCM / TCM */

/* ============================================================
 * 二、分区大小策略（按项目需求调整）
 * ============================================================ */
/* ---- Flash ----
 * 布局：BOOT → KERNEL → IMAGE_POOL（统一镜像池，App 与驱动共用）。
 *
 * 统一镜像池把「驱动固件区」和「App 固件区」合并成一块连续的 Flash，
 * 里面按 SVCRT_POOL_ALLOC_UNIT 为单位做细粒度首适配分配：镜像按自身实际
 * 长度向后紧邻排列，不再需要链接期钉死地址（镜像自带重定位表，
 * 装载时按实际落点打补丁，见 svcrt_app_image.h）。
 *
 * 三个概念必须分清，否则容量会算错：
 *
 *   IMAGE_POOL_SECTOR     芯片的物理擦除单位（F401 512K 档的 sector 5~7 = 128K）。
 *                         写是按字节写的，擦只能整扇区擦——所以「回收一小块
 *                         空间」在物理上必然等于「擦掉一整扇区 + 把同扇区里
 *                         的幸存镜像搬走」。这是卸载要压实、要预留余量的根因。
 *   SVCRT_POOL_ALLOC_UNIT 分配粒度与落点对齐单位（1KB）。镜像落点一定是它的
 *                         整数倍，上电扫描也按这个步长找魔数。
 *   POOL_RESERVE_SECTORS  压实余量：始终留出这么多整扇区不分配给镜像。
 *                         搬移任何镜像前都要有已擦除的空地能写新副本，
 *                         没有余量就会出现「想卸载却腾不出手」的死局。
 */
#define BOOT_SIZE            (0)             /* Bootloader 区大小；0 = 暂不划分（内核直接从 Flash 起始运行） */
#ifndef KERNEL_SIZE
#define KERNEL_SIZE          (128 * 1024)    /* 内核固件区；可按编译结果调整(inspect the resulting layout with tools/gen_scatter.py --target all) */
#endif
#define IMAGE_POOL_SECTOR    (128 * 1024)    /* 芯片物理擦除单位（F401 512K 档的 sector 5~7） */
/* 设备端布局配置区（安装策略的持久配置，SVCrtOS 的唯一「非编译期」布局来源）
 * ------------------------------------------------------------------
 * 位置：内核之后、镜像池之前，占一个物理擦除单位，可单独擦写。
 * 内容：安装模式（固定槽位 / 自动选址）+ 固定槽位表 + 每槽 RAM 窗口，
 *       由上位机工具（tools/svcrt_cfg.py）经串口在线写入，格式见
 *       kernelsrc/include/svcrt_layout_def.h。
 * 掉电：扇区内按 SVCRT_CFG_RECORD_SIZE 字节记录顺序追加，写满后整体擦除
 *       一次再写；上电取「CRC 有效且序号最大」的那份。写入瞬间掉电不会
 *       丢配置（旧份仍然有效）；擦除瞬间掉电则回退编译期默认布局。
 * 0 = 不划分配置区（纯编译期布局，小容量档可选）。 */
#ifndef CONFIG_SIZE
#define CONFIG_SIZE          (IMAGE_POOL_SECTOR)
#endif
#define SVCRT_POOL_ALLOC_UNIT (1024)         /* 池分配粒度 / 落点对齐单位（字节） */
#ifndef POOL_RESERVE_SECTORS
#define POOL_RESERVE_SECTORS (1)             /* 压实余量：保留不分配的整扇区数（>=1） */

/*
 * SVCRT_RECLAIM_MODE  卸载与上电时的空间回收力度（两种模式都带事务保护：搬移先把新位置
 *                     写成 UNCOMMITTED，校验通过才提交，最后才擦旧位置，中途掉电不会丢镜像）
 *   SVCRT_RECLAIM_GLOBAL   全局压实：把活镜像按落点从低到高逐个搬到池内
 *                          最低的干净空位，让顶部空闲区尽量连成一片。
 *                          搬动次数多，但能腾出最大的连续空闲。
 *   SVCRT_RECLAIM_MINIMAL  最小移动：只处理「含有失效字节的扇区」——
 *                          搬走该扇区里的活镜像后擦掉它。搬动次数最少，
 *                          代价是顶部空闲区可能被切碎。
 */
#define SVCRT_RECLAIM_GLOBAL  (0)
#define SVCRT_RECLAIM_MINIMAL (1)
#ifndef SVCRT_RECLAIM_MODE
#define SVCRT_RECLAIM_MODE (SVCRT_RECLAIM_GLOBAL)
#endif
#endif

/* ---- 槽位策略 ----
 * SLOT_MAX 是分区表能同时登记的镜像数上限（App 与驱动共用一个池），
 * 必须 <= svcrt_share.h 的 SVCRT_SLOT_ARRAY_MAX（固定数组长度）。
 * @note 它与 Flash 容量无关：容量由镜像实际长度之和决定，
 *       细粒度分配下一个 10KB 的 App 只占 11KB，不再整扇区占用。 */
#ifndef SLOT_MAX
#define SLOT_MAX             16
#endif

/* ---- RAM ----
 * 布局：SHARE → KERNEL → SLOT_RAM（镜像 RAM 池）。
 * 镜像的 .data/.bss/栈从 SLOT_RAM 里按「伙伴分配」切块：镜像头声明
 * ram_size，内核向上取整到 2 的幂后分配，块大小在
 * [SLOT_RAM_MIN_BLOCK, SLOT_RAM_MAX_BLOCK] 之间。
 * 伙伴分配天然满足 MPU region「2 的幂 + 按大小对齐」的硬约束。 */
#define SHARE_RAM_SIZE       (8  * 1024)     /* 共享内存：分区表 + 内核/App 数据交换 */
#define SLOT_RAM_TOTAL       (32 * 1024)     /* 镜像 RAM 池总大小（伙伴分配）；F401 只有 96KB SRAM，池取 32KB、内核留 56KB */
#define SLOT_RAM_MIN_BLOCK   (1  * 1024)     /* 最小分配块（字节，必须是 2 的幂） */
#define SLOT_RAM_MAX_BLOCK   (8  * 1024)     /* 最大分配块（字节，必须是 2 的幂，且要装得下栈+数据） */

/* App / 驱动镜像运行参数（由内核在启动任务时使用） */
#define APP_TASK_PRIORITY    10              /* App 任务优先级 */
#define APP_TASK_STACK_SIZE  (1024 * 4)      /* App 任务栈大小（字节） */
#define APP_TASK_PERIOD_MS   1000            /* App 任务周期（ms） */
#define APP_AUTO_START       1               /* 上电扫描到有效 App 镜像后是否自动启动（1=自动） */
#define APP_CRASH_RESTART_MAX 3              /* 连续故障重启上限：达到后禁用该 App 槽位（0=不限次自动重启） */

#define DRIVER_TASK_PRIORITY    9                /* 驱动任务优先级（默认高于 App） */
#define DRIVER_TASK_STACK_SIZE  (1024 * 1)       /* 驱动任务栈大小（字节） */
#define DRIVER_TASK_PERIOD_MS   1000             /* 驱动任务周期（ms） */
#define DRIVER_AUTO_START       1                /* 扫描到有效驱动镜像后是否自动启动 */

/* ============================================================
 * 三、以下全部自动推导，禁止手改
 * ============================================================ */

/* ---- Flash 布局 ---- */
#define BOOT_BASE            (CHIP_FLASH_BASE)
#define KERNEL_BASE          (BOOT_BASE   + BOOT_SIZE)
#define CONFIG_BASE          (KERNEL_BASE + KERNEL_SIZE)
#define IMAGE_POOL_BASE      (CONFIG_BASE + CONFIG_SIZE)
#define IMAGE_POOL_SIZE      (CHIP_FLASH_SIZE - BOOT_SIZE - KERNEL_SIZE - CONFIG_SIZE)
#define IMAGE_POOL_END       (CHIP_FLASH_BASE + CHIP_FLASH_SIZE)
#define IMAGE_POOL_UNITS     (IMAGE_POOL_SIZE / IMAGE_POOL_SECTOR)

/* 压实余量占用的字节数，以及真正可以分配给镜像的区间。
 * 分配器只在前 POOL_USABLE_SIZE 字节里分配；尾部保留区始终留空，
 * 卸载压实时所有的搬移目的地都取自这块空地。 */
#define IMAGE_POOL_RESERVED_SIZE (POOL_RESERVE_SECTORS * IMAGE_POOL_SECTOR)
#define IMAGE_POOL_USABLE_SIZE   (IMAGE_POOL_SIZE - IMAGE_POOL_RESERVED_SIZE)
#define IMAGE_POOL_USABLE_END    (IMAGE_POOL_BASE + IMAGE_POOL_USABLE_SIZE)

/* 第 n 个物理扇区单元（n = 0 .. IMAGE_POOL_UNITS-1）的基址。
 * 仅用于两条路径：开发调试裸镜像的固定落点（见开发槽位表），
 * 以及压实/擦除时把地址换算成扇区号。 */
#define IMAGE_UNIT_BASE(n)   (IMAGE_POOL_BASE + (n) * IMAGE_POOL_SECTOR)

/* 0 号单元（保留的名字，等价于 IMAGE_POOL_BASE）：裸镜像开发路径的默认落点 */
#define IMAGE_UNIT0_BASE     (IMAGE_POOL_BASE)

#define SVCRT_FLASH_END      (CHIP_FLASH_BASE  + CHIP_FLASH_SIZE)   /* 避开 CMSIS 的 FLASH_END 宏 */

/* ---- RAM 布局（地址由低到高：SHARE → KERNEL → SLOT_RAM） ---- */
#define KERNEL_RAM_SIZE      (CHIP_RAM_SIZE - SHARE_RAM_SIZE - SLOT_RAM_TOTAL)
#define SHARE_RAM_BASE       (CHIP_RAM_BASE)
#define KERNEL_RAM_BASE      (SHARE_RAM_BASE  + SHARE_RAM_SIZE)
#define SLOT_RAM_BASE        (KERNEL_RAM_BASE + KERNEL_RAM_SIZE)

/* 镜像 RAM 池（伙伴分配）：
 *   SLOT_RAM_BASE          池基址，按 SLOT_RAM_TOTAL 对齐（MPU region 硬约束）
 *   SLOT_RAM_TOTAL         池总大小
 *   SLOT_RAM_MIN/MAX_BLOCK 单个镜像能拿到的最小/最大块
 * @note 旧版本的「按槽等分窗口」已废弃：镜像数不再等于窗口数，
 *       窗口由伙伴分配器按镜像头声明的 ram_size 现算。 */
#define SLOT_RAM_TOTAL_BYTES (SLOT_RAM_TOTAL)

/* ---- 硬件兼容签名（App 与内核 ABI 匹配校验用） ----
 * 高 16 位：芯片型号标识；低 16 位：内核接口版本。
 * App 打包时应写入相同值，内核加载时校验，不匹配则拒绝加载。
 * v4：镜像自带重定位表 + 细粒度池分配 + 卸载压实（镜像格式与内核必须成对升级）。 */
#define SVCRT_HW_COMPAT_ID   (0x40100005u)   /* 0x4010 = STM32F401; low 16 bits = image compat generation (4 = dynamic load + relocation). NOT the partition table ABI version - that one is SVCRT_PARTITION_VERSION in svcrt_share.h and may differ. */

/* ============================================================
 * 七、镜像在池内的布局（安装路径 vs 开发调试路径）
 * @details .svcapp 镜像 = 固定 256 字节头 + 负载 + 重定位表，整块写在池内
 *          任意 SVCRT_POOL_ALLOC_UNIT 对齐的落点上：
 *
 *              镜像起始             +256                    +256+image_size
 *              +------------------+----------------------+------------------+
 *              | 镜像头 (256 字节) | 负载（代码/只读数据）|  重定位表        |
 *              +------------------+----------------------+------------------+
 *
 *          负载的**链接基址**取镜像头里的 nominal_base（打包时用于算重定位），
 *          与实际落点无关；装载时按 delta = 落点 - nominal_base 逐条打补丁。
 *          因此各 App / 驱动工程的 .sct 生成一份固定基址即可
 *          （tools/gen_scatter.py --target image --nominal），反复链接三次
 *          做差异比对就能得到重定位表（见 tools/pack_app.py）。
 *          entry_offset 是入口相对「负载起始」的偏移，内核计算入口地址时
 *          会先加上头长与本值（见 svcrt_loader_entry_addr）。
 * @note 开发调试用的裸镜像路径（APP_ALLOW_RAW_IMAGE=1）没有镜像头也没有
 *       重定位表，负载直接位于镜像基址，其 .sct 用
 *       tools/gen_scatter.py --raw --dev-slot N 单独生成，两条路径的二进制
 *       互不通用。
 * ============================================================ */
#define APP_IMAGE_HEADER_SIZE (256)          /* .svcapp 镜像头长度，必须与 svcrt_app_image.h 一致 */

/* ---- 镜像链接标称基址（打包时负载被链接到的地址，不是运行期落点） ----
 * 重定位表按「运行期地址 - 标称基址」算增量，因此工具链
 * （tools/gen_scatter.py + tools/pack_app.py）与内核（svcrt_loader.c）
 * 必须用同一组基址：
 *   ROM：负载链接在 SVCRT_APP_NOMINAL_ROM_LOAD（= 池基址 + 镜像头长），
 *        写入镜像头 nominal_base；运行期负载落在 落点 + payload_offset。
 *   RAM：镜像 RW/ZI 链接在 SVCRT_APP_NOMINAL_RAM_BASE（RAM 池基址），
 *        写入镜像头 nominal_ram_base；运行期落在伙伴分配器给的块基址。
 * SVCRT_RELOC_DELTA 是打包工具做差异链接用的偏移量：取 0xF000（非 2 的幂），
 * 避免小整数常量恰好等于增量而被误判成绝对地址。
 * @note 这些宏只供工具链与内核共享；App/Loader 工程不得包含本头文件。 */
#define SVCRT_APP_NOMINAL_ROM_BASE (IMAGE_POOL_BASE)
#define SVCRT_APP_NOMINAL_ROM_LOAD (SVCRT_APP_NOMINAL_ROM_BASE + APP_IMAGE_HEADER_SIZE)
#define SVCRT_APP_NOMINAL_RAM_BASE (SLOT_RAM_BASE)
#define SVCRT_RELOC_DELTA          (0xF000u)
#define SVCRT_DEV_RAM_WINDOW       (8 * 1024u)    /* dev bare-image RAM window；4 个窗口(32KB)必须放得进 32KB 的 RAM 池 */

/* 开发裸镜像的 RAM 窗口：裸镜像走固定地址路径，没有伙伴分配器，
 * 因此按「每个开发槽位一个窗口」静态划分（窗口序号 = 槽位号）。
 * 与动态装载路径互不相干：只有 APP_ALLOW_RAW_IMAGE=1 时才生效。 */

/* ============================================================
 * 十一、安装策略与编译期默认布局（配置区无效时的回退默认）
 * @details 布局的第一来源是设备端配置区（CONFIG_BASE 起一个扇区，
 *          见 tools/svcrt_cfg.py 写入的记录格式）；配置区没有有效记录时，
 *          回退到这里的编译期默认值：
 *
 *            SVCRT_LAYOUT_MODE_AUTO   自动选址：池内细粒度首适配分配，
 *                                     镜像可搬移、卸载按 SVCRT_RECLAIM_MODE
 *                                     压实与回收（v5 起的既有行为）。
 *            SVCRT_LAYOUT_MODE_FIXED  固定槽位：按下方默认槽表/配置区槽表
 *                                     钉死 Flash 落点与 RAM 窗口，卸载只擦
 *                                     本槽，不搬移、空间不退回池。
 *
 *          默认槽表 8 条，条目字段含义（与 svcrt_layout_def.h 的
 *          svcrt_cfg_slot_t 一一对应）：
 *            BASE  槽的 Flash 基址（绝对地址，落在池内、按分配单元对齐）
 *            SIZE  槽大小（字节，至少装得下镜像头 + 一个最小负载）
 *            TYPE  1=App / 2=驱动 / 0=该条目不使用
 *            RAM   槽的 RAM 窗口大小（字节，2 的幂；0 = 该槽不固定 RAM）
 *            BOOT  1 = 复位后自动启动该槽
 *          @note FIXED 模式下 RAM 窗口由槽表决定，不走伙伴分配器。
 * ============================================================ */
#ifndef SVCRT_LAYOUT_MODE_AUTO
#define SVCRT_LAYOUT_MODE_AUTO   (0)
#define SVCRT_LAYOUT_MODE_FIXED  (1)
#endif

#ifndef SVCRT_LAYOUT_DEFAULT_MODE
#define SVCRT_LAYOUT_DEFAULT_MODE (SVCRT_LAYOUT_MODE_AUTO)
#endif

/* 槽表条目上限（配置区与默认槽表共用；<= SVCRT_SLOT_ARRAY_MAX） */
#ifndef SVCRT_CFG_SLOT_MAX
#define SVCRT_CFG_SLOT_MAX        (8)
#endif

/* 默认槽表：全部 0 = 不使用（默认模式是自动选址，用不到固定槽）。
 * 需要「纯固定槽位」作为回退默认时，在此填写 BASE/SIZE/TYPE/RAM/BOOT，
 * 并与 tools/svcrt_layout.py 导出的布局保持一致。 */
#define SVCRT_CFG_SLOT0_BASE      (0)

#define SVCRT_CFG_SLOT0_SIZE      (0)

#define SVCRT_CFG_SLOT0_TYPE      (0)

#define SVCRT_CFG_SLOT0_RAM_SIZE  (0)

#define SVCRT_CFG_SLOT0_AUTOSTART (0)

#define SVCRT_CFG_SLOT1_BASE      (0)

#define SVCRT_CFG_SLOT1_SIZE      (0)

#define SVCRT_CFG_SLOT1_TYPE      (0)

#define SVCRT_CFG_SLOT1_RAM_SIZE  (0)

#define SVCRT_CFG_SLOT1_AUTOSTART (0)

#define SVCRT_CFG_SLOT2_BASE      (0)

#define SVCRT_CFG_SLOT2_SIZE      (0)

#define SVCRT_CFG_SLOT2_TYPE      (0)

#define SVCRT_CFG_SLOT2_RAM_SIZE  (0)

#define SVCRT_CFG_SLOT2_AUTOSTART (0)

#define SVCRT_CFG_SLOT3_BASE      (0)

#define SVCRT_CFG_SLOT3_SIZE      (0)

#define SVCRT_CFG_SLOT3_TYPE      (0)

#define SVCRT_CFG_SLOT3_RAM_SIZE  (0)

#define SVCRT_CFG_SLOT3_AUTOSTART (0)

#define SVCRT_CFG_SLOT4_BASE      (0)

#define SVCRT_CFG_SLOT4_SIZE      (0)

#define SVCRT_CFG_SLOT4_TYPE      (0)

#define SVCRT_CFG_SLOT4_RAM_SIZE  (0)

#define SVCRT_CFG_SLOT4_AUTOSTART (0)

#define SVCRT_CFG_SLOT5_BASE      (0)

#define SVCRT_CFG_SLOT5_SIZE      (0)

#define SVCRT_CFG_SLOT5_TYPE      (0)

#define SVCRT_CFG_SLOT5_RAM_SIZE  (0)

#define SVCRT_CFG_SLOT5_AUTOSTART (0)

#define SVCRT_CFG_SLOT6_BASE      (0)

#define SVCRT_CFG_SLOT6_SIZE      (0)

#define SVCRT_CFG_SLOT6_TYPE      (0)

#define SVCRT_CFG_SLOT6_RAM_SIZE  (0)

#define SVCRT_CFG_SLOT6_AUTOSTART (0)

#define SVCRT_CFG_SLOT7_BASE      (0)

#define SVCRT_CFG_SLOT7_SIZE      (0)

#define SVCRT_CFG_SLOT7_TYPE      (0)

#define SVCRT_CFG_SLOT7_RAM_SIZE  (0)

#define SVCRT_CFG_SLOT7_AUTOSTART (0)

/* ============================================================
 * 八、开发调试策略
 * @details 用 Keil 直接下载 App/驱动并下断点调试时，烧进 Flash 的是**裸镜像**
 *          （无 256 字节镜像头，也没有 CRC），与安装路径的 .svcapp 格式不同。
 *          打开本开关后，内核在扫描池时会把“非擦除状态且不带镜像头”的内容
 *          当作开发期裸镜像，入口取单元基址（|1 置 Thumb），从而保住
 *          “固定地址烧录 + MDK 调试”的开发闭环。
 *          发布固件时应关掉它，只接受带签名的 .svcapp。
 *
 *          裸镜像没有镜像头，扫描器无法从内容推断「类型」与「占几个扇区」，
 *          因此在这里显式声明一张开发槽位表：每个条目给出
 *          {起始单元, 单元数, 类型}，类型取 1=App / 2=驱动（与
 *          svcrt_app_image.h 的 SVCRT_APP_TYPE_x 一致），0 表示该条目不使用。
 *          条目 i 的 RAM 窗口取 SLOT_RAM_BASE_OF(起始单元)。
 *
 *          @note 裸镜像占用的是**整扇区**（MDK 的固定地址布局按扇区对齐），
 *                且它占的扇区会被分配器视为「不可分配」，因此开发期可用的
 *                池容量 = 总容量 - 裸镜像占用 - 压实余量。
 * ============================================================ */
#ifndef APP_ALLOW_RAW_IMAGE
#define APP_ALLOW_RAW_IMAGE     0                /* 1=允许池内直接烧录的裸镜像（开发调试用） */
#endif
/* 默认 0：发布固件只接受带 CRC 的 .svcapp。
 * 开发板在 svcrt_board_config.h 里显式置 1 打开裸镜像旁路，
 * 以保留“固定地址烧录 + MDK 断点调试”的开发闭环。 */

#define SVCRT_DEV_SLOT_MAX   (4)             /* 开发槽位表条目数 */
#define SVCRT_DEV_SLOT0_UNIT  (0)            /* 起始扇区单元 */
#define SVCRT_DEV_SLOT0_UNITS (1)            /* 占用扇区数（必须是 2 的幂） */
#define SVCRT_DEV_SLOT0_TYPE  (2)            /* 2 = 驱动（SVCRT_APP_TYPE_DRIVER） */
#define SVCRT_DEV_SLOT1_UNIT  (1)
#define SVCRT_DEV_SLOT1_UNITS (1)
#define SVCRT_DEV_SLOT1_TYPE  (1)            /* 1 = App（SVCRT_APP_TYPE_APP） */
#define SVCRT_DEV_SLOT2_UNIT  (0)            /* F401 池只 3 个单元、可用 2 个：后两个开发槽不启用 */
#define SVCRT_DEV_SLOT2_UNITS (1)
#define SVCRT_DEV_SLOT2_TYPE  (0)
#define SVCRT_DEV_SLOT3_UNIT  (0)
#define SVCRT_DEV_SLOT3_UNITS (1)
#define SVCRT_DEV_SLOT3_TYPE  (0)

/* 裸镜像占用的扇区数（分配器要把它从可分配空间里刨掉） */
#define SVCRT_DEV_SLOT_SECTORS   ((SVCRT_DEV_SLOT0_TYPE ? SVCRT_DEV_SLOT0_UNITS : 0) + \
                                  (SVCRT_DEV_SLOT1_TYPE ? SVCRT_DEV_SLOT1_UNITS : 0) + \
                                  (SVCRT_DEV_SLOT2_TYPE ? SVCRT_DEV_SLOT2_UNITS : 0) + \
                                  (SVCRT_DEV_SLOT3_TYPE ? SVCRT_DEV_SLOT3_UNITS : 0))

/* 开发裸镜像的 RAM 窗口必须放得进 RAM 池，否则裸镜像路径会在运行期
 * 越出池尾。（本宏定义在池与开发槽位表之后，因此这里的 #if 能真正生效。） */
#if (SVCRT_DEV_SLOT_MAX * SVCRT_DEV_RAM_WINDOW > SLOT_RAM_TOTAL)
#error "dev bare-image RAM windows do not fit in the SLOT_RAM pool"
#endif

/* ============================================================
 * 九、任务容量策略（唯一入口：只改这三个数字）
 * @details 任务表是一块静态 TCB 数组，容量在编译期定死。为了让
 *          “很多用户驱动 + 很多用户应用”的场景可以直接扩到大规模，
 *          容量按 内核 / 驱动 / 应用 三类分层：
 *
 *            SVCRT_TASK_MAX_NUM   任务表总容量（静态 TCB 数组元素个数）
 *            MAX_DRIVER_TASKS     驱动任务数上限（每个驱动镜像占 PER_DRIVER 个任务）
 *            MAX_APP_TASKS        App 任务数上限（每个 App 镜像占 PER_APP 个任务）
 *
 *          其余全部由本文件派生：
 *            SVCRT_TASK_NEED_MIN       三类需求之和（分区能装下所需的最小容量）
 *            SVCRT_TASK_TABLE_RAM_MAX  静态 TCB 数组的 RAM 预算上限（字节）
 *          两者都在下方编译期断言里校验：容量配小了直接编译报错，
 *          而不是运行期静默注册失败。
 * @note 运行时仍可继续用 svcrt_task_register() 动态创建任务，上限就是
 *       SVCRT_TASK_MAX_NUM；上面两个上限只约束固定占用的那部分。
 * ============================================================ */
#ifndef SVCRT_TASK_MAX_NUM
#define SVCRT_TASK_MAX_NUM        (48)   /* 任务表总容量（静态 TCB 数组元素个数） */
#endif

#define SVCRT_TASK_MAX_KERNEL     (8)    /* 内核自带任务 + 示例静态任务的预留 */
#define SVCRT_TASK_PER_DRIVER     (1)    /* 每个驱动镜像占用的任务数 */
#define SVCRT_TASK_PER_APP        (1)    /* 每个 App 镜像占用的任务数 */

/* 驱动 / App 各自的任务数上限：细粒度池里最多 SLOT_MAX 个镜像，全部按驱动算
 * 就是最大驱动任务数，全部按 App 算就是最大 App 任务数；本板驱动与 App
 * 共用一个池，因此两个上限都取 SLOT_MAX，需求之和按 SLOT_MAX 计一次。 */
#define MAX_DRIVER_TASKS          (SLOT_MAX)
#define MAX_APP_TASKS             (SLOT_MAX)

#define SVCRT_TASK_NEED_MIN       (SVCRT_TASK_MAX_KERNEL + \
                                   (SLOT_MAX * (SVCRT_TASK_PER_DRIVER > SVCRT_TASK_PER_APP ? \
                                                SVCRT_TASK_PER_DRIVER : SVCRT_TASK_PER_APP)))

/* 静态 TCB 数组的 RAM 预算（字节）。守护断言直接比较真实的
 * sizeof(svcrt_task_table)，不依赖「每个 TCB 多少字节」的估算：
 * Cortex-M4 上实测 sizeof(svcrt_task_t) = 76 字节（SVCRT_USE_MPU=0）
 * / 140 字节（SVCRT_USE_MPU=1）。
 * 预算固定 12KB（SLOT_MAX 提到 16 后 TCB 表按 16*2 条需求估），
 * 超出时把本宏显式改大即可，改多少就是显式占用多少内核 RAM。 */
#ifndef SVCRT_TASK_TABLE_RAM_MAX
#define SVCRT_TASK_TABLE_RAM_MAX  (12u * 1024u)
#endif

/* ============================================================
 * 十、MPU isolation windows (derived; do not edit by hand)
 * @details svcrt_mpu.c builds every task's MPU context from these windows
 *          plus the runtime slot table (per-image base/span), so the project
 *          still has exactly one place where addresses are defined:
 *          the pool is defined here, the per-image span is a runtime
 *          allocation recorded in the partition table.
 *          An MPU region must be a power of two in size and aligned to that
 *          size. The pool placement policy keeps that property: an image is
 *          first tried at a 2^n-aligned address (exact region); only when an
 *          aligned placement does not fit does the allocator fall back to a
 *          tightly packed one, in which case the ROM window degrades to the
 *          smallest enclosing aligned window (see svcrt_mpu_rom_window).
 * ============================================================ */
#define SVCRT_MPU_KERNEL_ROM_BASE   (KERNEL_BASE)
#define SVCRT_MPU_KERNEL_ROM_SIZE   (KERNEL_SIZE)
#define SVCRT_MPU_IMAGE_POOL_BASE   (IMAGE_POOL_BASE)
#define SVCRT_MPU_IMAGE_POOL_SIZE   (IMAGE_POOL_SIZE)
#define SVCRT_MPU_KERNEL_RAM_FALLBACK (CHIP_RAM_BASE)   /* kernel tasks fall back to the whole chip RAM */
#define SVCRT_MPU_SHARE_RAM_BASE    (SHARE_RAM_BASE)
#define SVCRT_MPU_SHARE_RAM_SIZE    (SHARE_RAM_SIZE)
#define SVCRT_MPU_SLOT_RAM_BASE     (SLOT_RAM_BASE)
#define SVCRT_MPU_SLOT_RAM_TOTAL    (SLOT_RAM_TOTAL)
#define SVCRT_MPU_RAM_BLOCK_MIN     (SLOT_RAM_MIN_BLOCK)
#define SVCRT_MPU_RAM_BLOCK_MAX     (SLOT_RAM_MAX_BLOCK)

/* Power-of-two and alignment self-check: the array size is the product of the
 * individual results, so a single failure makes it negative and the compiler
 * refuses the file. */
#define SVCRT_IS_POW2(x)            (((x) != 0) && (((x) & ((x) - 1)) == 0))
typedef char svcrt_mpu_window_check[
      (SVCRT_IS_POW2(KERNEL_SIZE)                 ? 1 : -1)
    * (SVCRT_IS_POW2(IMAGE_POOL_SECTOR)           ? 1 : -1)
    * (SVCRT_IS_POW2(SVCRT_POOL_ALLOC_UNIT)       ? 1 : -1)
    * (SVCRT_IS_POW2(SLOT_RAM_TOTAL)              ? 1 : -1)
    * (SVCRT_IS_POW2(SLOT_RAM_MIN_BLOCK)          ? 1 : -1)
    * (SVCRT_IS_POW2(SLOT_RAM_MAX_BLOCK)          ? 1 : -1)
    * (SVCRT_IS_POW2(SHARE_RAM_SIZE)              ? 1 : -1)
    * (((SVCRT_POOL_ALLOC_UNIT * 2) <= IMAGE_POOL_SECTOR) ? 1 : -1)
    * (((IMAGE_POOL_SECTOR % SVCRT_POOL_ALLOC_UNIT) == 0) ? 1 : -1)
    * (((KERNEL_BASE      % KERNEL_SIZE)          == 0) ? 1 : -1)
    * (((KERNEL_BASE      % SVCRT_POOL_ALLOC_UNIT) == 0) ? 1 : -1)
    * (((IMAGE_POOL_BASE  % IMAGE_POOL_SECTOR)    == 0) ? 1 : -1)
    * (((IMAGE_POOL_BASE  % SVCRT_POOL_ALLOC_UNIT) == 0) ? 1 : -1)
    * (((SLOT_RAM_BASE    % SLOT_RAM_TOTAL)       == 0) ? 1 : -1)
    * (((SHARE_RAM_BASE   % SHARE_RAM_SIZE)       == 0) ? 1 : -1)
    * (((SLOT_RAM_MAX_BLOCK % SLOT_RAM_MIN_BLOCK) == 0) ? 1 : -1)
    * 1];

/* 池一定是整数个扇区，且必须留出至少一个扇区的压实余量。
 * KERNEL_SIZE 必须是 IMAGE_POOL_SECTOR 的整数倍，否则池基址会落在
 * 扇区中间，地址与扇区的换算会错位（擦除会破坏相邻镜像）。 */
typedef char svcrt_pool_layout_check[
    (((IMAGE_POOL_SIZE % IMAGE_POOL_SECTOR) == 0) &&
     (IMAGE_POOL_UNITS >= 2) &&
     (POOL_RESERVE_SECTORS >= 1) &&
     (POOL_RESERVE_SECTORS < IMAGE_POOL_UNITS) &&
     (((POOL_RESERVE_SECTORS * IMAGE_POOL_SECTOR) % SVCRT_POOL_ALLOC_UNIT) == 0) &&
     ((KERNEL_SIZE % IMAGE_POOL_SECTOR) == 0)) ? 1 : -1];

/* 分配单元至少要装得下镜像头 + 一个最小负载，且要能整块写进一个扇区 */
typedef char svcrt_pool_unit_check[
    ((SVCRT_POOL_ALLOC_UNIT >= (APP_IMAGE_HEADER_SIZE + 256)) &&
     (SVCRT_POOL_ALLOC_UNIT <= IMAGE_POOL_SECTOR)) ? 1 : -1];

/* ============================================================
 * 五、安装器策略（方案A：内核内安装任务）
 * @details 由内核常驻任务从设备流式接收 .svcapp 并安装到池内空闲区，
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
/* ---- 配置区：要么不划分，要么是整数个物理扇区 ---- */
typedef char svcrt_config_region_check[
    (((CONFIG_SIZE % IMAGE_POOL_SECTOR) == 0) &&
     ((CONFIG_SIZE == 0) || (CONFIG_BASE >= (KERNEL_BASE + KERNEL_SIZE))) &&
     ((CONFIG_SIZE == 0) || (CONFIG_BASE == (KERNEL_BASE + KERNEL_SIZE)))) ? 1 : -1];

/* ---- 固定模式的默认槽表必须落在池内、大小合法 ---- */
typedef char svcrt_cfg_slot_check[
    ((SVCRT_CFG_SLOT0_TYPE == 0) || ((SVCRT_CFG_SLOT0_BASE >= IMAGE_POOL_BASE) &&
                                     ((SVCRT_CFG_SLOT0_BASE + SVCRT_CFG_SLOT0_SIZE) <= IMAGE_POOL_END) &&
                                     (SVCRT_CFG_SLOT0_SIZE >= (APP_IMAGE_HEADER_SIZE + 256)))) &&
    ((SVCRT_CFG_SLOT1_TYPE == 0) || ((SVCRT_CFG_SLOT1_BASE >= IMAGE_POOL_BASE) &&
                                     ((SVCRT_CFG_SLOT1_BASE + SVCRT_CFG_SLOT1_SIZE) <= IMAGE_POOL_END) &&
                                     (SVCRT_CFG_SLOT1_SIZE >= (APP_IMAGE_HEADER_SIZE + 256)))) ? 1 : -1];

#if (KERNEL_RAM_SIZE <= 0)
#error "RAM 配置过小：SHARE + SLOT_RAM_TOTAL 已超过 CHIP_RAM_SIZE"
#endif

#if (IMAGE_POOL_SIZE <= 0)
#error "Flash 配置过小：BOOT + KERNEL 已超过 CHIP_FLASH_SIZE"
#endif

#if (IMAGE_POOL_BASE < KERNEL_BASE)
#error "Flash 分区顺序错误"
#endif

/* ---- 任务容量：分层需求必须装得进任务表 ---- */
typedef char svcrt_task_capacity_check[
    (SVCRT_TASK_MAX_NUM >= SVCRT_TASK_NEED_MIN) ? 1 : -1];

/* ---- 最大 RAM 块要装得下：任务栈 + 最小数据；最小块要装得下最小镜像 ---- */
typedef char svcrt_slot_ram_fit_check[
    ((SLOT_RAM_MAX_BLOCK >= (APP_TASK_STACK_SIZE + 512)) &&
     (SLOT_RAM_MAX_BLOCK >= (DRIVER_TASK_STACK_SIZE + 512)) &&
     (SLOT_RAM_MIN_BLOCK >= 512) &&
     (SLOT_RAM_MAX_BLOCK <= SLOT_RAM_TOTAL)) ? 1 : -1];

/* ---- 池扇区必须装得下：镜像头 + 最小负载 ---- */
typedef char svcrt_slot_flash_fit_check[
    (IMAGE_POOL_SECTOR >= (APP_IMAGE_HEADER_SIZE + 1024)) ? 1 : -1];

/* ---- 开发槽位表：单元必须落在池内，且单元数是 2 的幂 ---- */
typedef char svcrt_dev_slot_check[
    ((SVCRT_DEV_SLOT0_TYPE == 0 || ((SVCRT_DEV_SLOT0_UNIT + SVCRT_DEV_SLOT0_UNITS) <= IMAGE_POOL_UNITS)) &&
     (SVCRT_DEV_SLOT1_TYPE == 0 || ((SVCRT_DEV_SLOT1_UNIT + SVCRT_DEV_SLOT1_UNITS) <= IMAGE_POOL_UNITS)) &&
     (SVCRT_DEV_SLOT2_TYPE == 0 || ((SVCRT_DEV_SLOT2_UNIT + SVCRT_DEV_SLOT2_UNITS) <= IMAGE_POOL_UNITS)) &&
     (SVCRT_DEV_SLOT3_TYPE == 0 || ((SVCRT_DEV_SLOT3_UNIT + SVCRT_DEV_SLOT3_UNITS) <= IMAGE_POOL_UNITS)) &&
     (SVCRT_IS_POW2(SVCRT_DEV_SLOT0_UNITS)) &&
     (SVCRT_IS_POW2(SVCRT_DEV_SLOT1_UNITS)) &&
     (SVCRT_IS_POW2(SVCRT_DEV_SLOT2_UNITS)) &&
     (SVCRT_IS_POW2(SVCRT_DEV_SLOT3_UNITS))) ? 1 : -1];

#endif /* SVCRT_PARTITION_H */
