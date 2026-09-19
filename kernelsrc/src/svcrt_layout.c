/**
* @file svcrt_layout.c
* @brief Reads, validates and publishes the device-side layout configuration
*        (install strategy + fixed slot table), and is the only writer of the
*        CONFIG region.
* @details See svcrt_layout.h for the contract and svcrt_layout_def.h for the
*          record format. Behaviour in one paragraph: the compile-time default
*          layout is always loaded first, then the newest valid record in the
*          CONFIG region may override it. Anything questionable about a record
*          (bad magic/CRC/hw id, illegal slot table, fixed mode without a single
*          usable slot) keeps the default and is reported as a reason code, so
*          the device boots rather than dying on a bad configuration.
*
* @note The record is written by appending 512-byte records until the region is
*       full, then the region is erased once. A reset in the middle of a write
*       keeps the previous record; only a reset during that single erase loses
*       the configuration, and the device falls back to the default layout.
*       This is why one erase unit is enough for the CONFIG region and no
*       second sector has to be reserved.
*
* @author xw
*/

#include "svcrt_layout.h"
#include "svcrt_partition.h"
#include "svcrt_ptable.h"
#include "svcrt_app_image.h"    /* svcrt_crc32 */
#include "svcrt_hal.h"          /* svcrt_port_flash_erase / _write */
#include "svcrt_log.h"          /* svcrt_log_set_level */
#include "svcrt_log_defs.h"     /* SVCRT_LOG_NONE..SVCRT_LOG_DEBUG */

/* ============================================================
 * 运行期状态
 * ============================================================ */
static svcrt_cfg_slot_t svcrt_layout_slots_ram[SVCRT_CFG_SLOT_MAX];
static uint32 svcrt_layout_mode_ram   = SVCRT_LAYOUT_MODE_AUTO;
static uint32 svcrt_layout_source_ram = SVCRT_LAYOUT_SOURCE_DEFAULT;
static uint32 svcrt_layout_count_ram  = 0u;
static uint32 svcrt_layout_reason_ram = SVCRT_CFG_OK;
static uint32 svcrt_layout_reclaim_ram = (uint32)SVCRT_RECLAIM_MODE;
static uint32 svcrt_layout_cfglvl_ram  = 0u;   /* 0 = keep the compile-time level */
static uint32 svcrt_layout_restart_ram = (uint32)APP_CRASH_RESTART_MAX;

/* 写操作的暂存区。写入只可能来自两条串行路径（启动期与 shell 命令），
 * 两者不会并发，因此用静态缓冲省下 1KB 的调用者栈。 */
static svcrt_cfg_record_t svcrt_layout_wr_rec;

#define SVCRT_LAYOUT_RECORDS   (CONFIG_SIZE / SVCRT_CFG_RECORD_SIZE)
#define SVCRT_LAYOUT_ERASED    (0xFFFFFFFFu)

#define SVCRT_LAYOUT_IS_POW2(x)  (((x) != 0u) && (((x) & ((x) - 1u)) == 0u))

/* ============================================================
 * 小工具（内核不依赖 libc）
 * ============================================================ */
static void svcrt_layout_memcpy(uint8 *dst, const uint8 *src, uint32 len)
{
    uint32 i;
    for(i = 0u; i < len; i++)
    {
        dst[i] = src[i];
    }
}

static int32 svcrt_layout_memcmp(const uint8 *a, const uint8 *b, uint32 len)
{
    uint32 i;
    for(i = 0u; i < len; i++)
    {
        if(a[i] != b[i])
        {
            return ((int32)a[i] - (int32)b[i]);
        }
    }
    return 0;
}

static uint32 svcrt_layout_align_up(uint32 v, uint32 a)
{
    return (v + (a - 1u)) & ~(a - 1u);
}

/* ============================================================
 * 编译期默认槽表
 * ============================================================ */
