/**
* @file svcrt_ptable.c
* @brief SVCrtOS 分区表运行期实现
* @details 唯一允许包含 config/svcrt_partition.h 的地方之一：把编译期布局
*          翻译为共享内存中的 svcrt_partition_table_t，并集中维护
*          「统一镜像池的字节粒度分配」与「镜像 RAM 池的 2 的幂分配」。
*
*          共享内存位于 SHARE_RAM_BASE（config/svcrt_partition.h 中定义），
*          该区域不在任何工程的 .sct 中分配，专供分区表与内核/App 数据交换使用。
*
*          空闲空间不维护位图或链表这类派生结构，而是每次从槽位记录现算：
*          记录数上限是编译期常量（SLOT_MAX <= 16），排序后扫缺口既便宜，
*          又不会出现「派生结构与记录不同步」这类只能靠运行期猜的错。
*
* @note 本文件不得包含任何芯片寄存器操作，只做数据搬运。
*/

#include "svcrt_ptable.h"
#include "svcrt_share.h"
#include "svcrt_app_image.h"
#include "svcrt_partition.h"    /* 内核专属：全工程唯一地址源头 */
#include "svcrt_layout_def.h"   /* 策略模式/来源常量（记录格式与工具共享） */
#include "svcrt_log_defs.h"      /* compile-time default log level */
#include "svcrt_spin.h"

/* Guards the (type, base, size, state, entry, task_id, ram) tuple of every slot.
 *
 * Two writers can meet here: the installer task (normal task context) and the
 * crash policy in svcrt_loader_on_fault(), which runs from exception context.
 * Without the lock a reader could observe state = LOADED together with a stale
 * entry or task id. The irqsave variant is used because the fault path runs
 * with exceptions masked already - disabling interrupts makes a nested take
 * safe (the lock stays re-entrant per CPU, see svcrt_spin_lock_irqsave). */
static svcrt_spinlock_t svcrt_ptable_lock = SVCRT_SPINLOCK_INIT;

/* 槽位数组的固定长度必须真实存在于结构体里，否则下面的循环会越界。
 * 两边都是编译期常量，配置错了直接编译不过。 */
typedef char svcrt_ptable_slot_array_check[
    ((SLOT_MAX <= SVCRT_SLOT_ARRAY_MAX) && (SLOT_MAX > 0)) ? 1 : -1];

/* 池可分配空间必须至少能放下一个分配单元，否则分配器恒失败。 */
typedef char svcrt_ptable_pool_space_check[
    ((IMAGE_POOL_USABLE_SIZE >= SVCRT_POOL_ALLOC_UNIT) &&
     ((IMAGE_POOL_USABLE_SIZE % SVCRT_POOL_ALLOC_UNIT) == 0)) ? 1 : -1];

/* RAM 池必须至少能放下一个最小块。 */
typedef char svcrt_ptable_ram_pool_check[
    ((SLOT_RAM_TOTAL >= SLOT_RAM_MIN_BLOCK) &&
     ((SLOT_RAM_TOTAL % SLOT_RAM_MIN_BLOCK) == 0)) ? 1 : -1];

/* 共享内存中的分区表实例（按绝对地址访问，不占用链接器分配的 RAM） */
static svcrt_partition_table_t *svcrt_ptable_ptr(void)
{
    return (svcrt_partition_table_t *)SHARE_RAM_BASE;
}

/* ---------------- 内部工具 ---------------- */

/* 向上对齐到 2 的幂边界（a 必须是 2 的幂） */
static uint32 svcrt_pt_align_up(uint32 v, uint32 a)
{
    return (v + a - 1u) & ~(a - 1u);
}

/* 向上取整到不小于 v 的 2 的幂 */
static uint32 svcrt_pt_pow2_ceil(uint32 v)
{
    uint32 p = 1u;

    while((p < v) && (p < 0x80000000u))
    {
        p <<= 1;
    }

    return p;
}

/* 一段已占用区间 */
typedef struct {
    uint32 base;
    uint32 end;
} svcrt_pt_range_t;

/* 收集所有占用区间（Flash 或 RAM），按基址升序插入排序。
 * 返回区间个数。选择哪一组字段由 ram_select 决定。 */
