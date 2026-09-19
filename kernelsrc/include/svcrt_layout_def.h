/**
* @file svcrt_layout_def.h
* @brief On-device layout + runtime configuration record. This is the single
*        persistent place where a device's install strategy (fixed slots vs.
*        automatic placement), its fixed slot table and a small set of runtime
*        knobs are stored, so a customer can configure a board without
*        rebuilding the kernel.
* @details The record lives in the CONFIG region (one physical erase unit
*          between the kernel and the image pool). It is written by the host
*          tool (tools/svcrt_cfg.py) over the serial link and read by the
*          kernel at boot. When no valid record exists -- brand new board,
*          erased region, bad CRC, hardware mismatch -- the compile-time
*          default layout in config/svcrt_partition.h is used instead.
*
*          Storage inside the CONFIG region: each record is
*          SVCRT_CFG_RECORD_SIZE bytes and records are appended one after
*          another until the region is full; then the whole region is erased
*          once and writing restarts at offset 0. The live record is the
*          valid one with the highest seq, so a reset during a write loses
*          nothing (the previous record is still intact). Only a reset during
*          the single erase loses the configuration, which then falls back to
*          the compile-time default -- the device never ends up unusable.
*
*          All fields are little-endian 32-bit words, the structure has no
*          padding, and its size is asserted at compile time.
*
* @note Keep this header free of kernel internals: it is also parsed by the
*       host tools and by the App/driver side, so it must not pull in board
*       or partition settings. Addresses live in config/svcrt_partition.h.
*/

#ifndef __SVCRT_LAYOUT_DEF_H__
#define __SVCRT_LAYOUT_DEF_H__

#include "svcrt_types.h"

/* ============================================================
 * 一、记录标识
 * ============================================================ */
#define SVCRT_CFG_MAGIC          (0x47464353u)   /* "SCFG" as little-endian u32 */
#define SVCRT_CFG_VERSION        (1u)

/* ============================================================
 * 二、安装策略模式
 * @note 权威定义在 config/svcrt_partition.h（内核侧默认模式要用到它）；
 *       这里给出同样的值，供不包含分区头的代码（工具、驱动侧）使用。
 * ============================================================ */
#ifndef SVCRT_LAYOUT_MODE_AUTO
#define SVCRT_LAYOUT_MODE_AUTO   (0u)   /* automatic placement: first-fit pool, movable, reclaimable */
#define SVCRT_LAYOUT_MODE_FIXED  (1u)   /* fixed slots: placement and RAM window are pinned by the slot table */
#endif

/* ============================================================
 * 三、当前生效布局的来源（上电解析结果，不是持久数据）
 * ============================================================ */
#define SVCRT_LAYOUT_SOURCE_DEFAULT  (0u)   /* compile-time default layout */
#define SVCRT_LAYOUT_SOURCE_CONFIG   (1u)   /* device-side CONFIG region */

/* ============================================================
 * 四、记录几何
 * ============================================================ */
#define SVCRT_CFG_RECORD_SIZE    (512u)                       /* bytes per record */
#define SVCRT_CFG_SLOT_SIZE      (32u)                        /* bytes per slot entry */
#ifndef SVCRT_CFG_SLOT_MAX
#define SVCRT_CFG_SLOT_MAX       (8u)                         /* keep in sync with config/svcrt_partition.h */
#endif
#define SVCRT_CFG_CRC_OFFSET     (SVCRT_CFG_RECORD_SIZE - 4u) /* crc32 is the last word */

/* Upper bound of svcrt_cfg_record_t.boot_delay_ms. The value only buys time
 * for a debugger or a host tool to attach before anything autostarts, so an
 * unbounded value would just look like a dead board; a minute is plenty. */
#define SVCRT_CFG_BOOT_DELAY_MAX (60000u)

/* ---- record flags (svcrt_cfg_record_t.flags) ---- */
#define SVCRT_CFG_FLAG_RAW_ALLOW (0x1u)   /* dev bypass: accept a headed-less bare image burned at a slot base */