static void svcrt_layout_default_slot(uint32 i, svcrt_cfg_slot_t *d)
{
    d->base      = 0u;
    d->size      = 0u;
    d->type      = SVCRT_CFG_TYPE_UNUSED;
    d->ram_base  = 0u;
    d->ram_size  = 0u;
    d->autostart = 0u;
    d->flags     = 0u;
    d->reserved  = 0u;

    switch(i)
    {
    case 0u:
        d->base      = (uint32)SVCRT_CFG_SLOT0_BASE;
        d->size      = (uint32)SVCRT_CFG_SLOT0_SIZE;
        d->type      = (uint32)SVCRT_CFG_SLOT0_TYPE;
        d->ram_size  = (uint32)SVCRT_CFG_SLOT0_RAM_SIZE;
        d->autostart = (uint32)SVCRT_CFG_SLOT0_AUTOSTART;
        break;
    case 1u:
        d->base      = (uint32)SVCRT_CFG_SLOT1_BASE;
        d->size      = (uint32)SVCRT_CFG_SLOT1_SIZE;
        d->type      = (uint32)SVCRT_CFG_SLOT1_TYPE;
        d->ram_size  = (uint32)SVCRT_CFG_SLOT1_RAM_SIZE;
        d->autostart = (uint32)SVCRT_CFG_SLOT1_AUTOSTART;
        break;
    case 2u:
        d->base      = (uint32)SVCRT_CFG_SLOT2_BASE;
        d->size      = (uint32)SVCRT_CFG_SLOT2_SIZE;
        d->type      = (uint32)SVCRT_CFG_SLOT2_TYPE;
        d->ram_size  = (uint32)SVCRT_CFG_SLOT2_RAM_SIZE;
        d->autostart = (uint32)SVCRT_CFG_SLOT2_AUTOSTART;
        break;
    case 3u:
        d->base      = (uint32)SVCRT_CFG_SLOT3_BASE;
        d->size      = (uint32)SVCRT_CFG_SLOT3_SIZE;
        d->type      = (uint32)SVCRT_CFG_SLOT3_TYPE;
        d->ram_size  = (uint32)SVCRT_CFG_SLOT3_RAM_SIZE;
        d->autostart = (uint32)SVCRT_CFG_SLOT3_AUTOSTART;
        break;
    case 4u:
        d->base      = (uint32)SVCRT_CFG_SLOT4_BASE;
        d->size      = (uint32)SVCRT_CFG_SLOT4_SIZE;
        d->type      = (uint32)SVCRT_CFG_SLOT4_TYPE;
        d->ram_size  = (uint32)SVCRT_CFG_SLOT4_RAM_SIZE;
        d->autostart = (uint32)SVCRT_CFG_SLOT4_AUTOSTART;
        break;
    case 5u:
        d->base      = (uint32)SVCRT_CFG_SLOT5_BASE;
        d->size      = (uint32)SVCRT_CFG_SLOT5_SIZE;
        d->type      = (uint32)SVCRT_CFG_SLOT5_TYPE;
        d->ram_size  = (uint32)SVCRT_CFG_SLOT5_RAM_SIZE;
        d->autostart = (uint32)SVCRT_CFG_SLOT5_AUTOSTART;
        break;
    case 6u:
        d->base      = (uint32)SVCRT_CFG_SLOT6_BASE;
        d->size      = (uint32)SVCRT_CFG_SLOT6_SIZE;
        d->type      = (uint32)SVCRT_CFG_SLOT6_TYPE;
        d->ram_size  = (uint32)SVCRT_CFG_SLOT6_RAM_SIZE;
        d->autostart = (uint32)SVCRT_CFG_SLOT6_AUTOSTART;
        break;
    case 7u:
        d->base      = (uint32)SVCRT_CFG_SLOT7_BASE;
        d->size      = (uint32)SVCRT_CFG_SLOT7_SIZE;
        d->type      = (uint32)SVCRT_CFG_SLOT7_TYPE;
        d->ram_size  = (uint32)SVCRT_CFG_SLOT7_RAM_SIZE;
        d->autostart = (uint32)SVCRT_CFG_SLOT7_AUTOSTART;
        break;
    default:
        break;
    }
}