static uint32 svcrt_pt_collect(const svcrt_partition_table_t *pt, svcrt_pt_range_t *out,
                               uint32 max, uint32 ram_select)
{
    uint32 n = 0u;
    uint32 i;
    uint32 j;

    for(i = 0u; (i < pt->slot_max) && (i < SVCRT_SLOT_ARRAY_MAX) && (n < max); i++)
    {
        uint32 base;
        uint32 size;

        if(ram_select != 0u)
        {
            base = pt->slot_ram_base[i];
            size = pt->slot_ram_size[i];
        }
        else
        {
            if(pt->slot_type[i] == SVCRT_SLOT_FREE)
            {
                continue;
            }
            base = pt->slot_base[i];
            size = pt->slot_size[i];
        }

        if((base == 0u) || (size == 0u))
        {
            continue;
        }

        /* 插入排序：把新区间插到正确位置 */
        j = n;
        while((j > 0u) && (out[j - 1u].base > base))
        {
            out[j] = out[j - 1u];
            j--;
        }

        out[j].base = base;
        out[j].end  = base + size;
        n++;
    }

    return n;
}

/* 在 [pool_base, pool_end) 里找第一个能放下 size 字节的缺口。
 * 优先 2 的幂对齐落点；对齐落点放不下时才退化为缺口起点。
 * aligned_span 由调用方给出：2^n >= size（MPU 单区域覆盖所需）。 */
static int32 svcrt_pt_find_gap(uint32 pool_base, uint32 pool_end,
                               const svcrt_pt_range_t *r, uint32 n,
                               uint32 size, uint32 gran,
                               uint32 *p_base, uint32 *p_aligned)
{
    uint32 aligned_span = svcrt_pt_pow2_ceil(size);
    uint32 cur = pool_base;
    uint32 i;

    for(i = 0u; i <= n; i++)
    {
        uint32 gap_start = cur;
        uint32 gap_end;
        uint32 addr;

        if(i < n)
        {
            gap_end = (r[i].base < pool_end) ? r[i].base : pool_end;

            if(r[i].end > cur)
            {
                cur = (r[i].end > pool_end) ? pool_end : r[i].end;
            }
        }
        else
        {
            gap_end = pool_end;
        }

        if((gap_end <= gap_start) || ((gap_end - gap_start) < size))
        {
            if(i >= n)
            {
                break;
            }
            continue;
        }

        /* 1) 优先对齐落点：基址按自身跨度对齐，MPU 一个 region 就能精确覆盖 */
        addr = svcrt_pt_align_up(gap_start, aligned_span);

        if(((addr + size) <= gap_end) && ((addr % gran) == 0u))
        {
            *p_base    = addr;
            *p_aligned = 1u;
            return 0;
        }

        /* 2) 退化：紧邻落点，ROM 窗口交给包含窗口处理（见 svcrt_mpu_rom_window） */
        addr = gap_start;

        if(((addr + size) <= gap_end) && ((addr % gran) == 0u))
        {
            *p_base    = addr;
            *p_aligned = 0u;
            return 0;
        }

        if(i >= n)
        {
            break;
        }
    }

    return -1;
}

/* ---------------- 初始化 ---------------- */