/* ---- slot entry type (same values as svcrt_app_image.h) ---- */
#define SVCRT_CFG_TYPE_UNUSED    (0u)
#define SVCRT_CFG_TYPE_APP       (1u)
#define SVCRT_CFG_TYPE_DRIVER    (2u)

/* ---- reclaim mode (svcrt_cfg_record_t.reclaim_mode) ----
 * Same values as SVCRT_RECLAIM_GLOBAL / SVCRT_RECLAIM_MINIMAL in
 * config/svcrt_partition.h. Only meaningful in AUTO mode: FIXED mode never
 * moves an image, so it never reclaims anything back into the pool. */
#define SVCRT_CFG_RECLAIM_GLOBAL  (0u)   /* compact the whole pool on uninstall / boot */
#define SVCRT_CFG_RECLAIM_MINIMAL (1u)   /* only touch sectors that hold invalid bytes    */
/* FIXED mode reports this as its reclaim mode; it is not selectable. */
#define SVCRT_CFG_RECLAIM_NONE    (2u)

/* ============================================================
 * 五、运行期参数（runtime knobs）
 * @details 这些值原来是编译期宏，放进配置区的唯一理由是「同一份固件要能
 *          按设备/客户不同而不同」。凡是会影响时基、分区几何、中断优先级
 *          的宏都**不能**放进来——那些值一改，链接脚本和 MPU 配置就跟着
 *          失效，属于必须重新编译的量。
 *          当前内核真正会去读的有：log_level、fault_restart_max、
 *          boot_delay_ms，以及 flags 里的 RAW_ALLOW 位。
 *          其余三个字段仍然只接受 0，写非 0 会被校验拒绝
 *          （SVCRT_CFG_ERR_NOT_IMPL），而不是默默忽略——「配置里写了但设备
 *          没照做」是最难查的一类问题。它们留在记录里是为了让结构体大小
 *          与偏移跨版本稳定，将来真接了再放开；今天不接的理由各自是：
 *            watchdog_ms          内核没有 IWDG 端口层，而且 Flash 擦除
 *                                 （一个 128K 扇区约 2s）会阻塞喂狗，安装
 *                                 过程中有被看门狗复位的风险；
 *            heap_size            POSIX 层的堆是 App 侧静态 arena，内核
 *                                 自己不持有堆，「给内核一个堆」没有消费者；
 *            thread_stack_default 线程栈由调用者在自己的 RAM 里声明
 *                                 （svcrt_thread_create 的 stack 参数），
 *                                 内核没有可分配的匿名栈池。
 * ============================================================ */
#define SVCRT_CFG_KNOB_UNSET     (0u)

/* ============================================================
 * 六、校验结果码（svcrt_layout_validate 的返回，也是串口打印的原因码）
 * ============================================================ */
#define SVCRT_CFG_OK                 (0u)
#define SVCRT_CFG_ERR_MAGIC          (1u)
#define SVCRT_CFG_ERR_VERSION        (2u)
#define SVCRT_CFG_ERR_CRC            (3u)
#define SVCRT_CFG_ERR_HW             (4u)   /* hw_compat_id does not match the kernel */
#define SVCRT_CFG_ERR_MODE           (5u)
#define SVCRT_CFG_ERR_COUNT          (6u)   /* slot_count out of range */
#define SVCRT_CFG_ERR_SLOT_TYPE      (7u)
#define SVCRT_CFG_ERR_SLOT_RANGE     (8u)   /* slot not inside the pool */
#define SVCRT_CFG_ERR_SLOT_SIZE      (9u)   /* slot too small or not carrying a payload */
#define SVCRT_CFG_ERR_SLOT_ALIGN     (10u)  /* slot base not aligned to the allocation unit */
#define SVCRT_CFG_ERR_SLOT_OVERLAP   (11u)
#define SVCRT_CFG_ERR_SLOT_RAM       (12u)  /* RAM window not a power of two / out of the RAM pool / misaligned */
#define SVCRT_CFG_ERR_SLOT_COUNT_MAX (13u)  /* more entries than the kernel can hold */
#define SVCRT_CFG_ERR_RECLAIM        (14u)  /* reclaim_mode value not understood */
#define SVCRT_CFG_ERR_KNOB           (15u)  /* a runtime knob is out of range */
#define SVCRT_CFG_ERR_NOT_IMPL       (16u)  /* the field is understood but this kernel build ignores it */
#define SVCRT_CFG_ERR_RESERVED       (17u)  /* reserved words not zero: written by a newer tool */
#define SVCRT_CFG_ERR_SLOTS_IN_MODE  (18u)  /* AUTO mode must not carry a slot table, FIXED must */