/* RAM 窗口：槽表只给了大小、没给基址时（编译期默认表就是这样），按槽序在
 * 镜像 RAM 池里顺序切分，天然满足「2 的幂 + 按大小对齐」的 MPU 约束。 */
static void svcrt_layout_pin_ram(void)
{
    uint32 cursor = SLOT_RAM_BASE;
    uint32 i;

    for(i = 0u; i < SVCRT_CFG_SLOT_MAX; i++)
    {
        if((svcrt_layout_slots_ram[i].type == SVCRT_CFG_TYPE_UNUSED) ||
           (svcrt_layout_slots_ram[i].ram_size == 0u) ||
           (svcrt_layout_slots_ram[i].ram_base != 0u))
        {
            continue;
        }
        cursor = svcrt_layout_align_up(cursor, svcrt_layout_slots_ram[i].ram_size);
        svcrt_layout_slots_ram[i].ram_base = cursor;
        cursor += svcrt_layout_slots_ram[i].ram_size;
    }
}

/* ============================================================
 * 采用 / 发布
 * ============================================================ */
static uint32 svcrt_layout_count_used(const svcrt_cfg_slot_t *tbl, uint32 count)
{
    uint32 i;
    uint32 n = 0u;

    for(i = 0u; i < count; i++)
    {
        if(tbl[i].type != SVCRT_CFG_TYPE_UNUSED)
        {
            n++;
        }
    }
    return n;
}

static void svcrt_layout_use_default(void)
{
    uint32 i;

    for(i = 0u; i < SVCRT_CFG_SLOT_MAX; i++)
    {
        svcrt_layout_default_slot(i, &svcrt_layout_slots_ram[i]);
    }
    svcrt_layout_mode_ram   = (uint32)SVCRT_LAYOUT_DEFAULT_MODE;
    svcrt_layout_source_ram = SVCRT_LAYOUT_SOURCE_DEFAULT;
    svcrt_layout_count_ram  = svcrt_layout_count_used(svcrt_layout_slots_ram, SVCRT_CFG_SLOT_MAX);
    svcrt_layout_reclaim_ram = (uint32)SVCRT_RECLAIM_MODE;
    svcrt_layout_cfglvl_ram  = 0u;
    svcrt_layout_restart_ram = (uint32)APP_CRASH_RESTART_MAX;
    svcrt_layout_pin_ram();
}

static void svcrt_layout_adopt(const svcrt_cfg_record_t *rec)
{
    uint32 i;

    for(i = 0u; i < SVCRT_CFG_SLOT_MAX; i++)
    {
        svcrt_layout_slots_ram[i] = rec->slot[i];
    }
    svcrt_layout_mode_ram   = rec->mode;
    svcrt_layout_source_ram = SVCRT_LAYOUT_SOURCE_CONFIG;
    svcrt_layout_count_ram  = svcrt_layout_count_used(svcrt_layout_slots_ram, SVCRT_CFG_SLOT_MAX);
    /* FIXED 模式从不搬移镜像，也就从不回收，回收力度对它没有意义 */
    svcrt_layout_reclaim_ram = (rec->mode == SVCRT_LAYOUT_MODE_FIXED)
                             ? (uint32)SVCRT_CFG_RECLAIM_NONE : rec->reclaim_mode;
    svcrt_layout_cfglvl_ram  = rec->log_level;
    svcrt_layout_restart_ram = (rec->fault_restart_max != 0u)
                             ? rec->fault_restart_max : (uint32)APP_CRASH_RESTART_MAX;
    svcrt_layout_pin_ram();
}