void svcrt_ptable_init(void)
{
    svcrt_partition_table_t *pt = svcrt_ptable_ptr();

    pt->magic          = SVCRT_PARTITION_MAGIC;
    pt->version        = SVCRT_PARTITION_VERSION;
    pt->hw_compat_id   = SVCRT_HW_COMPAT_ID;

    pt->kernel_base    = KERNEL_BASE;
    pt->kernel_size    = KERNEL_SIZE;
    pt->pool_base      = IMAGE_POOL_BASE;
    pt->pool_size      = IMAGE_POOL_SIZE;
    pt->pool_usable_size = IMAGE_POOL_USABLE_SIZE;
    pt->pool_alloc_unit  = SVCRT_POOL_ALLOC_UNIT;
    pt->pool_sector    = IMAGE_POOL_SECTOR;
    pt->pool_units     = IMAGE_POOL_UNITS;
    pt->pool_reserve   = POOL_RESERVE_SECTORS;
    pt->slot_max       = SLOT_MAX;
    pt->layout_mode    = (uint32)SVCRT_LAYOUT_DEFAULT_MODE;
    pt->layout_source  = SVCRT_LAYOUT_SOURCE_DEFAULT;
    pt->config_base    = (uint32)CONFIG_BASE;
    pt->config_size    = (uint32)CONFIG_SIZE;
    pt->cfg_slot_count = 0u;
    pt->reclaim_mode   = (uint32)SVCRT_RECLAIM_MODE;
    pt->cfg_log_level  = (uint32)SVCRT_LOG_LEVEL;
    pt->cfg_restart_max = (uint32)APP_CRASH_RESTART_MAX;
    pt->cfg_boot_delay_ms = 0u;
    pt->cfg_raw_allow     = (uint32)APP_ALLOW_RAW_IMAGE;

    pt->share_ram_base = SHARE_RAM_BASE;
    pt->share_ram_size = SHARE_RAM_SIZE;
    pt->kernel_ram_base = KERNEL_RAM_BASE;
    pt->kernel_ram_size = KERNEL_RAM_SIZE;
    pt->image_ram_base  = SLOT_RAM_BASE;
    pt->image_ram_total = SLOT_RAM_TOTAL;
    pt->image_ram_min_block = SLOT_RAM_MIN_BLOCK;
    pt->image_ram_max_block = SLOT_RAM_MAX_BLOCK;

    svcrt_ptable_clear_slots();
}

/* Drop every slot record. This is the part of the reset that the pool scan
 * actually wants: shared RAM survives a reset, so stale records must go.
 * The layout the device is running on (install mode, slot table, reclaim
 * policy) is NOT seed data -- it was resolved from the CONFIG region and
 * published into this same structure, so it must be left alone. Calling
 * svcrt_ptable_init() here instead used to wipe it back to the compile-time
 * default, which silently turned a configured fixed-slot device back into
 * auto placement.
 */
void svcrt_ptable_clear_slots(void)
{
    svcrt_partition_table_t *pt = svcrt_ptable_ptr();
    uint32 i;

    for(i = 0; i < SVCRT_SLOT_ARRAY_MAX; i++)
    {
        pt->slot_type[i]      = SVCRT_SLOT_FREE;
        pt->slot_state[i]     = SVCRT_APP_SLOT_EMPTY;
        pt->slot_base[i]      = 0u;
        pt->slot_size[i]      = 0u;
        pt->slot_ram_base[i]  = 0u;
        pt->slot_ram_size[i]  = 0u;
        pt->slot_entry[i]     = 0u;
        pt->slot_task_id[i]   = 0u;
        pt->slot_crash_cnt[i] = 0u;
        pt->slot_autostart[i] = 0u;
    }

    pt->dev_slot_count = 0u;
}

svcrt_partition_table_t *svcrt_ptable_get(void)
{
    svcrt_partition_table_t *pt = svcrt_ptable_ptr();

    if(pt->magic != SVCRT_PARTITION_MAGIC)
    {
        /* 极早期被调用：就地补一次初始化，保证调用方总能拿到有效表 */
        svcrt_ptable_init();
    }

    return pt;
}

/* 单条记录是否占用池内区间：只要类型不是 FREE 就算占用。
 * INVALID 记录同样占用——它对应一段含有不可信内容的 Flash，
 * 不能被后来的安装当成空白擦掉。 */
static int32 svcrt_ptable_used(const svcrt_partition_table_t *pt, uint32 slot)
{
    return (pt->slot_type[slot] != SVCRT_SLOT_FREE) ? 1 : 0;
}

/* ---------------- Flash 池分配 ---------------- */

int32 svcrt_ptable_range_check(uint32 base, uint32 size)
{
    const svcrt_partition_table_t *pt = svcrt_ptable_get();
    uint32 limit = IMAGE_POOL_BASE + IMAGE_POOL_USABLE_SIZE;

    /* 尾部保留扇区只是给压实搬移留的余量；固定槽位模式从不压实，
     * 因而整个池都是合法落点（把最后一个扇区划给某个固定槽是常见做法）。 */
    if(pt->layout_mode == (uint32)SVCRT_LAYOUT_MODE_FIXED)
    {
        limit = IMAGE_POOL_BASE + IMAGE_POOL_SIZE;
    }

    if((size == 0u) || (base < IMAGE_POOL_BASE))
    {
        return -1;
    }

    if((base % SVCRT_POOL_ALLOC_UNIT) != 0u)
    {
        return -1;              /* 落点必须按分配粒度对齐（上电扫描依赖它） */
    }

    if((base >= limit) || (size > (limit - base)))
    {
        return -1;              /* 越界，或侵入了尾部压实余量 */
    }

    return 0;
}