/* ============================================================
 * 七、记录结构
 * ============================================================ */

/** @brief One fixed slot: Flash placement + RAM window of a headed or bare image. */
typedef struct {
    uint32 base;        /* slot base address (absolute, inside the image pool) */
    uint32 size;        /* slot size in bytes (>= header + minimal payload) */
    uint32 type;        /* SVCRT_CFG_TYPE_x; 0 = entry not used */
    uint32 ram_base;    /* RAM window base (0 = let the buddy allocator decide, AUTO-mode behaviour) */
    uint32 ram_size;    /* RAM window size in bytes (power of two, 0 = not pinned) */
    uint32 autostart;   /* non-zero = start this slot after boot */
    uint32 flags;       /* per-slot flags, reserved (must be 0) */
    uint32 reserved;    /* must be 0 */
} svcrt_cfg_slot_t;

/** @brief The persistent layout + runtime configuration record (512 bytes, no padding). */
typedef struct {
    /* ---- identity and mode ---- */
    uint32 magic;                       /* SVCRT_CFG_MAGIC */
    uint32 version;                     /* SVCRT_CFG_VERSION */
    uint32 hw_compat_id;                /* must equal the kernel's SVCRT_HW_COMPAT_ID */
    uint32 mode;                        /* SVCRT_LAYOUT_MODE_x */
    uint32 seq;                         /* monotonically increasing; the highest valid seq wins */
    uint32 slot_count;                  /* number of meaningful entries in slot[] */
    uint32 flags;                       /* SVCRT_CFG_FLAG_x */
    uint32 reclaim_mode;                /* SVCRT_CFG_RECLAIM_x (AUTO mode only) */

    /* ---- fixed slot table (FIXED mode) ---- */
    svcrt_cfg_slot_t slot[SVCRT_CFG_SLOT_MAX];

    /* ---- runtime knobs ---- */
    uint32 log_level;                   /* 0 = keep the compile-time level; else SVCRT_LOG_NONE..DEBUG */
    uint32 fault_restart_max;           /* 0 = keep APP_CRASH_RESTART_MAX; else 1..64 consecutive faults */
    uint32 boot_delay_ms;               /* ms to hold before autostart; 0 = none (debugger attach window) */
    uint32 watchdog_ms;                 /* reserved: must be 0 (no IWDG port layer yet, see the knob notes) */
    uint32 heap_size;                   /* reserved: must be 0 (the kernel owns no heap, see the knob notes) */
    uint32 thread_stack_default;        /* reserved: must be 0 (stacks are caller-declared RAM, see the notes) */
    uint32 tool_version;                /* host tool revision, informational only */

    /* ---- tail ---- */
    uint32 reserved[48];                /* must be 0; pad to SVCRT_CFG_CRC_OFFSET */
    uint32 crc32;                       /* zlib-compatible CRC32 over the record with this word taken as 0 */
} svcrt_cfg_record_t;

/* Size and layout self-check: a mismatch makes the array negative and the
 * compiler refuses the file instead of silently writing the wrong offsets. */
typedef char svcrt_cfg_record_check[
      ((sizeof(svcrt_cfg_slot_t)  == SVCRT_CFG_SLOT_SIZE)   ? 1 : -1)
    * ((sizeof(svcrt_cfg_record_t) == SVCRT_CFG_RECORD_SIZE) ? 1 : -1)
    * 1];

#endif /* __SVCRT_LAYOUT_DEF_H__ */