/* 把结果写进共享分区表：Loader 与 App 侧只经这里看布局，不再直接读配置区 */
static void svcrt_layout_publish(void)
{
    svcrt_partition_table_t *pt = svcrt_ptable_get();

    if(pt == 0)
    {
        return;
    }
    pt->layout_mode    = svcrt_layout_mode_ram;
    pt->layout_source  = svcrt_layout_source_ram;
    pt->config_base    = (CONFIG_SIZE > 0u) ? (uint32)CONFIG_BASE : 0u;
    pt->config_size    = (uint32)CONFIG_SIZE;
    pt->cfg_slot_count  = svcrt_layout_count_ram;
    pt->reclaim_mode    = svcrt_layout_reclaim_ram;
    pt->cfg_log_level   = svcrt_layout_cfglvl_ram;
    pt->cfg_restart_max = svcrt_layout_restart_ram;
}

/* ============================================================
 * CRC 与校验
 * ============================================================ */
uint32 svcrt_layout_crc(const svcrt_cfg_record_t *rec)
{
    uint32 zero4 = 0u;
    uint32 crc;

    crc = svcrt_crc32((const void *)rec, SVCRT_CFG_CRC_OFFSET, 0u);
    crc = svcrt_crc32((const void *)&zero4, 4u, crc);
    return crc;
}