int32 svcrt_ptable_find_free(uint32 size, uint32 *p_base, uint32 *p_aligned)
{
    svcrt_partition_table_t *pt = svcrt_ptable_get();
    svcrt_pt_range_t ranges[SVCRT_SLOT_ARRAY_MAX];
    uint32 irq_state;
    uint32 n;
    int32 ret;

    if((size == 0u) || (p_base == 0))
    {
        return -1;
    }

    svcrt_spin_lock_irqsave(&svcrt_ptable_lock, &irq_state);

    n = svcrt_pt_collect(pt, ranges, SVCRT_SLOT_ARRAY_MAX, 0u);

    ret = svcrt_pt_find_gap(IMAGE_POOL_BASE,
                            IMAGE_POOL_BASE + IMAGE_POOL_USABLE_SIZE,
                            ranges, n, size, SVCRT_POOL_ALLOC_UNIT,
                            p_base, p_aligned);

    svcrt_spin_unlock_irqrestore(&svcrt_ptable_lock, irq_state);

    return ret;
}

int32 svcrt_ptable_alloc(uint32 type, uint32 base, uint32 size)
{
    svcrt_partition_table_t *pt = svcrt_ptable_get();
    uint32 irq_state;
    uint32 end = base + size;
    uint32 i;
    int32 free_slot = -1;

    if((type != SVCRT_SLOT_APP) && (type != SVCRT_SLOT_DRIVER))
    {
        return -1;
    }

    if(svcrt_ptable_range_check(base, size) != 0)
    {
        return -1;
    }

    svcrt_spin_lock_irqsave(&svcrt_ptable_lock, &irq_state);

    /* 重叠校验：与所有在用记录两两比较。同一区间的重复安装（覆盖安装）
     * 是允许的，那正是"先用旧镜像启动 → 再装新镜像"的正常路径。 */
    for(i = 0u; (i < pt->slot_max) && (i < SVCRT_SLOT_ARRAY_MAX); i++)
    {
        uint32 other_base;
        uint32 other_end;

        if(svcrt_ptable_used(pt, i) == 0)
        {
            continue;
        }

        other_base = pt->slot_base[i];
        other_end  = other_base + pt->slot_size[i];

        if((base == other_base) && (size == pt->slot_size[i]))
        {
            /* 同一区间重复登记：复用这条记录 */
            free_slot = (int32)i;
            break;
        }

        if((base < other_end) && (other_base < end))
        {
            svcrt_spin_unlock_irqrestore(&svcrt_ptable_lock, irq_state);
            return -1;          /* 与已登记的镜像区间重叠 */
        }
    }

    if(free_slot < 0)
    {
        for(i = 0u; (i < pt->slot_max) && (i < SVCRT_SLOT_ARRAY_MAX); i++)
        {
            if(svcrt_ptable_used(pt, i) == 0)
            {
                free_slot = (int32)i;
                break;
            }
        }
    }

    if(free_slot < 0)
    {
        svcrt_spin_unlock_irqrestore(&svcrt_ptable_lock, irq_state);
        return -1;              /* 没有空闲记录 */
    }

    pt->slot_type[(uint32)free_slot]      = type;
    pt->slot_base[(uint32)free_slot]      = base;
    pt->slot_size[(uint32)free_slot]      = size;
    pt->slot_state[(uint32)free_slot]     = SVCRT_APP_SLOT_INSTALLING;
    pt->slot_ram_base[(uint32)free_slot]  = 0u;
    pt->slot_ram_size[(uint32)free_slot]  = 0u;
    pt->slot_entry[(uint32)free_slot]     = 0u;
    pt->slot_task_id[(uint32)free_slot]   = 0u;
    pt->slot_crash_cnt[(uint32)free_slot] = 0u;
    pt->slot_autostart[(uint32)free_slot] = 0u;

    svcrt_spin_unlock_irqrestore(&svcrt_ptable_lock, irq_state);

    return free_slot;
}