uint32 svcrt_layout_validate(const svcrt_cfg_record_t *rec)
{
    uint32 i;
    uint32 j;
    uint32 used = 0u;

    if(rec == 0)
    {
        return SVCRT_CFG_ERR_MAGIC;
    }
    if(rec->magic != SVCRT_CFG_MAGIC)
    {
        return SVCRT_CFG_ERR_MAGIC;
    }
    if(rec->version != SVCRT_CFG_VERSION)
    {
        return SVCRT_CFG_ERR_VERSION;
    }
    if(rec->crc32 != svcrt_layout_crc(rec))
    {
        return SVCRT_CFG_ERR_CRC;
    }
    if(rec->hw_compat_id != SVCRT_HW_COMPAT_ID)
    {
        return SVCRT_CFG_ERR_HW;
    }
    if((rec->mode != SVCRT_LAYOUT_MODE_AUTO) && (rec->mode != SVCRT_LAYOUT_MODE_FIXED))
    {
        return SVCRT_CFG_ERR_MODE;
    }
    if(rec->slot_count > SVCRT_CFG_SLOT_MAX)
    {
        return SVCRT_CFG_ERR_SLOT_COUNT_MAX;
    }

    for(i = 0u; i < SVCRT_CFG_SLOT_MAX; i++)
    {
        const svcrt_cfg_slot_t *s = &rec->slot[i];

        if((i >= rec->slot_count) || (s->type == SVCRT_CFG_TYPE_UNUSED))
        {
            continue;
        }
        used++;

        if((s->type != SVCRT_CFG_TYPE_APP) && (s->type != SVCRT_CFG_TYPE_DRIVER))
        {
            return SVCRT_CFG_ERR_SLOT_TYPE;
        }
        if((s->flags != 0u) || (s->reserved != 0u))
        {
            return SVCRT_CFG_ERR_RESERVED;
        }
        if((s->base < IMAGE_POOL_BASE) || (s->base >= IMAGE_POOL_END) ||
           (s->size > (IMAGE_POOL_END - s->base)))
        {
            return SVCRT_CFG_ERR_SLOT_RANGE;
        }
        if(s->size < (APP_IMAGE_HEADER_SIZE + 256u))
        {
            return SVCRT_CFG_ERR_SLOT_SIZE;
        }
        if((s->base % SVCRT_POOL_ALLOC_UNIT) != 0u)
        {
            return SVCRT_CFG_ERR_SLOT_ALIGN;
        }

        /* 固定槽位是「给客户用 MDK 直接下载」的稳定落点，因此多两条要求：
         *   1) 整槽落在整个物理扇区上 —— 卸载时擦除的就是它自己，不会连带
         *      擦掉邻居（擦除粒度是扇区，不是字节）；
         *   2) RAM 窗口必须钉死 —— 固定地址的镜像若去抢伙伴分配器的块，
         *      同一块 RAM 可能被两个镜像同时拿到。
         * 只对 FIXED 生效：AUTO 模式里的槽表来自「上一次自动安装的结果」，
         * 它本来就落在分配粒度上、RAM 也由伙伴分配器给出。 */
        if(rec->mode == SVCRT_LAYOUT_MODE_FIXED)
        {
            if(((s->base % (uint32)IMAGE_POOL_SECTOR) != 0u) ||
               ((s->size % (uint32)IMAGE_POOL_SECTOR) != 0u))
            {
                return SVCRT_CFG_ERR_SLOT_ALIGN;
            }
            if((s->ram_base == 0u) || (s->ram_size == 0u))
            {
                return SVCRT_CFG_ERR_SLOT_RAM;
            }
        }
        if(s->ram_size != 0u)
        {
            if((SVCRT_LAYOUT_IS_POW2(s->ram_size) == 0u) ||
               (s->ram_size < SLOT_RAM_MIN_BLOCK) ||
               (s->ram_size > SLOT_RAM_MAX_BLOCK))
            {
                return SVCRT_CFG_ERR_SLOT_RAM;
            }
            if(s->ram_base != 0u)
            {
                if(((s->ram_base % s->ram_size) != 0u) ||
                   (s->ram_base < SLOT_RAM_BASE) ||
                   (s->ram_size > ((SLOT_RAM_BASE + SLOT_RAM_TOTAL) - s->ram_base)))
                {
                    return SVCRT_CFG_ERR_SLOT_RAM;
                }
            }
        }

        /* Flash 区间两两不重叠；RAM 窗口（若都固定）同样不重叠 */
        for(j = 0u; j < i; j++)
        {
            const svcrt_cfg_slot_t *o = &rec->slot[j];

            if((j >= rec->slot_count) || (o->type == SVCRT_CFG_TYPE_UNUSED))
            {
                continue;
            }
            if((s->base < (o->base + o->size)) && (o->base < (s->base + s->size)))
            {
                return SVCRT_CFG_ERR_SLOT_OVERLAP;
            }
            if((s->ram_size != 0u) && (o->ram_size != 0u) && (s->ram_base != 0u) && (o->ram_base != 0u))
            {
                if((s->ram_base < (o->ram_base + o->ram_size)) &&
                   (o->ram_base < (s->ram_base + s->ram_size)))
                {
                    return SVCRT_CFG_ERR_SLOT_OVERLAP;
                }
            }
        }
    }

    /* 模式与槽表必须一致：
     *   FIXED 至少要有一个可用槽，否则设备装不了也跑不了任何镜像；
     *   AUTO  不许带槽表——自动选址会自己找落点，槽表放着也不会被读，
     *         那就等于「配置里写了、设备没照做」，必须当场报错。 */
    if(rec->mode == SVCRT_LAYOUT_MODE_FIXED)
    {
        if(used == 0u)
        {
            return SVCRT_CFG_ERR_SLOTS_IN_MODE;
        }
    }
    else if(used != 0u)
    {
        return SVCRT_CFG_ERR_SLOTS_IN_MODE;
    }

    /* 回收力度与运行期参数 */
    if((rec->reclaim_mode != SVCRT_CFG_RECLAIM_GLOBAL) &&
       (rec->reclaim_mode != SVCRT_CFG_RECLAIM_MINIMAL))
    {
        return SVCRT_CFG_ERR_RECLAIM;
    }
    if(rec->log_level > (uint32)SVCRT_LOG_DEBUG)
    {
        return SVCRT_CFG_ERR_KNOB;
    }
    if(rec->fault_restart_max > 64u)
    {
        return SVCRT_CFG_ERR_KNOB;
    }

    /* 已知但本内核还没接的项：拒绝，不默默忽略 */
    if(rec->flags != 0u)
    {
        return SVCRT_CFG_ERR_NOT_IMPL;   /* RAW_ALLOW 随固定槽位路径一起生效 */
    }
    if((rec->boot_delay_ms != 0u) || (rec->watchdog_ms != 0u) ||
       (rec->heap_size != 0u) || (rec->thread_stack_default != 0u))
    {
        return SVCRT_CFG_ERR_NOT_IMPL;
    }

    /* 保留区非 0 说明这份记录由更新的工具写出，
     * 本内核看不懂。把不认识的位当 0 用就是假的——
     * 宁可拒绝（回退编译期默认布局）也不能猜。 */
    for(i = 0u; i < (uint32)(sizeof(rec->reserved) / sizeof(rec->reserved[0])); i++)
    {
        if(rec->reserved[i] != 0u)
        {
            return SVCRT_CFG_ERR_RESERVED;
        }
    }
    return SVCRT_CFG_OK;
}

/* ============================================================
 * 扫描 / 读取
 * ============================================================ */
static const svcrt_cfg_record_t *svcrt_layout_scan(uint32 *p_index, uint32 *p_seq)
{
    const svcrt_cfg_record_t *best = 0;
    uint32 best_seq = 0u;
    uint32 best_idx = 0u;
    uint32 i;

    for(i = 0u; i < SVCRT_LAYOUT_RECORDS; i++)
    {
        const svcrt_cfg_record_t *r =
            (const svcrt_cfg_record_t *)(uint32)(CONFIG_BASE + (i * SVCRT_CFG_RECORD_SIZE));
        uint32 magic = *(volatile const uint32 *)r;

        if(magic == SVCRT_LAYOUT_ERASED)
        {
            break;                      /* 记录顺序追加：遇到空位就不会有更新的了 */
        }
        if(magic != SVCRT_CFG_MAGIC)
        {
            continue;                   /* 半写或陌生内容：跳过，继续找 */
        }
        if(svcrt_layout_validate(r) != SVCRT_CFG_OK)
        {
            continue;
        }
        if((best == 0) || (r->seq > best_seq))
        {
            best = r;
            best_seq = r->seq;
            best_idx = i;
        }
    }

    if(best != 0)
    {
        if(p_index != 0)
        {
            *p_index = best_idx;
        }
        if(p_seq != 0)
        {
            *p_seq = best_seq;
        }
    }
    return best;
}

int32 svcrt_layout_read(svcrt_cfg_record_t *out, uint32 *p_seq)
{
    const svcrt_cfg_record_t *r;

    if(out == 0)
    {
        return SVCRT_LAYOUT_ERR_ARG;
    }
    r = svcrt_layout_scan(0, p_seq);
    if(r == 0)
    {
        return SVCRT_LAYOUT_ERR_INVALID;
    }
    svcrt_layout_memcpy((uint8 *)out, (const uint8 *)r, SVCRT_CFG_RECORD_SIZE);
    return 0;
}

/* ============================================================
 * 启动期解析
 * ============================================================ */
void svcrt_layout_init(void)
{
    const svcrt_cfg_record_t *r;
    uint32 rc;

    /* 默认布局先落地：无论配置区发生什么都必须有一份可用布局 */
    svcrt_layout_use_default();
    svcrt_layout_reason_ram = SVCRT_CFG_OK;

    r = svcrt_layout_scan(0, 0);
    if(r != 0)
    {
        rc = svcrt_layout_validate(r);
        if(rc == SVCRT_CFG_OK)
        {
            svcrt_layout_adopt(r);
        }
        else
        {
            svcrt_layout_reason_ram = rc;
        }
    }
    svcrt_layout_publish();
    svcrt_layout_apply_runtime();
}

void svcrt_layout_apply_runtime(void)
{
    /* 只有这两个纽子是内核今就能真正生效的：
     * 日志级别有现成的运行期设置，重启上限被加载器读到共享分区表里。
     * 其余纽子在 validate 里就被拒绝了，不会走到这里。 */
    if(svcrt_layout_cfglvl_ram != 0u)
    {
        svcrt_log_set_level(svcrt_layout_cfglvl_ram);
    }
}

uint32 svcrt_layout_reclaim_mode(void)
{
    return svcrt_layout_reclaim_ram;
}

uint32 svcrt_layout_cfg_log_level(void)
{
    return svcrt_layout_cfglvl_ram;
}

uint32 svcrt_layout_cfg_restart_max(void)
{
    return svcrt_layout_restart_ram;
}