int32 svcrt_ptable_alloc_raw(uint32 type, uint32 base, uint32 size, uint32 entry)
{
    svcrt_partition_table_t *pt = svcrt_ptable_get();
    uint32 irq_state;
    uint32 i;
    int32 free_slot = -1;

    if((type != SVCRT_SLOT_APP) && (type != SVCRT_SLOT_DRIVER))
    {
        return -1;
    }

    if((size == 0u) || (base < IMAGE_POOL_BASE) ||
       ((base + size) > (IMAGE_POOL_BASE + IMAGE_POOL_SIZE)))
    {
        return -1;
    }

    svcrt_spin_lock_irqsave(&svcrt_ptable_lock, &irq_state);

    for(i = 0u; (i < pt->slot_max) && (i < SVCRT_SLOT_ARRAY_MAX); i++)
    {
        if((svcrt_ptable_used(pt, i) != 0) &&
           (pt->slot_base[i] == base) && (pt->slot_size[i] == size))
        {
            free_slot = (int32)i;
            break;
        }
    }

    if(free_slot < 0)
    {
        for(i = 0u; (i < pt->slot_max) && (i < SVCRT_SLOT_ARRAY_MAX); i++)
        {
            if(svcrt_ptable_used(pt, i) == 0)
            {
                free_slot = (int32)i;
                break;
            }
        }
    }

    if(free_slot < 0)
    {
        svcrt_spin_unlock_irqrestore(&svcrt_ptable_lock, irq_state);
        return -1;
    }

    pt->slot_type[(uint32)free_slot]      = type;
    pt->slot_base[(uint32)free_slot]      = base;
    pt->slot_size[(uint32)free_slot]      = size;
    pt->slot_state[(uint32)free_slot]     = SVCRT_APP_SLOT_RAW;
    pt->slot_ram_base[(uint32)free_slot]  = 0u;
    pt->slot_ram_size[(uint32)free_slot]  = 0u;
    pt->slot_entry[(uint32)free_slot]     = entry;
    pt->slot_task_id[(uint32)free_slot]   = 0u;
    pt->slot_crash_cnt[(uint32)free_slot] = 0u;
    pt->slot_autostart[(uint32)free_slot] = 0u;

    svcrt_spin_unlock_irqrestore(&svcrt_ptable_lock, irq_state);

    return free_slot;
}

void svcrt_ptable_free(uint32 slot)
{
    svcrt_partition_table_t *pt = svcrt_ptable_get();
    uint32 irq_state;

    if((slot >= pt->slot_max) || (slot >= SVCRT_SLOT_ARRAY_MAX))
    {
        return;
    }

    svcrt_spin_lock_irqsave(&svcrt_ptable_lock, &irq_state);

    pt->slot_type[slot]      = SVCRT_SLOT_FREE;
    pt->slot_state[slot]     = SVCRT_APP_SLOT_EMPTY;
    pt->slot_base[slot]      = 0u;
    pt->slot_size[slot]      = 0u;
    pt->slot_ram_base[slot]  = 0u;
    pt->slot_ram_size[slot]  = 0u;
    pt->slot_entry[slot]     = 0u;
    pt->slot_task_id[slot]   = 0u;
    pt->slot_crash_cnt[slot] = 0u;
    pt->slot_autostart[slot] = 0u;

    svcrt_spin_unlock_irqrestore(&svcrt_ptable_lock, irq_state);
}

int32 svcrt_ptable_move(uint32 slot, uint32 new_base)
{
    svcrt_partition_table_t *pt = svcrt_ptable_get();
    uint32 irq_state;
    uint32 i;
    uint32 end;

    if((slot >= pt->slot_max) || (slot >= SVCRT_SLOT_ARRAY_MAX))
    {
        return -1;
    }

    if(svcrt_ptable_range_check(new_base, pt->slot_size[slot]) != 0)
    {
        return -1;
    }

    end = new_base + pt->slot_size[slot];

    svcrt_spin_lock_irqsave(&svcrt_ptable_lock, &irq_state);

    /* 目标区间不能与任何其它记录重叠 */
    for(i = 0u; (i < pt->slot_max) && (i < SVCRT_SLOT_ARRAY_MAX); i++)
    {
        uint32 other_base;
        uint32 other_end;

        if((i == slot) || (svcrt_ptable_used(pt, i) == 0))
        {
            continue;
        }

        other_base = pt->slot_base[i];
        other_end  = other_base + pt->slot_size[i];

        if((new_base < other_end) && (other_base < end))
        {
            svcrt_spin_unlock_irqrestore(&svcrt_ptable_lock, irq_state);
            return -1;
        }
    }

    pt->slot_base[slot] = new_base;

    svcrt_spin_unlock_irqrestore(&svcrt_ptable_lock, irq_state);

    return 0;
}

/* ---------------- 查询 ---------------- */

int32 svcrt_ptable_find_addr(uint32 addr, uint32 *p_base, uint32 *p_size)
{
    svcrt_partition_table_t *pt = svcrt_ptable_get();
    uint32 irq_state;
    uint32 i;
    int32 hit = -1;

    svcrt_spin_lock_irqsave(&svcrt_ptable_lock, &irq_state);

    for(i = 0u; (i < pt->slot_max) && (i < SVCRT_SLOT_ARRAY_MAX); i++)
    {
        uint32 base;
        uint32 end;

        if((svcrt_ptable_used(pt, i) == 0) || (pt->slot_state[i] == SVCRT_APP_SLOT_EMPTY))
        {
            continue;
        }

        base = pt->slot_base[i];
        end  = base + pt->slot_size[i];

        if((addr >= base) && (addr < end))
        {
            if(p_base != 0)
            {
                *p_base = base;
            }
            if(p_size != 0)
            {
                *p_size = pt->slot_size[i];
            }
            hit = (int32)i;
            break;
        }
    }

    svcrt_spin_unlock_irqrestore(&svcrt_ptable_lock, irq_state);

    return hit;
}

int32 svcrt_ptable_find_task(uint32 task_id)
{
    svcrt_partition_table_t *pt = svcrt_ptable_get();
    uint32 irq_state;
    uint32 i;
    int32 hit = -1;

    if(task_id == 0u)
    {
        return -1;
    }

    svcrt_spin_lock_irqsave(&svcrt_ptable_lock, &irq_state);

    for(i = 0u; (i < pt->slot_max) && (i < SVCRT_SLOT_ARRAY_MAX); i++)
    {
        if(pt->slot_task_id[i] == task_id)
        {
            hit = (int32)i;
            break;
        }
    }

    svcrt_spin_unlock_irqrestore(&svcrt_ptable_lock, irq_state);

    return hit;
}

int32 svcrt_ptable_slot_info(uint32 slot, uint32 *p_type, uint32 *p_base, uint32 *p_size)
{
    svcrt_partition_table_t *pt = svcrt_ptable_get();
    uint32 irq_state;

    if((slot >= pt->slot_max) || (slot >= SVCRT_SLOT_ARRAY_MAX))
    {
        return -1;
    }

    svcrt_spin_lock_irqsave(&svcrt_ptable_lock, &irq_state);

    if(p_type != 0)
    {
        *p_type = pt->slot_type[slot];
    }
    if(p_base != 0)
    {
        *p_base = pt->slot_base[slot];
    }
    if(p_size != 0)
    {
        *p_size = pt->slot_size[slot];
    }

    svcrt_spin_unlock_irqrestore(&svcrt_ptable_lock, irq_state);

    return 0;
}

int32 svcrt_ptable_read_header(uint32 slot, svcrt_app_header_t *out)
{
    svcrt_partition_table_t *pt = svcrt_ptable_get();
    const svcrt_app_header_t *p_hdr;

    if((slot >= pt->slot_max) || (slot >= SVCRT_SLOT_ARRAY_MAX))
    {
        return -1;
    }

    if((pt->slot_type[slot] == SVCRT_SLOT_FREE) ||
       (pt->slot_state[slot] == SVCRT_APP_SLOT_RAW))
    {
        return -1;              /* 裸镜像没有镜像头 */
    }

    p_hdr = (const svcrt_app_header_t *)pt->slot_base[slot];

    if(p_hdr->magic != SVCRT_APP_MAGIC)
    {
        return -2;
    }

    if(out != 0)
    {
        *out = *p_hdr;
    }

    return 0;
}