/* ============================================================
 * 只读查询
 * ============================================================ */
uint32 svcrt_layout_mode(void)
{
    return svcrt_layout_mode_ram;
}

uint32 svcrt_layout_source(void)
{
    return svcrt_layout_source_ram;
}

uint32 svcrt_layout_slot_count(void)
{
    return svcrt_layout_count_ram;
}

uint32 svcrt_layout_reason(void)
{
    return svcrt_layout_reason_ram;
}

const svcrt_cfg_slot_t *svcrt_layout_slots(void)
{
    return svcrt_layout_slots_ram;
}

const svcrt_cfg_slot_t *svcrt_layout_slot(uint32 index)
{
    if(index >= SVCRT_CFG_SLOT_MAX)
    {
        return 0;
    }
    return &svcrt_layout_slots_ram[index];
}

int32 svcrt_layout_slot_by_base(uint32 addr)
{
    uint32 i;

    for(i = 0u; i < SVCRT_CFG_SLOT_MAX; i++)
    {
        const svcrt_cfg_slot_t *s = &svcrt_layout_slots_ram[i];

        if(s->type == SVCRT_CFG_TYPE_UNUSED)
        {
            continue;
        }
        if((addr >= s->base) && (addr < (s->base + s->size)))
        {
            return (int32)i;
        }
    }
    return -1;
}

const char *svcrt_layout_reason_text(uint32 reason)
{
    switch(reason)
    {
    case SVCRT_CFG_OK:                 return "ok";
    case SVCRT_CFG_ERR_MAGIC:          return "bad magic";
    case SVCRT_CFG_ERR_VERSION:        return "bad version";
    case SVCRT_CFG_ERR_CRC:            return "bad crc";
    case SVCRT_CFG_ERR_HW:             return "hw compat mismatch";
    case SVCRT_CFG_ERR_MODE:           return "bad mode";
    case SVCRT_CFG_ERR_COUNT:          return "bad slot count";
    case SVCRT_CFG_ERR_SLOT_TYPE:      return "bad slot type";
    case SVCRT_CFG_ERR_SLOT_RANGE:     return "slot outside pool";
    case SVCRT_CFG_ERR_SLOT_SIZE:      return "slot too small";
    case SVCRT_CFG_ERR_SLOT_ALIGN:     return "slot not aligned";
    case SVCRT_CFG_ERR_SLOT_OVERLAP:   return "slots overlap";
    case SVCRT_CFG_ERR_SLOT_RAM:       return "bad ram window";
    case SVCRT_CFG_ERR_SLOT_COUNT_MAX: return "too many slots";
    case SVCRT_CFG_ERR_RECLAIM:        return "bad reclaim mode";
    case SVCRT_CFG_ERR_KNOB:           return "runtime knob out of range";
    case SVCRT_CFG_ERR_NOT_IMPL:       return "field not implemented by this kernel";
    case SVCRT_CFG_ERR_RESERVED:       return "reserved words not zero";
    case SVCRT_CFG_ERR_SLOTS_IN_MODE:  return "slot table does not match the mode";
    default:                           return "unknown";
    }
}

/* ============================================================
 * 写入 / 擦除
 * ============================================================ */