int32 svcrt_ptable_set_slot(uint32 slot, uint32 state, uint32 entry, uint32 task_id)
{
    svcrt_partition_table_t *pt = svcrt_ptable_get();
    uint32 irq_state;

    if((slot >= pt->slot_max) || (slot >= SVCRT_SLOT_ARRAY_MAX))
    {
        return -1;
    }

    svcrt_spin_lock_irqsave(&svcrt_ptable_lock, &irq_state);

    pt->slot_state[slot]   = state;
    pt->slot_entry[slot]   = entry;
    pt->slot_task_id[slot] = task_id;

    svcrt_spin_unlock_irqrestore(&svcrt_ptable_lock, irq_state);

    return 0;
}

int32 svcrt_ptable_get_slot(uint32 slot, uint32 *p_state, uint32 *p_entry, uint32 *p_task_id)
{
    svcrt_partition_table_t *pt = svcrt_ptable_get();
    uint32 irq_state;

    if((slot >= pt->slot_max) || (slot >= SVCRT_SLOT_ARRAY_MAX))
    {
        return -1;
    }

    svcrt_spin_lock_irqsave(&svcrt_ptable_lock, &irq_state);

    if(p_state != 0)
    {
        *p_state = pt->slot_state[slot];
    }
    if(p_entry != 0)
    {
        *p_entry = pt->slot_entry[slot];
    }
    if(p_task_id != 0)
    {
        *p_task_id = pt->slot_task_id[slot];
    }

    svcrt_spin_unlock_irqrestore(&svcrt_ptable_lock, irq_state);

    return 0;
}

/* ---------------- 镜像 RAM 池（2 的幂分配） ---------------- */

int32 svcrt_ptable_ram_alloc(uint32 bytes, uint32 *p_base, uint32 *p_size)
{
    svcrt_partition_table_t *pt = svcrt_ptable_get();
    svcrt_pt_range_t ranges[SVCRT_SLOT_ARRAY_MAX];
    uint32 irq_state;
    uint32 want;
    uint32 n;
    int32 ret;

    if((p_base == 0) || (p_size == 0) || (bytes == 0u))
    {
        return -1;
    }

    want = svcrt_pt_pow2_ceil(bytes);

    if(want < SLOT_RAM_MIN_BLOCK)
    {
        want = SLOT_RAM_MIN_BLOCK;
    }

    if(want > SLOT_RAM_MAX_BLOCK)
    {
        return -1;              /* 镜像声明的 RAM 需求超过单块上限 */
    }

    svcrt_spin_lock_irqsave(&svcrt_ptable_lock, &irq_state);

    n = svcrt_pt_collect(pt, ranges, SVCRT_SLOT_ARRAY_MAX, 1u);

    /* RAM 也必须按块大小对齐：MPU 一个 region 要能覆盖整块，
     * 否则镜像的 .data/.bss 会露在 region 之外，一访问就 MemManage。 */
    ret = svcrt_pt_find_gap(SLOT_RAM_BASE, SLOT_RAM_BASE + SLOT_RAM_TOTAL,
                            ranges, n, want, want, p_base, p_size);

    if(ret == 0)
    {
        *p_size = want;
    }

    svcrt_spin_unlock_irqrestore(&svcrt_ptable_lock, irq_state);

    return ret;
}

/* 按指定基址预留一块 RAM：只校验「能不能占」，不把结果记到任何槽位上
 * （登记由调用方在预留成功后调 svcrt_ptable_ram_bind() 做）。
 * 用途见头文件：上电扫描要把镜像安装时固化在头里的那个基址原样要回来。 */
int32 svcrt_ptable_ram_reserve(uint32 base, uint32 size)
{
    svcrt_partition_table_t *pt = svcrt_ptable_get();
    svcrt_pt_range_t ranges[SVCRT_SLOT_ARRAY_MAX];
    uint32 irq_state;
    uint32 n;
    uint32 i;
    uint32 end;
    int32 ret = 0;

    /* 必须是 2 的幂：MPU 一个 region 的覆盖范围只能是 2 的幂，块大小不是
     * 2 的幂时镜像的 .data/.bss 会露在 region 外面，一访问就 MemManage。 */
    if((size == 0u) || ((size & (size - 1u)) != 0u))
    {
        return -1;
    }

    end = base + size;

    if((base < SLOT_RAM_BASE) || (end < base) ||
       (end > (SLOT_RAM_BASE + SLOT_RAM_TOTAL)) ||
       ((base % size) != 0u))
    {
        return -1;              /* 越界（含回绕）或没按块大小对齐 */
    }

    svcrt_spin_lock_irqsave(&svcrt_ptable_lock, &irq_state);

    n = svcrt_pt_collect(pt, ranges, SVCRT_SLOT_ARRAY_MAX, 1u);

    for(i = 0u; i < n; i++)
    {
        if((base < ranges[i].end) && (ranges[i].base < end))
        {
            /* 已有人占着这块。这里绝不去找「另一个空闲块」替代：那等于默认
             * 镜像里的绝对地址可以随便挪，而它们已经写死在 Flash 里了。 */
            ret = -1;
            break;
        }
    }

    svcrt_spin_unlock_irqrestore(&svcrt_ptable_lock, irq_state);

    return ret;
}

int32 svcrt_ptable_ram_bind(uint32 slot, uint32 base, uint32 size)
{
    svcrt_partition_table_t *pt = svcrt_ptable_get();
    uint32 irq_state;

    if((slot >= pt->slot_max) || (slot >= SVCRT_SLOT_ARRAY_MAX))
    {
        return -1;
    }

    svcrt_spin_lock_irqsave(&svcrt_ptable_lock, &irq_state);

    pt->slot_ram_base[slot] = base;
    pt->slot_ram_size[slot] = size;

    svcrt_spin_unlock_irqrestore(&svcrt_ptable_lock, irq_state);

    return 0;
}

int32 svcrt_ptable_ram_info(uint32 slot, uint32 *p_base, uint32 *p_size)
{
    svcrt_partition_table_t *pt = svcrt_ptable_get();
    uint32 irq_state;

    if((slot >= pt->slot_max) || (slot >= SVCRT_SLOT_ARRAY_MAX))
    {
        return -1;
    }

    svcrt_spin_lock_irqsave(&svcrt_ptable_lock, &irq_state);

    if(p_base != 0)
    {
        *p_base = pt->slot_ram_base[slot];
    }
    if(p_size != 0)
    {
        *p_size = pt->slot_ram_size[slot];
    }

    svcrt_spin_unlock_irqrestore(&svcrt_ptable_lock, irq_state);

    return 0;
}

int32 svcrt_ptable_stats(uint32 *p_used, uint32 *p_total, uint32 *p_frag)
{
    svcrt_partition_table_t *pt = svcrt_ptable_get();
    svcrt_pt_range_t ranges[SVCRT_SLOT_ARRAY_MAX];
    uint32 irq_state;
    uint32 n;
    uint32 used = 0u;
    uint32 frag = 0u;
    uint32 cur;
    uint32 i;

    svcrt_spin_lock_irqsave(&svcrt_ptable_lock, &irq_state);

    n = svcrt_pt_collect(pt, ranges, SVCRT_SLOT_ARRAY_MAX, 0u);

    for(i = 0u; i < n; i++)
    {
        used += ranges[i].end - ranges[i].base;
    }

    /* 统计空闲区间个数：>1 说明池内有碎片（压实后应当为 1） */
    cur = IMAGE_POOL_BASE;

    for(i = 0u; i < n; i++)
    {
        if(ranges[i].base > cur)
        {
            frag++;
        }
        if(ranges[i].end > cur)
        {
            cur = ranges[i].end;
        }
    }

    if(cur < (IMAGE_POOL_BASE + IMAGE_POOL_USABLE_SIZE))
    {
        frag++;
    }

    if(p_used != 0)
    {
        *p_used = used;
    }
    if(p_total != 0)
    {
        *p_total = IMAGE_POOL_USABLE_SIZE;
    }
    if(p_frag != 0)
    {
        *p_frag = frag;
    }

    svcrt_spin_unlock_irqrestore(&svcrt_ptable_lock, irq_state);

    return 0;
}