int32 svcrt_layout_write(const uint8 *record, uint32 len, uint32 *p_erased)
{
    uint32 i;
    uint32 idx = SVCRT_LAYOUT_ERASED;
    uint32 max_seq = 0u;
    uint32 erased = 0u;
    uint32 rc;

    if((record == 0) || (len != SVCRT_CFG_RECORD_SIZE))
    {
        return SVCRT_LAYOUT_ERR_ARG;
    }
    if(SVCRT_LAYOUT_RECORDS == 0u)
    {
        return SVCRT_LAYOUT_ERR_NOREGION;
    }

    svcrt_layout_memcpy((uint8 *)&svcrt_layout_wr_rec, record, SVCRT_CFG_RECORD_SIZE);

    if(svcrt_layout_wr_rec.magic != SVCRT_CFG_MAGIC)
    {
        return SVCRT_LAYOUT_ERR_INVALID;
    }
    if(svcrt_layout_wr_rec.crc32 != svcrt_layout_crc(&svcrt_layout_wr_rec))
    {
        return SVCRT_LAYOUT_ERR_CRC;
    }

    /* 找到第一个空位，同时记录现有最大序号（序号只增不减，便于选最新的） */
    for(i = 0u; i < SVCRT_LAYOUT_RECORDS; i++)
    {
        const svcrt_cfg_record_t *r =
            (const svcrt_cfg_record_t *)(uint32)(CONFIG_BASE + (i * SVCRT_CFG_RECORD_SIZE));
        uint32 magic = *(volatile const uint32 *)r;

        if(magic == SVCRT_LAYOUT_ERASED)
        {
            idx = i;
            break;
        }
        if((magic == SVCRT_CFG_MAGIC) && (svcrt_layout_validate(r) == SVCRT_CFG_OK))
        {
            if(r->seq > max_seq)
            {
                max_seq = r->seq;
            }
        }
    }

    if(idx == SVCRT_LAYOUT_ERASED)
    {
        /* 区满：整体擦一次再从头写。这是配置区唯一会丢内容的时间窗，
         * 掉在这里就回退编译期默认布局，设备不会变砖。 */
        if(svcrt_port_flash_erase((uint32)CONFIG_BASE, (uint32)CONFIG_SIZE) != 0)
        {
            return SVCRT_LAYOUT_ERR_ERASE;
        }
        idx = 0u;
        erased = 1u;
        max_seq = 0u;
    }

    /* 序号与硬件签名由内核盖章：以本内核为准，工具只提供策略与槽表 */
    svcrt_layout_wr_rec.seq          = max_seq + 1u;
    svcrt_layout_wr_rec.hw_compat_id = SVCRT_HW_COMPAT_ID;
    svcrt_layout_wr_rec.crc32        = svcrt_layout_crc(&svcrt_layout_wr_rec);

    rc = svcrt_layout_validate(&svcrt_layout_wr_rec);
    if(rc != SVCRT_CFG_OK)
    {
        return SVCRT_LAYOUT_ERR_INVALID;
    }

    if(svcrt_port_flash_write((uint32)(CONFIG_BASE + (idx * SVCRT_CFG_RECORD_SIZE)),
                              (const uint8 *)&svcrt_layout_wr_rec,
                              SVCRT_CFG_RECORD_SIZE) != 0)
    {
        return SVCRT_LAYOUT_ERR_WRITE;
    }

    /* 回读校验：写不进去（例如扇区没擦干净）必须当场暴露，不能等下次启动 */
    if(svcrt_layout_memcmp((const uint8 *)(uint32)(CONFIG_BASE + (idx * SVCRT_CFG_RECORD_SIZE)),
                           (const uint8 *)&svcrt_layout_wr_rec,
                           SVCRT_CFG_RECORD_SIZE) != 0)
    {
        return SVCRT_LAYOUT_ERR_VERIFY;
    }

    /* 立即生效：本机不用重启就能看到新布局与新模式 */
    svcrt_layout_adopt(&svcrt_layout_wr_rec);
    svcrt_layout_reason_ram = SVCRT_CFG_OK;
    svcrt_layout_publish();

    if(p_erased != 0)
    {
        *p_erased = erased;
    }
    return (int32)idx;
}

int32 svcrt_layout_erase(void)
{
    if(SVCRT_LAYOUT_RECORDS == 0u)
    {
        return SVCRT_LAYOUT_ERR_NOREGION;
    }
    if(svcrt_port_flash_erase((uint32)CONFIG_BASE, (uint32)CONFIG_SIZE) != 0)
    {
        return SVCRT_LAYOUT_ERR_ERASE;
    }
    svcrt_layout_use_default();
    svcrt_layout_reason_ram = SVCRT_CFG_OK;
    svcrt_layout_publish();
    return 0;
}
