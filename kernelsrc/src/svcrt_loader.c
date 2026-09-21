/**
* @file svcrt_loader.c
* @brief SVCrtOS 镜像加载器实现（动态装载：安装 / 扫描 / 重定位 / 搬移 / 回收 / 启停）
* @details 加载流程：
*          1) 校验镜像头（魔数 / 兼容签名 / 长度 / 入口 / 重定位表）；
*          2) 在池里找第一段足够长的**已擦除**空间（首适配），
*             因此镜像按实际长度紧邻排列，密度 = 实际占用之和；
*          3) 分配镜像 RAM 块（2 的幂、按块对齐），登记槽位记录；
*          4) 写入镜像头（state = UNCOMMITTED）；
*          5) 流式接收重定位表并落盘；
*          6) 流式接收负载，逐块按 ROM / RAM 增量就地打补丁后落盘；
*          7) CRC32 复核；成功后唯一的一个字写入：state = VALID（提交）；
*          8) 置 LOADED，返回槽位号。
*          启动时把入口地址注册为内核任务（优先级 / 栈 / 周期按镜像类型取分区配置）。
*
*          为什么分配不维护任何元数据：Flash 里「连续 0xFF 段」就是可用空间，
*          而活镜像必然含非 0xFF 字节（镜像头魔数），所以一次物理扫描同时
*          完成了「找空位」与「避开已装镜像」。重启后不需要恢复分配器状态，
*          也不会出现「元数据与实际内容不一致」这种只能靠猜的故障。
*
*          回收同样受物理规律约束：擦除粒度是整扇区，因此一个扇区里只要还有
*          活镜像就不能擦。svcrt_loader_reclaim() 先把该扇区的活镜像搬到池内
*          更低处的已擦除空位（应用重定位，走 UNCOMMITTED -> VALID 事务），
*          再擦扇区。搬不动的（正在运行 / 无空位）跳过，不影响其它扇区。
*
* @note 本模块只能由内核特权态调用（经 SVC 0x18 分发）。
*/

#include "svcrt_loader.h"
#include "svcrt_ptable.h"
#include "svcrt_crash.h"
#include "svcrt_share.h"
#include "svcrt_layout_def.h"   /* strategy constants (reclaim mode) */
#include "svcrt_layout.h"       /* effective mode + fixed slot table */
#include "svcrt_app_image.h"
#include "svcrt_hal.h"
#include "svcrt_config.h"
#include "svcrt_dev.h"
#include "svcrt_cfg.h"
#include "svcrt_task.h"
#include "svcrt_mpu.h"
#include "svcrt_fault.h"
#include "svcrt_partition.h"    /* 内核专属：读取池布局 / 任务运行参数 */
#include "svcrt_log.h"

/* 设备流式加载的分块缓冲（避免整镜像驻留 RAM） */
#define SVCRT_LOADER_CHUNK_SIZE   (512u)

/* 分块缓冲必须与重定位表项的 4 字节对齐兼容：块大小是 4 的倍数，
 * 而重定位表项偏移也按 4 字节对齐，因此一个表项永远不会跨块边界。 */
typedef char svcrt_loader_chunk_align_check[
    ((SVCRT_LOADER_CHUNK_SIZE % SVCRT_APP_RELOC_SIZE) == 0) ? 1 : -1];

/* 分区容量必须装得下任务栈：编译期就把配置错误暴露出来 */
typedef char svcrt_loader_app_stack_check[
    (APP_TASK_STACK_SIZE <= SLOT_RAM_MAX_BLOCK) ? 1 : -1];
typedef char svcrt_loader_driver_stack_check[
    (DRIVER_TASK_STACK_SIZE <= SLOT_RAM_MAX_BLOCK) ? 1 : -1];

/* 镜像头必须能整块写进一个分配单元，否则「先写头再写负载」会越过分配边界 */
typedef char svcrt_loader_header_fit_check[
    (SVCRT_APP_HEADER_SIZE <= SVCRT_POOL_ALLOC_UNIT) ? 1 : -1];

/* 设备流式加载的分块缓冲 */
static uint8  svcrt_loader_chunk[SVCRT_LOADER_CHUNK_SIZE];

/* 池扫描结果缓存：scan() 与 scan_driver() 共用同一次遍历 */
static uint32 svcrt_loader_scan_done    = 0u;
static uint32 svcrt_loader_scan_apps    = 0u;
static uint32 svcrt_loader_scan_drivers = 0u;

/* ============================================================
 * CRC32（IEEE 802.3 反射多项式 0xEDB88320）
 * @note 语义与 zlib.crc32() 逐位一致（输入/输出均取反），因此可以把上一段的
 *       返回值直接传回来继续累积：
 *           crc = svcrt_crc32(seg1, len1, 0);
 *           crc = svcrt_crc32(seg2, len2, crc);
 *       等价于 zlib.crc32(两段拼起来)。打包工具 tools/pack_app.py 用的就是
 *       zlib，两边必须是同一个函数。
 *
 *       旧实现只做了反射循环、没有前后取反，算出来是另一个值：同一份
 *       APP_DEMO 镜像 zlib 给 0x474E26D7、旧内核给 0xAE1EDD7F，
 *       安装必定报 SVCRT_LOADER_ERR_CRC。
 * ============================================================ */
uint32 svcrt_crc32(const void *data, uint32 len, uint32 crc)
{
    const uint8 *p = (const uint8 *)data;
    uint32 c;
    uint32 i;
    uint32 j;

    /* 前置取反（zlib.crc32 的 init ^= 0xFFFFFFFF）：空输入返回 0。 */
    c = crc ^ 0xFFFFFFFFu;

    for(i = 0u; i < len; i++)
    {
        c ^= (uint32)p[i];

        for(j = 0u; j < 8u; j++)
        {
            if((c & 1u) != 0u)
            {
                c = (c >> 1) ^ 0xEDB88320u;
            }
            else
            {
                c >>= 1;
            }
        }
    }

    /* 后置取反（zlib.crc32 的 xorout）：保证可以继续累积。 */
    return c ^ 0xFFFFFFFFu;
}

/* ============================================================
 * 小工具
 * ============================================================ */

static uint32 svcrt_loader_align_up(uint32 v, uint32 a)
{
    return (v + a - 1u) & ~(a - 1u);
}

/* 向上取整到不小于 v 的 2 的幂 */
static uint32 svcrt_loader_pow2_ceil(uint32 v)
{
    uint32 p = 1u;

    while((p < v) && (p < 0x80000000u))
    {
        p <<= 1;
    }

    return p;
}

/* 入口地址：入口相对「负载起始」的偏移，负载起点 = 镜像起始 + payload_offset。
 * 裸镜像调试路径不经过本函数：它没有镜像头，入口直接取单元基址
 * （见 svcrt_loader_identify 里的 APP_ALLOW_RAW_IMAGE 分支）。 */
static uint32 svcrt_loader_entry_addr(uint32 image_base, uint32 payload_offset, uint32 entry_offset)
{
    uint32 entry = image_base + payload_offset + entry_offset;

    #if (SVCRT_ARCH_IS_ARM == 1)
    entry |= 1u;                /* Cortex-M：Thumb 指令集标志位 */
    #endif

    return entry;
}

/* 整段是否处于擦除态（全 0xFF） */
static uint32 svcrt_loader_span_erased(uint32 base, uint32 size)
{
    const uint32 *p = (const uint32 *)base;
    uint32 words = size / 4u;
    uint32 i;

    for(i = 0u; i < words; i++)
    {
        if(p[i] != 0xFFFFFFFFu)
        {
            return 0u;
        }
    }

    return 1u;
}

/* 硬停一个正在运行的外部任务（任意槽位）。
 * 用于覆盖安装前把旧任务踢出调度，避免“擦除正在执行的代码区”导致取指崩溃。 */
static void svcrt_loader_halt_task(uint32 task_id)
{
    if(task_id == 0u || task_id > (uint32)svcrt_task_count)
    {
        return;
    }

    /* 收尸：把该任务从同步对象/消息队列的等待队列中摘除，并释放它持有的锁。
     * 否则它被踢出调度后，残留的锁与等待登记会牵连其它任务。 */
    svcrt_task_release_resources((int32)task_id);

    SVCRT_DISABLE_IRQ();
    svcrt_task_table[task_id - 1u].recover_pending = 0u;
    svcrt_ready_del((int32)task_id - 1);
    svcrt_delay_disarm((int32)task_id - 1);
    svcrt_task_table[task_id - 1u].status          = SVCRT_TASK_INVALID;
    SVCRT_ENABLE_IRQ();
}

/* Hard stop every task that lives in the same RAM window, not just the one
 * passed in: an App may have created threads of its own, and a stopped or
 * uninstalled image must not leave them in the scheduler. Left behind, they
 * keep occupying task slots and holding sync / mq objects that the next
 * start has to create again, so the second start begins to fail.
 * The RAM window is the identity here: the loader hands the main task the
 * image's RAM block, and every thread an App creates passes a kernel check
 * proving its stack lies inside that same block. Kernel tasks (shell, timer,
 * idle ...) live in the kernel RAM region and can never match. */
static void svcrt_loader_halt_image(uint32 task_id)
{
    uint32 ram_base;
    uint32 ram_end;
    int32  i;

    if((task_id == 0u) || (task_id > (uint32)svcrt_task_count))
    {
        return;
    }

    ram_base = svcrt_task_table[task_id - 1u].ram_start;
    ram_end  = ram_base + svcrt_task_table[task_id - 1u].ram_size;

    for(i = 0; i < (int32)svcrt_task_count; i++)
    {
        uint32 t_base;
        uint32 t_end;

        if(svcrt_task_table[i].status == SVCRT_TASK_INVALID)
        {
            continue;
        }

        t_base = svcrt_task_table[i].ram_start;
        t_end  = t_base + svcrt_task_table[i].ram_size;

        if((t_base < ram_base) || (t_end > ram_end))
        {
            continue;
        }

        svcrt_loader_halt_task((uint32)(i + 1));
    }
}

/* 校验镜像头：字段级约束 + 重定位表表体。返回 0 或 SVCRT_LOADER_ERR_x。
 * @param p_hdr 镜像头
 * @param p_rel 重定位表表体地址。传 0 表示「此刻表体还不在可寻址的地方」
 *              （流式安装只先拿到 256 字节的头，表体还在串口上），
 *              此时只校字段，调用方必须在表体落盘后补一次带地址的调用。 */
static int32 svcrt_loader_check_header(const svcrt_app_header_t *p_hdr,
                                       const uint32 *p_rel)
{
    if(p_hdr->magic != SVCRT_APP_MAGIC)
    {
        return SVCRT_LOADER_ERR_MAGIC;
    }

    if(p_hdr->hw_compat_id != SVCRT_HW_COMPAT_ID)
    {
        return SVCRT_LOADER_ERR_COMPAT;
    }

    if((p_hdr->type != SVCRT_APP_TYPE_APP) && (p_hdr->type != SVCRT_APP_TYPE_DRIVER))
    {
        return SVCRT_LOADER_ERR_PARAM;
    }

    if(p_hdr->image_size == 0u)
    {
        return SVCRT_LOADER_ERR_SIZE;
    }

    /* image_size 不能让总长回绕：一旦回绕（如 image_size = 0xFFFFFFFF），
     * 后面的「总长 > 可用空间」检查会被绕过，随后的写入与 CRC 全部按错误长度进行。 */
    if(p_hdr->image_size > (0xFFFFFFFFu - SVCRT_APP_HEADER_SIZE))
    {
        return SVCRT_LOADER_ERR_SIZE;
    }

    /* entry_offset 是入口相对「负载起始」的偏移，必须落在负载范围内，
     * 否则镜像能把入口指到镜像内任意地址（含镜像头或重定位表）。 */
    if(p_hdr->entry_offset >= p_hdr->image_size)
    {
        return SVCRT_LOADER_ERR_ENTRY;
    }

    /* 重定位表：固定落在镜像头之后，条目数有上限，编码版本必须是当前版本。
     * 表长算不出来就会让「总长」算错，进而写坏邻居镜像，所以这里必须挡住。 */
    if(p_hdr->reloc_offset != SVCRT_APP_RELOC_OFFSET)
    {
        return SVCRT_LOADER_ERR_RELOC;
    }

    if(p_hdr->reloc_count > SVCRT_APP_RELOC_MAX)
    {
        return SVCRT_LOADER_ERR_RELOC;
    }

    if(p_hdr->reloc_kind != SVCRT_APP_RELOC_KIND_CUR)
    {
        return SVCRT_LOADER_ERR_RELOC;
    }

    if(p_hdr->payload_offset != SVCRT_APP_PAYLOAD_OFFSET(p_hdr))
    {
        return SVCRT_LOADER_ERR_RELOC;
    }

    /* 重定位表项的偏移必须落在负载内、4 字节对齐、且按升序排列：
     * 流式打补丁靠一个前进指针走完整表，乱序或越界都会静默漏改。
     *
     * 只能用调用方交给的 p_rel，不能用 p_hdr + SVCRT_APP_RELOC_OFFSET 现推：
     * 那个表达式只在「头与表在内存里连续」的路径（RAM 缓冲、Flash 常驻镜像）
     * 成立；流式安装与池内搬移传进来的只是一份 256 字节的头拷贝，
     * 推出来的地址落在栈/全局变量的邻居上，会把随机的栈字节当成表项。 */
    if(p_rel != 0)
    {
        uint32 prev = 0u;
        uint32 i;

        for(i = 0u; i < p_hdr->reloc_count; i++)
        {
            uint32 entry = p_rel[i];
            uint32 kind  = SVCRT_APP_RELOC_KIND(entry);
            uint32 off   = SVCRT_APP_RELOC_OFF(entry);
            uint32 need;

            /* 数据类表项是整字、4 字节对齐；指令类表项指向一条 Thumb 指令，
             * 半字对齐即可，但要覆盖 MOVW+MOVT 两条指令共 8 字节 */
            if((kind == SVCRT_APP_RELOC_KIND_ROM_MOVW) ||
               (kind == SVCRT_APP_RELOC_KIND_RAM_MOVW))
            {
                need = SVCRT_APP_RELOC_MOV_SIZE;

                if((off % 2u) != 0u)
                {
                    return SVCRT_LOADER_ERR_RELOC;
                }
            }
            else
            {
                need = SVCRT_APP_RELOC_SIZE;

                if((off % SVCRT_APP_RELOC_SIZE) != 0u)
                {
                    return SVCRT_LOADER_ERR_RELOC;
                }
            }

            if((off < p_hdr->payload_offset) ||
               (off > (SVCRT_APP_TOTAL_LEN(p_hdr) - need)))
            {
                return SVCRT_LOADER_ERR_RELOC;
            }

            if((i > 0u) && (off < prev))
            {
                return SVCRT_LOADER_ERR_RELOC;
            }

            prev = off;
        }
    }

    /* RAM 需求必须能在单块上限内满足，否则装上也没法启动 */
    if((p_hdr->ram_size == 0u) || (p_hdr->ram_size > SLOT_RAM_MAX_BLOCK))
    {
        return SVCRT_LOADER_ERR_SIZE;
    }

    return 0;
}

/* 计算镜像 CRC 时代入 crc32 / state 两个字段的占位：打包工具算这两个字段时
 * 它们都是 0，内核必须**真的喂四个 0 字节**，而不是把这两段从流里跳过去——
 * 对 CRC 来说「追加 4 个 0」和「什么都不追加」是两个不同的值。 */
static const uint8 svcrt_loader_zero4[4] = { 0u, 0u, 0u, 0u };

/* 镜像 CRC：头（crc32 与 state 两个字段按 0 代入）+ 重定位表 + 负载。
 * state 要按 0 代入，是因为它必须在写入之后被单独改写（提交动作），
 * 而 CRC 不能因此失效；crc32 字段自身同理——文件里它装着最终校验值，
 * 不能把它的字节也算进去，否则就成了「用结果算结果」。 */
static uint32 svcrt_loader_image_crc(const uint8 *image, const svcrt_app_header_t *p_hdr)
{
    uint32 total = SVCRT_APP_TOTAL_LEN(p_hdr);
    uint32 crc;

    crc = svcrt_crc32(image, SVCRT_APP_OFF_CRC32, 0u);
    crc = svcrt_crc32(svcrt_loader_zero4, 4u, crc);
    crc = svcrt_crc32(image + (SVCRT_APP_OFF_CRC32 + 4u),
                      SVCRT_APP_OFF_STATE - (SVCRT_APP_OFF_CRC32 + 4u), crc);
    crc = svcrt_crc32(svcrt_loader_zero4, 4u, crc);
    crc = svcrt_crc32(image + (SVCRT_APP_OFF_STATE + 4u),
                      SVCRT_APP_HEADER_SIZE - (SVCRT_APP_OFF_STATE + 4u), crc);
    crc = svcrt_crc32(image + SVCRT_APP_HEADER_SIZE,
                      total - SVCRT_APP_HEADER_SIZE, crc);

    return crc;
}

/* 注：曾有一个 svcrt_loader_payload_crc()，用来把落盘后的负载回读比对 hdr.image_id。
 * 它和 svcrt_loader_image_crc() 是同一个错：image_id 是对标称形态算的，
 * 而 Flash 上躺的是打过重定位补丁的形态，两者对不上。
 * 传输阶段的负载完整性现在由 svcrt_loader_stream_image() 的滚动 CRC 负责
 * （见那里的说明），落盘阶段则由 svcrt_port_flash_write() 的返回值把关，
 * 所以这个函数已无存在的必要。 */

/* ============================================================
 * 空闲空间查找：物理 0xFF 首适配
 * ============================================================ */

/**
* @brief 在池内找一段足够长、且不越过压实余量的已擦除空间
* @param size  需要的字节数
* @param skip_base 需要避开的区间起点（0 = 不回避）
* @param skip_size 需要避开的区间长度
* @param p_base 输出落点
* @return 0=找到，-1=没有
* @details 逐 4 字节字扫全池，跟踪当前 0xFF 连续段的长度。活镜像必然含
*          非 0xFF 字节（镜像头魔数），所以这个物理扫描同时避开了一切已装镜像，
*          不需要任何分配元数据。落点必须是 SVCRT_POOL_ALLOC_UNIT 的整数倍。
*/
/* 落点是否压在已登记的槽位记录上。
 *
 * 池里的物理 0xFF 扫描看不见槽位表：开发槽位的裸镜像记录可能横跨整个
 * 扇区，此时 find_clean 找到的候选落点必然被 svcrt_ptable_alloc 判为重叠，
 * 上层只能报出误导性的 NO_SLOT。这里返回冲突记录之后的地址（0 = 不冲突），
 * 让调用方能跳过这段区间继续找。 */
static uint32 svcrt_loader_slot_conflict_end(uint32 base, uint32 size)
{
    svcrt_partition_table_t *pt = svcrt_ptable_get();
    uint32 end = base + size;
    uint32 i;

    for(i = 0u; (i < pt->slot_max) && (i < SVCRT_SLOT_ARRAY_MAX); i++)
    {
        uint32 other_base;
        uint32 other_end;

        if(pt->slot_type[i] == SVCRT_SLOT_FREE)
        {
            continue;
        }

        other_base = pt->slot_base[i];
        other_end  = other_base + pt->slot_size[i];

        if((base < other_end) && (other_base < end))
        {
            return other_end;
        }
    }

    return 0u;
}

static int32 svcrt_loader_find_clean(uint32 size, uint32 skip_base, uint32 skip_size,
                                     uint32 *p_base)
{
    uint32 begin = IMAGE_POOL_BASE;
    uint32 limit = IMAGE_POOL_BASE + IMAGE_POOL_USABLE_SIZE;
    uint32 run_start = begin;
    uint32 run_end   = begin;
    uint32 p;

    if((size == 0u) || (size > IMAGE_POOL_USABLE_SIZE))
    {
        return -1;
    }

    for(p = begin; p < limit; p += 4u)
    {
        uint32 w = *(const volatile uint32 *)p;

        if(w == 0xFFFFFFFFu)
        {
            if(run_end != p)
            {
                run_start = p;      /* 上一段被非 0xFF 打断，从这里重新起算 */
            }
            run_end = p + 4u;
        }
        else
        {
            run_start = p + 4u;
            run_end   = p + 4u;
        }

        if((run_end - run_start) >= size)
        {
            /* 落点策略（优先精确隔离、退让给容量）：
             *   1) 先把落点向上取整到 2^n（2^n >= 跨度），此时镜像自身的跨度
             *      就是 2 的幂、基址也按该跨度对齐，MPU 一个 region 就能精确覆盖；
             *   2) 只有当对齐落点放不下而紧邻落点放得下时，才用紧邻落点，
             *      ROM 窗口退化为「包含该镜像的最小 2 的幂窗口」。 */
            uint32 cand = svcrt_loader_align_up(run_start, svcrt_loader_pow2_ceil(size));

            if(((cand + size) > run_end) ||
               ((cand % SVCRT_POOL_ALLOC_UNIT) != 0u))
            {
                cand = svcrt_loader_align_up(run_start, SVCRT_POOL_ALLOC_UNIT);
            }

            /* 对齐后仍要放得下，否则这一段不能用 */
            if((cand + size) > run_end)
            {
                continue;
            }

            /* 搬移时用来避开「正在清空的扇区」，免得刚搬走的镜像又落回去 */
            if((skip_size != 0u) && (cand < (skip_base + skip_size)) && (skip_base < (cand + size)))
            {
                continue;
            }

            /* 候选落点还不能压在已登记的槽位记录上（见上面该函数的说明） */
            {
                uint32 taken = svcrt_loader_slot_conflict_end(cand, size);

                if(taken != 0u)
                {
                    if(taken > run_start)
                    {
                        run_start = taken;
                        run_end   = taken;
                    }

                    continue;
                }
            }

            *p_base = cand;
            return 0;
        }
    }

    return -1;
}

/* ============================================================
 * 重定位
 * ============================================================ */

/* MOVW/MOVT 前缀半字的掩码：11110 i 10 0100/1100 imm4 —— 除 imm4 与 i 位，
 * 其余位（含 Rd 所在的那个半字）都不许动。 */
#define SVCRT_LOADER_MOV_PREFIX_MASK   (0xFBF0u)
#define SVCRT_LOADER_MOV_IMM4_MASK     (0x000Fu)

/**
* @brief 从 MOVW/MOVT 的一对半字里取出 imm16
* @param h1 前缀半字（11110 i 10 0100 imm4）
* @param h2 第二个半字（0 imm3 Rd imm8）
*/
static uint32 svcrt_loader_mov_imm16(uint16 h1, uint16 h2)
{
    return (((uint32)(h1 & SVCRT_LOADER_MOV_IMM4_MASK) << 12) |
            ((uint32)((h1 >> 10) & 1u) << 11) |
            ((uint32)((h2 >> 12) & 7u) << 8) |
            (uint32)(h2 & 0xFFu));
}

/**
* @brief 把 imm16 写回 MOVW/MOVT 的一对半字（Rd 与指令种类保持不变）
*/
static void svcrt_loader_mov_set_imm16(uint16 *p_h1, uint16 *p_h2, uint32 imm16)
{
    *p_h1 = (uint16)((*p_h1 & SVCRT_LOADER_MOV_PREFIX_MASK) |
                     (uint16)(((imm16 >> 11) & 1u) << 10) |
                     (uint16)((imm16 >> 12) & SVCRT_LOADER_MOV_IMM4_MASK));
    *p_h2 = (uint16)(((uint16)((imm16 >> 8) & 7u) << 12) |
                     (uint16)(*p_h2 & 0x0F00u) |
                     (uint16)(imm16 & 0xFFu));
}

/**
* @brief 给一对相邻的 MOVW+MOVT 立即数打补丁
* @param p     指向 MOVW 指令（半字对齐）
* @param delta 要叠加到这条 32 位地址上的增量
* @details 编译器把 32 位绝对地址拆成「MOVW 低 16 位 + MOVT 高 16 位」两条指令时，
*          链接期填进去的是指令内的立即数，字节变化不是「整字加增量」，只能解码
*          成 32 位值、加上增量、再重新编码。低 16 位会被进位的连带影响，所以两条
*          必须一起算，不能拆成两个独立表项。
*/
static void svcrt_loader_reloc_movw(uint8 *p, uint32 delta)
{
    uint16 h1w = *(uint16 *)(p);
    uint16 h2w = *(uint16 *)(p + 2u);
    uint16 h1t = *(uint16 *)(p + 4u);
    uint16 h2t = *(uint16 *)(p + 6u);
    uint32 value = ((svcrt_loader_mov_imm16(h1t, h2t) << 16) |
                    svcrt_loader_mov_imm16(h1w, h2w));

    value += delta;

    svcrt_loader_mov_set_imm16(&h1w, &h2w, value & 0xFFFFu);
    svcrt_loader_mov_set_imm16(&h1t, &h2t, (value >> 16) & 0xFFFFu);

    *(uint16 *)(p)      = h1w;
    *(uint16 *)(p + 2u) = h2w;
    *(uint16 *)(p + 4u) = h1t;
    *(uint16 *)(p + 6u) = h2t;
}

/**
* @brief 给一段负载打重定位补丁，并给出本块可以安全落盘的末尾
* @param buf   装载缓冲（对应镜像内偏移 buf_off 起的连续 len 字节）
* @param buf_off 该缓冲在镜像内的起始偏移
* @param len   缓冲长度
* @param p_hdr 镜像头（提供表项数与两条标称基址）
* @param p_rel 重定位表表体地址（流式安装指向刚落盘的 Flash 表，
*              池内搬移指向旧副本的表，RAM 缓冲路径指向缓冲里的表）
* @param idx   重定位表前进指针（跨块调用时必须复用同一个变量）
* @param delta_rom 本次要给 ROM 类表项叠加的增量
* @param delta_ram 本次要给 RAM 类表项叠加的增量
* @return 本块可以落盘的末尾偏移（镜像内）。正常等于 buf_off + len；若块尾
*         恰好切开一条指令类表项，则等于该表项的起始偏移，调用者必须把这段
*         之后的字节留到下一块开头一起处理。
* @details 表项按偏移升序，只要记住走到第几条，就能边收边改，不必把整表读进
*          RAM。数据类表项是整字、4 字节对齐，永远不会跨块；指令类表项
*          （MOVW/MOVT）长 8 字节且只保证半字对齐，「表项不跨块」这条对它们
*          不成立，所以要把可提交长度交回调用者。
*/
static uint32 svcrt_loader_reloc_apply(uint8 *buf, uint32 buf_off, uint32 len,
                                      const svcrt_app_header_t *p_hdr,
                                      const uint32 *p_rel,
                                      uint32 *p_idx,
                                      uint32 delta_rom, uint32 delta_ram)
{
    uint32 idx = *p_idx;
    uint32 end = buf_off + len;
    uint32 submit = end;

    while(idx < p_hdr->reloc_count)
    {
        uint32 entry = p_rel[idx];
        uint32 kind  = SVCRT_APP_RELOC_KIND(entry);
        uint32 off   = SVCRT_APP_RELOC_OFF(entry);
        uint32 need;
        uint32 delta;

        if(off < buf_off)
        {
            idx++;                  /* 落在本块之前（不该发生，防御性跳过） */
            continue;
        }

        if(off >= end)
        {
            break;                  /* 还没轮到 */
        }

        if((kind == SVCRT_APP_RELOC_KIND_ROM_MOVW) ||
           (kind == SVCRT_APP_RELOC_KIND_RAM_MOVW))
        {
            need = SVCRT_APP_RELOC_MOV_SIZE;
        }
        else
        {
            need = SVCRT_APP_RELOC_SIZE;
        }

        if((off + need) > end)
        {
            submit = off;           /* 这条表项被本块切断，尾部留给下一块 */
            break;
        }

        if((kind == SVCRT_APP_RELOC_KIND_RAM) ||
           (kind == SVCRT_APP_RELOC_KIND_RAM_MOVW))
        {
            delta = delta_ram;
        }
        else
        {
            delta = delta_rom;
        }

        if(need == SVCRT_APP_RELOC_MOV_SIZE)
        {
            svcrt_loader_reloc_movw(buf + (off - buf_off), delta);
        }
        else
        {
            uint32 *p_word = (uint32 *)(buf + (off - buf_off));

            *p_word += delta;
        }

        idx++;
    }

    *p_idx = idx;
    return submit;
}

/* ============================================================
 * 设备流式读写与流控
 * @details While a flash sector is erased (0.5~2 s on STM32F4) or a chunk is
 *          programmed, the CPU cannot fetch from flash, so the UART interrupt
 *          does not run and any byte arriving in that window is lost (the
 *          USART receive path holds a single byte). A sender that streams the
 *          whole file without pausing therefore loses bytes on any image of
 *          more than a few KB.
 *
 *          Protocol: the host sends the 256-byte header and the relocation
 *          table back to back (the table is not addressable until it has been
 *          written, so the device cannot ACK any earlier); the device then
 *          answers one ACK (0x06), and one more ACK after each payload chunk
 *          has been programmed. A NAK (0x15) means the frame was rejected and
 *          the host must stop sending. The host sends the next unit only
 *          after it sees a flow byte, so it never transmits while the device
 *          is blocked in flash.
 *          See tools/send_image.py for the host implementation.
 *
 *          A sender that ignores the ACKs (a plain file dump to the port)
 *          still works, it just keeps the old lossy behaviour.
 * ============================================================ */
#define SVCRT_LOADER_ACK_BYTE    (0x06u)
#define SVCRT_LOADER_NAK_BYTE    (0x15u)

static void svcrt_loader_ack(int32 dev)
{
    uint8 ack = SVCRT_LOADER_ACK_BYTE;

    (void)svcrt_dev_write_internal(dev, &ack, 1);
}

/* 拒绝帧：告诉主机「本帧已经没法继续收」，让它当场停手。
 * 没有它的话，主机只能靠 ACK 超时（几秒）才发现失败。 */
static void svcrt_loader_nak(int32 dev)
{
    uint8 nak = SVCRT_LOADER_NAK_BYTE;

    (void)svcrt_dev_write_internal(dev, &nak, 1);
}

/* 从设备读取指定长度（重试有限次，避免非阻塞读返回 0 时空转） */
static int32 svcrt_loader_read_dev(int32 dev, uint8 *buf, uint32 len)
{
    uint32 got = 0u;
    uint32 idle = 0u;

    while(got < len)
    {
        int32 r = svcrt_dev_read_internal(dev, buf + got, (int32)(len - got));

        if(r > 0)
        {
            got += (uint32)r;
            idle = 0u;
        }
        else
        {
            idle++;
            if(idle > 100000u)      /* 约百万次空转后放弃，防止死等 */
            {
                return -1;
            }
        }
    }

    return (int32)got;
}

/* 镜像头在镜像 CRC 里的参与方式：crc32 与 state 两个字段各按四个 0 字节代入。
 * 注意不能把这两段「跳过」：打包工具算校验时这两个字段确实是 0，但零字节
 * 是要参与 CRC 运算的，跳过与代入 0 得到的值不同（实测 APP_DEMO 差
 * 0x27DE38AB vs 0x474E26D7），跳过会让每一次安装都误报 CRC 错。 */
static uint32 svcrt_loader_crc_header(const svcrt_app_header_t *p_hdr)
{
    const uint8 *p = (const uint8 *)p_hdr;
    uint32 crc;

    crc = svcrt_crc32(p, SVCRT_APP_OFF_CRC32, 0u);
    crc = svcrt_crc32(svcrt_loader_zero4, 4u, crc);
    crc = svcrt_crc32(p + (SVCRT_APP_OFF_CRC32 + 4u),
                      SVCRT_APP_OFF_STATE - (SVCRT_APP_OFF_CRC32 + 4u), crc);
    crc = svcrt_crc32(svcrt_loader_zero4, 4u, crc);
    crc = svcrt_crc32(p + (SVCRT_APP_OFF_STATE + 4u),
                      SVCRT_APP_HEADER_SIZE - (SVCRT_APP_OFF_STATE + 4u), crc);

    return crc;
}

/* 把已接收的镜像尾部（重定位表 + 负载）流式落盘，并在落盘前打补丁。
 * 返回 0 或 SVCRT_LOADER_ERR_x。
 *
 * @param p_crc_written 出参：实际落盘形态（打过重定位补丁）的镜像 CRC。
 *                      调用方用它回读复核，见 svcrt_loader_image_crc()。
 *
 * @note 头里的 crc32 是对「标称形态」算的（打包工具算的，那时还没打过补丁）。
 *       因此完整性校验必须在**收字节的时候**对原始字节滚动计算，不能在落盘后
 *       回读整个区域去比 hdr.crc32：落盘的是打了 delta 的形态，只要本次落点
 *       不是标称基址，两者就不一样，回读比 hdr.crc32 永远不过。
 *       回读那一遍另有用途：把回读值和本文里记下的 p_crc_written 比，
 *       那才是真在验「Flash 写进去的字节对不对」。 */
static int32 svcrt_loader_stream_image(int32 dev, uint32 base, const svcrt_app_header_t *p_hdr,
                                       uint32 *p_idx, uint32 delta_rom, uint32 delta_ram,
                                       uint32 *p_crc_written)
{
    uint32 rt_len = p_hdr->reloc_count * SVCRT_APP_RELOC_SIZE;
    uint32 written = 0u;
    uint32 crc_recv;            /* 标称形态（收到的原始字节） */
    uint32 crc_written;         /* 落盘形态（打过补丁的字节） */
    uint32 crc_hdr;             /* 只算完镜像头时的值（诊断用） */
    uint32 crc_rel;             /* 只算完重定位表时的值（诊断用） */

    crc_recv    = svcrt_loader_crc_header(p_hdr);
    crc_written = crc_recv;
    crc_hdr     = crc_recv;
    crc_rel     = crc_recv;

    /* 重定位表紧跟镜像头，先整段落盘 */
    if(rt_len != 0u)
    {
        uint32 got = 0u;

        while(got < rt_len)
        {
            uint32 want = rt_len - got;

            if(want > SVCRT_LOADER_CHUNK_SIZE)
            {
                want = SVCRT_LOADER_CHUNK_SIZE;
            }

            if(svcrt_loader_read_dev(dev, svcrt_loader_chunk, want) != (int32)want)
            {
                return SVCRT_LOADER_ERR_SIZE;
            }

            /* 表体是原样落盘的，所以这一份字节既进标称 CRC 也进落盘 CRC */
            crc_recv    = svcrt_crc32(svcrt_loader_chunk, want, crc_recv);
            crc_written = svcrt_crc32(svcrt_loader_chunk, want, crc_written);

            if(svcrt_port_flash_write(base + SVCRT_APP_RELOC_OFFSET + got,
                                      svcrt_loader_chunk, want) != 0)
            {
                return SVCRT_LOADER_ERR_FLASH;
            }

            got += want;
        }
    }

    crc_rel = crc_recv;

    /* 表体到这一刻才落盘，也才能被寻址：在这里校验它。
     * 表体非法的话，后面的补丁会按乱序/越界的偏移乱改 Flash，
     * 而且改完就不可逆，所以必须在收负载之前先拒掉。 */
    {
        int32 chk = svcrt_loader_check_header(p_hdr,
                                              (const uint32 *)(base + SVCRT_APP_RELOC_OFFSET));

        if(chk != 0)
        {
            /* NAK 在协议里只是一个字节（0x15 = 本帧到此为止），不带原因。主机的做法是
             * 读同一路串口，所以**这里打出来的日志就是主机唯一能拿到的失败原因**。
             * 以前这里闷着不响，主机只好自己猜一个原因（曾把任何 NAK 都印成
             * "relocation table invalid"），而真正的失败点可能是魔数/兼容号/类型/
             * 尺寸/入口中的任何一个，猜错方向会让排查白跑一轮。 */
            SVCRT_LOGE("LOADER", "frame rejected before payload: err=%d", (int)chk);
            SVCRT_LOGE("LOADER", "hdr: magic=%08X compat=%08X type=%u size=%u ent=%u r_off=%u r_cnt=%u r_kind=%u",
                       (unsigned)p_hdr->magic, (unsigned)p_hdr->hw_compat_id,
                       (unsigned)p_hdr->type, (unsigned)p_hdr->image_size,
                       (unsigned)p_hdr->entry_offset, (unsigned)p_hdr->reloc_offset,
                       (unsigned)p_hdr->reloc_count, (unsigned)p_hdr->reloc_kind);
            svcrt_loader_nak(dev);
            return chk;         /* 返回细分码，不再一律报 RELOC：上层与日志都按真实原因走 */
        }
    }

    /* 表和字段都过了，主机可以开始送负载 */
    svcrt_loader_ack(dev);

    {
        uint32 carry = 0u;      /* 上一轮被表项切开、留到本轮开头的字节数 */

        while(written < p_hdr->image_size)
        {
            /* How many bytes still have to arrive over the wire: the payload
             * that is left, minus the tail already held in carry.  Sizing this
             * from image_size - written alone over-reads by carry, and that
             * only shows up on the last round: when a relocation entry
             * straddles the final host block, the loop would wait for bytes
             * the host has already finished sending (it sent the whole
             * payload), stalling until the read times out with ERR_SIZE. */
            uint32 left = p_hdr->image_size - written;
            uint32 want = (left > carry) ? (left - carry) : 0u;
            uint32 submit;
            uint32 keep;

            if(want > (SVCRT_LOADER_CHUNK_SIZE - carry))
            {
                want = SVCRT_LOADER_CHUNK_SIZE - carry;
            }

            {
                uint32 recv = want;     /* 本轮真正新收到的字节数（不含上一轮的尾巴） */

                if(want == 0u)
                {
                    /* Nothing left to read yet the payload is not committed:
                     * the relocation table runs past the end of the image.
                     * The packer must never produce that, so refuse the frame
                     * instead of spinning on bytes that will never arrive. */
                    SVCRT_LOGE("LOADER", "reloc table runs past payload end: written=%u carry=%u size=%u",
                               (unsigned)written, (unsigned)carry,
                               (unsigned)p_hdr->image_size);
                    svcrt_loader_nak(dev);
                    return SVCRT_LOADER_ERR_RELOC;
                }

                if(svcrt_loader_read_dev(dev, svcrt_loader_chunk + carry, want) != (int32)want)
                {
                    return SVCRT_LOADER_ERR_SIZE;
                }

                /* 标称 CRC 必须在打补丁之前算：这些字节马上会被就地改成落盘形态。
                 * 只算新收到的这段，尾巴在上一轮已经算过了。 */
                crc_recv = svcrt_crc32(svcrt_loader_chunk + carry, recv, crc_recv);
            }

            want += carry;

            /* 补丁必须在落盘之前打：Flash 只能把 1 写成 0，写下去就改不动了。
             * 若块尾正好切开一条 MOVW/MOVT 表项，本块只落盘到该表项之前，
             * 尾部几个字节原样留到下一轮开头，和后续数据拼齐再处理。 */
            submit = svcrt_loader_reloc_apply(svcrt_loader_chunk,
                                              p_hdr->payload_offset + written, want,
                                              p_hdr,
                                              (const uint32 *)(base + SVCRT_APP_RELOC_OFFSET),
                                              p_idx, delta_rom, delta_ram);

            keep = want - (submit - (p_hdr->payload_offset + written));

            if(keep == want)
            {
                return SVCRT_LOADER_ERR_RELOC;  /* 缓冲装不下一条表项，不应发生 */
            }

            if((submit - (p_hdr->payload_offset + written)) != 0u)
            {
                uint32 n = submit - (p_hdr->payload_offset + written);

                /* 落盘 CRC 算的是「打进 Flash 的那份字节」，与回读结果对得上才算写对 */
                crc_written = svcrt_crc32(svcrt_loader_chunk, n, crc_written);

                if(svcrt_port_flash_write(base + p_hdr->payload_offset + written,
                                          svcrt_loader_chunk, n) != 0)
                {
                    return SVCRT_LOADER_ERR_FLASH;
                }
            }

            written = submit - p_hdr->payload_offset;

            if(keep != 0u)
            {
                uint32 k;

                for(k = 0u; k < keep; k++)
                {
                    svcrt_loader_chunk[k] = svcrt_loader_chunk[want - keep + k];
                }
            }

            carry = keep;

            /* This chunk is in flash: let the host send the next one. */
            svcrt_loader_ack(dev);
        }
    }

    /* 收到的字节和打包时算出来的 crc32 必须一致：这一步盖住了传输丢字节、
     * 主机发错文件、镜像文件本身损坏三类问题。 */
    if(crc_recv != p_hdr->crc32)
    {
        SVCRT_LOGE("LOADER", "transfer crc: hdr 0x%08X rel 0x%08X end 0x%08X want 0x%08X",
                   (unsigned)crc_hdr, (unsigned)crc_rel,
                   (unsigned)crc_recv, (unsigned)p_hdr->crc32);
        svcrt_loader_nak(dev);
        return SVCRT_LOADER_ERR_CRC;
    }

    *p_crc_written = crc_written;

    return 0;
}

/* ============================================================
 * 安装
 * ============================================================ */

/* 在池里为 total 字节的镜像找落点、分配 RAM、登记槽位。
 * 成功时 *p_slot 为记录号，*p_base 为落点，*p_ram_base 为 RAM 块基址。 */
/* ============================================================
 * 安装策略：自动选址 / 固定槽位
 *
 * AUTO ：落点由 svcrt_loader_find_clean() 现算（第一段足够长的已擦除空间），
 *        卸载后按 reclaim_mode 压实回收，空间回到池里。
 * FIXED：落点与 RAM 窗口都取自设备端配置里的槽表（见 svcrt_layout.h），
 *        槽位与配置一一对应；卸载只擦本槽、不搬移、空间不退回池。
 *        这是「把设备交给客户二次开发、客户直接用 MDK 往固定地址下载」
 *        的使用形态。
 * ============================================================ */

/* 下一次安装落在配置槽表的哪一条（-1 = 内核按类型自己挑）。一次性提示。 */
static int32 svcrt_loader_slot_hint = -1;

void svcrt_loader_slot_hint_set(int32 index)
{
    svcrt_loader_slot_hint = index;
}

int32 svcrt_loader_slot_hint_get(void)
{
    return svcrt_loader_slot_hint;
}

/* 固定槽位模式下挑槽：只认配置里登记的地址，不在池里另找空位。 */
static int32 svcrt_loader_reserve_fixed(uint32 type, const svcrt_app_header_t *p_hdr,
                                        uint32 total, uint32 *p_base, uint32 *p_ram_base)
{
    svcrt_partition_table_t *pt = svcrt_ptable_get();
    const svcrt_cfg_slot_t *slots = svcrt_layout_slots();
    uint32 count = svcrt_layout_slot_count();
    uint32 want  = (type == SVCRT_SLOT_APP) ? (uint32)SVCRT_CFG_TYPE_APP
                                            : (uint32)SVCRT_CFG_TYPE_DRIVER;
    int32  hint  = svcrt_loader_slot_hint;
    uint32 i;

    (void)p_hdr;

    /* 提示是一次性的：消费掉，后面的安装回到默认行为 */
    svcrt_loader_slot_hint = -1;

    for(i = 0u; i < count; i++)
    {
        uint32 s_base = slots[i].base;
        uint32 s_size = slots[i].size;
        uint32 j;
        int32  slot;

        if(slots[i].type != want)
        {
            continue;
        }
        if((hint >= 0) && ((uint32)hint != i))
        {
            continue;               /* 操作者点名了槽，别的槽一律不看 */
        }
        if(total > s_size)
        {
            /* 点名的槽装不下就直接说清楚；默默换下一个槽不是操作者要的结果 */
            return (hint >= 0) ? SVCRT_LOADER_ERR_SIZE : SVCRT_LOADER_ERR_NOSPACE;
        }

        /* 正在运行的镜像不能被覆盖：擦写会让它当场跑飞。
         * 其它状态（EMPTY / LOADED / INSTALLING / INVALID）都可以覆盖——
         * 固定槽位的语义就是「这个地址永远属于这个槽」。 */
        for(j = 0u; (j < pt->slot_max) && (j < SVCRT_SLOT_ARRAY_MAX); j++)
        {
            if(pt->slot_type[j] == SVCRT_SLOT_FREE)
            {
                continue;
            }
            if(!((s_base < (pt->slot_base[j] + pt->slot_size[j])) &&
                 (pt->slot_base[j] < (s_base + s_size))))
            {
                continue;           /* 与这个槽没有交集 */
            }
            if(pt->slot_state[j] == SVCRT_APP_SLOT_RUNNING)
            {
                return SVCRT_LOADER_ERR_BUSY;
            }
        }

        /* 覆盖安装：先把落在本槽内的旧记录摘掉，再按整槽登记，
         * 这样分区表里的区间永远等于配置里的槽，不多不少。 */
        for(j = 0u; (j < pt->slot_max) && (j < SVCRT_SLOT_ARRAY_MAX); j++)
        {
            if(pt->slot_type[j] == SVCRT_SLOT_FREE)
            {
                continue;
            }
            if((s_base < (pt->slot_base[j] + pt->slot_size[j])) &&
               (pt->slot_base[j] < (s_base + s_size)))
            {
                svcrt_ptable_free(j);
            }
        }

        slot = svcrt_ptable_alloc(type, s_base, s_size);

        if(slot < 0)
        {
            return SVCRT_LOADER_ERR_NO_SLOT;
        }

        (void)svcrt_ptable_ram_bind((uint32)slot, slots[i].ram_base, slots[i].ram_size);

        *p_base     = s_base;
        *p_ram_base = slots[i].ram_base;
        return slot;
    }

    return (hint >= 0) ? SVCRT_LOADER_ERR_NO_SLOT : SVCRT_LOADER_ERR_NOSPACE;
}

static int32 svcrt_loader_reserve(uint32 type, const svcrt_app_header_t *p_hdr,
                                  uint32 total, uint32 *p_base, uint32 *p_ram_base)
{
    svcrt_partition_table_t *pt = svcrt_ptable_get();
    uint32 ram_base = 0u;
    uint32 ram_size = 0u;
    uint32 base = 0u;
    int32 slot;

    /* 记录与查找都按分配粒度向上取整后的跨度算：槽位里存的字节数必须与
     * 上电扫描重建出来的值完全一致，否则搬移时算出的长度会差一个尾巴。 */
    uint32 span = svcrt_loader_align_up(total, SVCRT_POOL_ALLOC_UNIT);

    /* 安装策略来自设备端配置区（见 svcrt_layout.h）：固定槽位模式下
     * 落点是人定的，池里有没有空位与本次安装无关。 */
    if(pt->layout_mode == (uint32)SVCRT_LAYOUT_MODE_FIXED)
    {
        return svcrt_loader_reserve_fixed(type, p_hdr, total, p_base, p_ram_base);
    }

    /* 自动选址不认槽位提示，但同样要把它消费掉，免得留下一枚哑弹 */
    svcrt_loader_slot_hint = -1;

    if(svcrt_loader_find_clean(span, 0u, 0u, &base) != 0)
    {
        return SVCRT_LOADER_ERR_NOSPACE;
    }

    if(svcrt_ptable_ram_alloc(p_hdr->ram_size, &ram_base, &ram_size) != 0)
    {
        return SVCRT_LOADER_ERR_NOSPACE;
    }

    slot = svcrt_ptable_alloc(type, base, span);

    if(slot < 0)
    {
        return SVCRT_LOADER_ERR_NO_SLOT;
    }

    (void)svcrt_ptable_ram_bind((uint32)slot, ram_base, ram_size);

    *p_base     = base;
    *p_ram_base = ram_base;

    (void)pt;

    return slot;
}

/* 从设备流式安装一个镜像（镜像头已由调用方读出并完成魔数同步）。
 *
 * 这是唯一的安装实现：App 与驱动走同一条路径，只是镜像头里的 type 不同。
 * 落点由内核按「第一段足够长的已擦除空间」现算，镜像头里的 nominal_base
 * 只用于重定位，不表示落点。 */
int32 svcrt_loader_load_dev_hdr(int32 dev, const svcrt_app_header_t *p_hdr, uint32 image_len)
{
    svcrt_app_header_t hdr = *p_hdr;
    uint32 total;
    uint32 base = 0u;
    uint32 ram_base = 0u;
    uint32 delta_rom;
    uint32 delta_ram;
    uint32 idx = 0u;
    uint32 crc_written = 0u;
    int32 slot;
    int32 ret;

    if(dev < 0)
    {
        return SVCRT_LOADER_ERR_PARAM;
    }

    /* 此刻只收到 256 字节的头，表体还在设备上：先只校字段，
     * 表体等 svcrt_loader_stream_image() 把它落盘后再校。 */
    ret = svcrt_loader_check_header(&hdr, 0);
    if(ret != 0)
    {
        return ret;
    }

    total = SVCRT_APP_TOTAL_LEN(&hdr);
    if((image_len != 0u) && (total > image_len))
    {
        return SVCRT_LOADER_ERR_SIZE;
    }

    if(total > IMAGE_POOL_USABLE_SIZE)
    {
        return SVCRT_LOADER_ERR_SIZE;
    }

    slot = svcrt_loader_reserve(hdr.type, &hdr, total, &base, &ram_base);
    if(slot < 0)
    {
        return slot;
    }

    /* 负载被链接在 nominal_base，运行期却落在 base + payload_offset：
     * 重定位表本身把负载往后推了 reloc_count*4 字节，这部分位移必须计入，
     * 否则代码里的自引用会整体偏出一个表长。 */
    delta_rom = (base + hdr.payload_offset) - hdr.nominal_base;
    delta_ram = ram_base - hdr.nominal_ram_base;

    /* 先写头：state 置 UNCOMMITTED，等 CRC 复核通过再单独改这一个字。
     * 这样中途掉电留下的残片会被上电扫描认定为无效副本而丢弃。 */
    hdr.state = SVCRT_APP_STATE_UNCOMMITTED;

    if(svcrt_port_flash_write(base, (const uint8 *)&hdr, SVCRT_APP_HEADER_SIZE) != 0)
    {
        svcrt_ptable_free((uint32)slot);
        return SVCRT_LOADER_ERR_FLASH;
    }

    /* stream_image 自己会把「收到的字节 vs 头里的 crc32」比完（标称形态），
     * 同时把「实际落盘形态」的 CRC 交回来给下面回读复核用。 */
    ret = svcrt_loader_stream_image(dev, base, &hdr, &idx, delta_rom, delta_ram, &crc_written);

    if(ret != 0)
    {
        svcrt_ptable_free((uint32)slot);
        return ret;
    }

    /* 回读复核：Flash 已映射，可直接按地址重算，与写下去的那份比。
     * 不能拿 hdr.crc32 比——那是标称形态的 CRC，而这里躺着的是打了 delta 的
     * 形态，两者只在本次落点恰好等于标称基址时才相等。 */
    {
        uint32 rb = svcrt_loader_image_crc((const uint8 *)base, &hdr);

        if(rb != crc_written)
        {
            SVCRT_LOGE("LOADER", "readback crc: flash 0x%08X written 0x%08X (base 0x%08X)",
                       (unsigned)rb, (unsigned)crc_written, (unsigned)base);
            svcrt_ptable_free((uint32)slot);
            return SVCRT_LOADER_ERR_CRC;
        }
    }

    /* 提交点：单字写入。写下去之前掉电算「没装成」，写下去之后就算装成了；
     * 1 -> 0 是把已置位擦回 0，方向合法（Flash 只能把 1 写成 0）。 */
    hdr.state = SVCRT_APP_STATE_VALID;

    if(svcrt_port_flash_write(base + SVCRT_APP_OFF_STATE,
                              (const uint8 *)&hdr.state, 4u) != 0)
    {
        svcrt_ptable_free((uint32)slot);
        return SVCRT_LOADER_ERR_FLASH;
    }

    /* 新镜像写入成功：清零故障计数（重新安装 = 重新开始） */
    svcrt_ptable_get()->slot_crash_cnt[slot] = 0u;
    svcrt_crash_forget((uint32)slot);   /* the cross-reset copy goes too */

    svcrt_ptable_set_slot((uint32)slot, SVCRT_APP_SLOT_LOADED,
                          svcrt_loader_entry_addr(base, hdr.payload_offset, hdr.entry_offset),
                          0u);

    return slot;
}

int32 svcrt_loader_load_dev(int32 dev, uint32 image_len)
{
    svcrt_app_header_t hdr;

    if(dev < 0)
    {
        return SVCRT_LOADER_ERR_PARAM;
    }

    if(svcrt_loader_read_dev(dev, (uint8 *)&hdr, SVCRT_APP_HEADER_SIZE) != (int32)SVCRT_APP_HEADER_SIZE)
    {
        return SVCRT_LOADER_ERR_SIZE;
    }

    return svcrt_loader_load_dev_hdr(dev, &hdr, image_len);
}

/* 驱动专用包装：驱动镜像走同一条安装路径，只是额外要求 type == DRIVER。 */
int32 svcrt_loader_load_driver_dev(int32 dev, const svcrt_app_header_t *p_hdr, uint32 image_len)
{
    if(p_hdr->type != SVCRT_APP_TYPE_DRIVER)
    {
        return SVCRT_LOADER_ERR_PARAM;
    }

    return svcrt_loader_load_dev_hdr(dev, p_hdr, image_len);
}

int32 svcrt_loader_load_driver(int32 dev, uint32 image_len)
{
    svcrt_app_header_t hdr;

    if(dev < 0)
    {
        return SVCRT_LOADER_ERR_PARAM;
    }

    if(svcrt_loader_read_dev(dev, (uint8 *)&hdr, SVCRT_APP_HEADER_SIZE) != (int32)SVCRT_APP_HEADER_SIZE)
    {
        return SVCRT_LOADER_ERR_SIZE;
    }

    return svcrt_loader_load_driver_dev(dev, &hdr, image_len);
}

/* 内存缓冲区安装路径：把整镜像拷进 RAM 打补丁后再落盘。
 * 预留给内核自测 / 后续 OTA 复用；当前内核内没有调用者。 */
int32 svcrt_loader_load_buffer(const uint8 *image, uint32 image_len)
{
    svcrt_partition_table_t *pt;
    const svcrt_app_header_t *p_src;
    svcrt_app_header_t hdr;
    uint32 total;
    uint32 base = 0u;
    uint32 ram_base = 0u;
    uint32 delta_rom;
    uint32 delta_ram;
    uint32 idx = 0u;
    uint32 crc_written;
    int32 slot;
    int32 ret;

    if(image == 0 || image_len < SVCRT_APP_HEADER_SIZE)
    {
        return SVCRT_LOADER_ERR_PARAM;
    }

    p_src = (const svcrt_app_header_t *)image;

    ret = svcrt_loader_check_header(
              p_src,
              (const uint32 *)((const uint8 *)p_src + SVCRT_APP_RELOC_OFFSET));
    if(ret != 0)
    {
        return ret;
    }

    total = SVCRT_APP_TOTAL_LEN(p_src);
    if(total > image_len)
    {
        return SVCRT_LOADER_ERR_SIZE;
    }

    if(svcrt_loader_image_crc(image, p_src) != p_src->crc32)
    {
        return SVCRT_LOADER_ERR_CRC;
    }

    pt = svcrt_ptable_get();

    slot = svcrt_loader_reserve(p_src->type, p_src, total, &base, &ram_base);
    if(slot < 0)
    {
        return slot;
    }

    delta_rom = (base + p_src->payload_offset) - p_src->nominal_base;
    delta_ram = ram_base - p_src->nominal_ram_base;

    hdr = *p_src;
    hdr.state = SVCRT_APP_STATE_UNCOMMITTED;

    if(svcrt_port_flash_write(base, (const uint8 *)&hdr, SVCRT_APP_HEADER_SIZE) != 0)
    {
        svcrt_ptable_free((uint32)slot);
        return SVCRT_LOADER_ERR_FLASH;
    }

    crc_written = svcrt_loader_crc_header(&hdr);

    /* 重定位表原样落盘 */
    {
        uint32 rt_len = p_src->reloc_count * SVCRT_APP_RELOC_SIZE;

        if((rt_len != 0u) &&
           (svcrt_port_flash_write(base + SVCRT_APP_RELOC_OFFSET,
                                   image + SVCRT_APP_RELOC_OFFSET, rt_len) != 0))
        {
            svcrt_ptable_free((uint32)slot);
            return SVCRT_LOADER_ERR_FLASH;
        }
    }

    /* 负载分块拷贝 + 打补丁 */
    {
        uint32 written = 0u;

        while(written < p_src->image_size)
        {
            uint32 want = p_src->image_size - written;
            uint32 submit;
            uint32 i;

            if(want > SVCRT_LOADER_CHUNK_SIZE)
            {
                want = SVCRT_LOADER_CHUNK_SIZE;
            }

            for(i = 0u; i < want; i++)
            {
                svcrt_loader_chunk[i] =
                    image[p_src->payload_offset + written + i];
            }

            /* 源数据还在 RAM 里，被切开的尾部下一轮重读即可 */
            submit = svcrt_loader_reloc_apply(
                         svcrt_loader_chunk,
                         p_src->payload_offset + written, want,
                         p_src,
                         (const uint32 *)((const uint8 *)p_src + SVCRT_APP_RELOC_OFFSET),
                         &idx, delta_rom, delta_ram);

            if(submit <= (p_src->payload_offset + written))
            {
                svcrt_ptable_free((uint32)slot);
                return SVCRT_LOADER_ERR_RELOC;
            }

            {
                uint32 n = submit - (p_src->payload_offset + written);

                crc_written = svcrt_crc32(svcrt_loader_chunk, n, crc_written);

                if(svcrt_port_flash_write(base + p_src->payload_offset + written,
                                          svcrt_loader_chunk, n) != 0)
                {
                    svcrt_ptable_free((uint32)slot);
                    return SVCRT_LOADER_ERR_FLASH;
                }
            }

            written = submit - p_src->payload_offset;
        }
    }

    /* 回读复核：与刚刚真正写下去的那份比（标称 CRC 对不上打了补丁的形态） */
    if(svcrt_loader_image_crc((const uint8 *)base, &hdr) != crc_written)
    {
        svcrt_ptable_free((uint32)slot);
        return SVCRT_LOADER_ERR_CRC;
    }

    /* 提交点：见 svcrt_loader_load_dev_hdr 的说明 */
    hdr.state = SVCRT_APP_STATE_VALID;

    if(svcrt_port_flash_write(base + SVCRT_APP_OFF_STATE,
                              (const uint8 *)&hdr.state, 4u) != 0)
    {
        svcrt_ptable_free((uint32)slot);
        return SVCRT_LOADER_ERR_FLASH;
    }

    pt->slot_crash_cnt[slot] = 0u;
    svcrt_crash_forget((uint32)slot);   /* the cross-reset copy goes too */

    svcrt_ptable_set_slot((uint32)slot, SVCRT_APP_SLOT_LOADED,
                          svcrt_loader_entry_addr(base, hdr.payload_offset, hdr.entry_offset),
                          0u);

    return slot;
}

/* ============================================================
 * 扫描与断电恢复
 * ============================================================ */

/* 开发期裸镜像表查询：返回该扇区单元是否有裸镜像条目，并给出类型与跨度。
 * 裸镜像没有镜像头，扫描器无法从内容推断「类型」与「占几个扇区」，
 * 这张表在 config/svcrt_partition.h 里显式声明（全工程唯一地址源头）。 */
#if (APP_ALLOW_RAW_IMAGE == 1)
static uint32 svcrt_loader_dev_slot(uint32 unit, uint32 *p_units, uint32 *p_type)
{
    static const struct
    {
        uint32 unit;
        uint32 units;
        uint32 type;
    } dev_slot[SVCRT_DEV_SLOT_MAX] = {
        { SVCRT_DEV_SLOT0_UNIT, SVCRT_DEV_SLOT0_UNITS, SVCRT_DEV_SLOT0_TYPE },
        { SVCRT_DEV_SLOT1_UNIT, SVCRT_DEV_SLOT1_UNITS, SVCRT_DEV_SLOT1_TYPE },
        { SVCRT_DEV_SLOT2_UNIT, SVCRT_DEV_SLOT2_UNITS, SVCRT_DEV_SLOT2_TYPE },
        { SVCRT_DEV_SLOT3_UNIT, SVCRT_DEV_SLOT3_UNITS, SVCRT_DEV_SLOT3_TYPE },
    };
    uint32 i;

    for(i = 0u; i < SVCRT_DEV_SLOT_MAX; i++)
    {
        if((dev_slot[i].type != 0u) && (dev_slot[i].unit == unit))
        {
            *p_units = dev_slot[i].units;
            *p_type  = dev_slot[i].type;
            return 1u;
        }
    }

    return 0u;
}
#endif

/* 认定一个带头的镜像；返回 0 表示可用，负值为原因。
 * *out_total 给出它的总长度（用于跳过整段区间）。 */
static int32 svcrt_loader_accept(uint32 base, uint32 *out_total, uint32 *out_entry,
                                 uint32 *out_autostart)
{
    const svcrt_app_header_t *p_hdr = (const svcrt_app_header_t *)base;
    uint32 total;

    if(svcrt_loader_check_header(
           p_hdr,
           (const uint32 *)((const uint8 *)p_hdr + SVCRT_APP_RELOC_OFFSET)) != 0)
    {
        /* 头字段不合法：不属于可用镜像。给出一个保守的跳过长度，
         * 免得在残片里逐个分配单元地磨下去。 */
        *out_total = SVCRT_POOL_ALLOC_UNIT;
        return -1;
    }

    total = SVCRT_APP_TOTAL_LEN(p_hdr);

    if((total > IMAGE_POOL_USABLE_SIZE) ||
       ((base + total) > (IMAGE_POOL_BASE + IMAGE_POOL_USABLE_SIZE)))
    {
        *out_total = SVCRT_POOL_ALLOC_UNIT;
        return -1;
    }

    *out_total = svcrt_loader_align_up(total, SVCRT_POOL_ALLOC_UNIT);

    /* 未提交的副本（搬移/安装中途掉电）一律作废。
     *
     * 这一条就是常驻副本的完整性判据：提交点是在全部字节都落盘之后才单独写
     * 的那一个字，没走到那一步的副本永远停在 UNCOMMITTED。
     *
     * 不能在这里再拿 hdr.crc32 比：那是打包时对「标称形态」算的，而 Flash 上
     * 躺的是打了重定位补丁的形态，只要落点不等于标称基址就对不上；
     * 而「落盘形态」的 CRC 又没法存回头里（Flash 只能把 1 写成 0，
     * 装完再回写一个任意 CRC 是做不到的）。 */
    if(p_hdr->state != SVCRT_APP_STATE_VALID)
    {
        return -1;
    }

    *out_entry = svcrt_loader_entry_addr(base, p_hdr->payload_offset, p_hdr->entry_offset);
    *out_autostart = ((p_hdr->flags & SVCRT_APP_FLAG_AUTOSTART) != 0u) ? 1u : 0u;

    return 0;
}

/* 扫描整个统一镜像池，重建槽位表。
 * 池内任意分配单元都可能是一个镜像的起点，因此按分配粒度线性扫过去，
 * 发现镜像就跳过它占用的整段区间。扫描是槽位表在重启后唯一的事实来源。 */
/* 开发期裸镜像认领：按开发槽位表（SVCRT_DEV_SLOTn_*）把区间占住。
 * 裸镜像没有镜像头，类型与跨度无法从内容推断，只能由那张表给出。
 *
 * 必须在带头镜像扫描之后调用：开发槽位落在池底，而运行期安装器同样从池底
 * 往后分配，所以一个「装进去的带头镜像」完全可能就落在开发槽位里。先认领
 * 会把这种镜像整段当成裸镜像，带电位的镜像头就再也认不出来——槽位表里见不到
 * 它，上电也就不会自启。反过来先扫带头镜像没有副作用：裸镜像的头不合法，
 * 扫描本来就会跳过它。 */
#if (APP_ALLOW_RAW_IMAGE == 1)
static void svcrt_loader_claim_dev_slots(void)
{
    svcrt_partition_table_t *pt = svcrt_ptable_get();
    uint32 unit;

    for(unit = 0u; unit < pt->pool_units; unit++)
    {
        uint32 dev_units = 1u;
        uint32 dev_type  = 0u;
        uint32 base;
        uint32 span;
        uint32 entry;
        uint32 r_base = 0u;
        uint32 r_size = 0u;

        if(svcrt_loader_dev_slot(unit, &dev_units, &dev_type) == 0u)
        {
            continue;
        }

        base = pt->pool_base + unit * pt->pool_sector;
        span = dev_units * pt->pool_sector;

        /* 带镜像头的内容一律不当作裸镜像，哪怕它因为编码版本不符 / CRC 不符 /
         * 未提交而被带头扫描拒收：认领成裸镜像会白占一整个扇区，而且这条记录
         * 让 reclaim 永远不敢擦这个扇区。把它留给 reclaim，按「扇区内没有任何
         * 有效槽位」判定后整片擦掉。 */
        if(*(const volatile uint32 *)base == (uint32)SVCRT_APP_MAGIC)
        {
            continue;
        }

        /* 已被带头镜像认领的区间不动（头尾各查一次，防止跨在边界上） */
        if((svcrt_ptable_find_addr(base, &r_base, &r_size) >= 0) ||
           (svcrt_ptable_find_addr(base + span - 4u, 0, 0) >= 0))
        {
            continue;
        }

        if(svcrt_loader_span_erased(base, span) != 0u)
        {
            continue;               /* 该槽位是空的，不当成镜像 */
        }

        entry = base;
        #if (SVCRT_ARCH_IS_ARM == 1)
        entry |= 1u;                /* Cortex-M：Thumb 指令集标志位 */
        #endif

        if(svcrt_ptable_alloc_raw(dev_type, base, span, entry) >= 0)
        {
            int32 raw_slot = svcrt_ptable_find_addr(base, 0, 0);

            if(raw_slot >= 0)
            {
                /* 裸镜像没有镜像头，扫描器读不到它的 ram_size，RAM 块只能由
                 * 开发槽位表静态给出：窗口序号 = 起始单元号，与 gen_scatter.py
                 * 生成 App .sct 用的是同一条公式。不 bind 的话
                 * svcrt_loader_start() 会因 ram_base==0 直接拒绝启动。 */
                (void)svcrt_ptable_ram_bind((uint32)raw_slot,
                                            SVCRT_DEV_SLOT_RAM_BASE(unit),
                                            (uint32)SVCRT_DEV_RAM_WINDOW);

                /* 裸镜像没有镜像头，无法携带 per-slot 策略，沿用原全局开关 */
                pt->slot_autostart[(uint32)raw_slot] =
                    (dev_type == SVCRT_APP_TYPE_DRIVER)
                    ? ((DRIVER_AUTO_START != 0) ? 1u : 0u)
                    : ((APP_AUTO_START != 0) ? 1u : 0u);

                if(dev_type == SVCRT_APP_TYPE_DRIVER)
                {
                    svcrt_loader_scan_drivers++;
                }
                else
                {
                    svcrt_loader_scan_apps++;
                }
            }
        }
    }
}
#else
static void svcrt_loader_claim_dev_slots(void)
{
    /* 关闭裸镜像支持：开发槽位不参与识别 */
}
#endif

static void svcrt_loader_scan_pool(void)

{
    svcrt_partition_table_t *pt;
    uint32 addr;

    if(svcrt_loader_scan_done != 0u)
    {
        return;
    }
    svcrt_loader_scan_done = 1u;

    /* 扫描前清空槽位表：共享 RAM 掉电不清，旧记录必须先全部丢弃。
     * 只清槽位表，不能重新种布整张分区表：它里
     * 还有 svcrt_layout_init() 从设备端配置区得出的安装模式与
     * 槽表，本扫描就要按它分流。 */
    svcrt_ptable_clear_slots();
    pt = svcrt_ptable_get();

    svcrt_loader_scan_apps    = 0u;
    svcrt_loader_scan_drivers = 0u;

    addr = IMAGE_POOL_BASE;

    while(addr < (IMAGE_POOL_BASE + IMAGE_POOL_USABLE_SIZE))
    {
        const svcrt_app_header_t *p_hdr = (const svcrt_app_header_t *)addr;
        uint32 total = 0u;
        uint32 entry = 0u;
        uint32 autostart = 0u;

        /* 已被裸镜像占用的区间：整段跳过 */
        {
            uint32 r_base = 0u;
            uint32 r_size = 0u;

            if(svcrt_ptable_find_addr(addr, &r_base, &r_size) >= 0)
            {
                if(r_base > addr)
                {
                    addr = r_base;
                }
                addr += r_size;
                continue;
            }
        }

        if(p_hdr->magic == SVCRT_APP_MAGIC)
        {
            if(svcrt_loader_accept(addr, &total, &entry, &autostart) == 0)
            {
                int32 slot = svcrt_ptable_alloc(p_hdr->type, addr, total);

                if(slot >= 0)
                {
                    svcrt_ptable_set_slot((uint32)slot, SVCRT_APP_SLOT_LOADED, entry, 0u);
                    /* scan 在调度启动之前单线程执行，此处直接写共享字段即可 */
                    pt->slot_autostart[slot] = autostart;

                    /* 镜像头声明的 RAM 需求要在这里重新绑定：RAM 分配结果
                     * 不落盘，重启后必须靠镜像头重建（块基址由分配器重算，
                     * 镜像内的 RAM 绝对地址在装载时就按同一个块打过补丁，
                     * 因此必须按「同样的分配顺序」重建才能对上）。
                     * 由于分配顺序 = 池内地址升序，而扫描正是按地址升序进行的，
                     * 重建结果与装载时一致。 */
                    {
                        uint32 ram_base = 0u;
                        uint32 ram_size = 0u;

                        if(svcrt_ptable_ram_alloc(p_hdr->ram_size, &ram_base, &ram_size) == 0)
                        {
                            (void)svcrt_ptable_ram_bind((uint32)slot, ram_base, ram_size);
                        }
                    }

                    if(p_hdr->type == SVCRT_APP_TYPE_DRIVER)
                    {
                        svcrt_loader_scan_drivers++;
                    }
                    else
                    {
                        svcrt_loader_scan_apps++;
                    }
                }

                addr += total;
                continue;
            }

            /* 不能用的带头内容（CRC 不符 / 未提交 / 重复副本）：
             * 不登记，交给 reclaim 按物理 0xFF 判定后擦除 */
            addr += (total != 0u) ? total : SVCRT_POOL_ALLOC_UNIT;
            continue;
        }

        addr += SVCRT_POOL_ALLOC_UNIT;
    }

    /* 带头镜像先认得，开发槽位里的裸镜像再认领。
     * 编译期开关决定这段代码在不在，运行期开关（设备端配置区 flags 的
     * RAW_ALLOW 位）决定这台设备要不要走它：整机交付后关掉，池里就只剩
     * 「带头安装」一条路，烧错地址的裸镜像不会再被当成镜像跑起来。 */
    if(svcrt_layout_raw_allow() != 0u)
    {
        svcrt_loader_claim_dev_slots();
    }
}

uint32 svcrt_loader_scan(void)
{
    svcrt_loader_scan_pool();
    return svcrt_loader_scan_apps;
}

uint32 svcrt_loader_scan_driver(void)
{
    svcrt_loader_scan_pool();
    return svcrt_loader_scan_drivers;
}

/* ============================================================
 * 搬移：把一个镜像整体挪到池内别处
 * @details 搬移不是「拷内存」，而是把已经打好补丁的字节按新落点**重打一遍**：
 *          ROM 类表项再叠加一个 (新落点 - 旧落点)，RAM 类不动（RAM 块没换）。
 *          全程走 UNCOMMITTED -> VALID 事务，中途掉电只会留下一个会被
 *          上电扫描丢弃的残片。
 * @return 0=成功，负值为 SVCRT_LOADER_ERR_x
 * ============================================================ */
static int32 svcrt_loader_move_slot(uint32 slot, uint32 new_base)
{
    svcrt_partition_table_t *pt = svcrt_ptable_get();
    const svcrt_app_header_t *p_old;
    svcrt_app_header_t hdr;
    uint32 old_base = pt->slot_base[slot];
    uint32 total;
    uint32 delta;
    uint32 written;
    uint32 idx = 0u;
    uint32 crc_written;

    /* 只有「已加载、未运行」的镜像可以搬：运行中的镜像 RAM 里有活状态，
     * 而且它的代码正在被取指；裸镜像没有镜像头，根本无从重定位。 */
    if(pt->slot_state[slot] != SVCRT_APP_SLOT_LOADED)
    {
        return SVCRT_LOADER_ERR_BUSY;
    }

    if((new_base == 0u) || (new_base >= old_base))
    {
        return SVCRT_LOADER_ERR_ADDR;       /* 只向下搬，保证往返单调、不会摆动 */
    }

    p_old = (const svcrt_app_header_t *)old_base;

    if(svcrt_loader_check_header(
           p_old,
           (const uint32 *)((const uint8 *)p_old + SVCRT_APP_RELOC_OFFSET)) != 0)
    {
        return SVCRT_LOADER_ERR_MAGIC;
    }

    total = SVCRT_APP_TOTAL_LEN(p_old);

    if((new_base < IMAGE_POOL_BASE) ||
       ((new_base + total) > (IMAGE_POOL_BASE + IMAGE_POOL_USABLE_SIZE)))
    {
        return SVCRT_LOADER_ERR_ADDR;
    }

    /* 目标必须是干净的：Flash 只能把 1 写成 0，写进没擦过的字节会直接失败 */
    if(svcrt_loader_span_erased(new_base, total) == 0u)
    {
        return SVCRT_LOADER_ERR_OCCUPIED;
    }

    hdr = *p_old;
    hdr.state = SVCRT_APP_STATE_UNCOMMITTED;
    delta = new_base - old_base;

    if(svcrt_port_flash_write(new_base, (const uint8 *)&hdr, SVCRT_APP_HEADER_SIZE) != 0)
    {
        return SVCRT_LOADER_ERR_FLASH;
    }

    crc_written = svcrt_loader_crc_header(&hdr);

    /* 重定位表原样搬：表项是「相对镜像起始」的偏移，与新落点无关 */
    {
        uint32 rt_len = hdr.reloc_count * SVCRT_APP_RELOC_SIZE;
        uint32 got = 0u;

        while(got < rt_len)
        {
            uint32 want = rt_len - got;
            uint32 i;

            if(want > SVCRT_LOADER_CHUNK_SIZE)
            {
                want = SVCRT_LOADER_CHUNK_SIZE;
            }

            for(i = 0u; i < want; i++)
            {
                svcrt_loader_chunk[i] =
                    *(const uint8 *)(old_base + SVCRT_APP_RELOC_OFFSET + got + i);
            }

            crc_written = svcrt_crc32(svcrt_loader_chunk, want, crc_written);

            if(svcrt_port_flash_write(new_base + SVCRT_APP_RELOC_OFFSET + got,
                                      svcrt_loader_chunk, want) != 0)
            {
                return SVCRT_LOADER_ERR_FLASH;
            }

            got += want;
        }
    }

    /* 负载：读出旧副本已打好的补丁，再叠加 (新落点 - 旧落点) */
    for(written = 0u; written < hdr.image_size; )
    {
        uint32 want = hdr.image_size - written;
        uint32 submit;
        uint32 i;

        if(want > SVCRT_LOADER_CHUNK_SIZE)
        {
            want = SVCRT_LOADER_CHUNK_SIZE;
        }

        for(i = 0u; i < want; i++)
        {
            svcrt_loader_chunk[i] =
                *(const uint8 *)(old_base + hdr.payload_offset + written + i);
        }

        /* 旧副本还在 Flash 里，被切开的尾部下一轮重读即可 */
        submit = svcrt_loader_reloc_apply(
                     svcrt_loader_chunk,
                     hdr.payload_offset + written, want,
                     &hdr,
                     (const uint32 *)(old_base + SVCRT_APP_RELOC_OFFSET),
                     &idx, delta, 0u);

        if(submit <= (hdr.payload_offset + written))
        {
            return SVCRT_LOADER_ERR_RELOC;
        }

        {
            uint32 n = submit - (hdr.payload_offset + written);

            crc_written = svcrt_crc32(svcrt_loader_chunk, n, crc_written);

            if(svcrt_port_flash_write(new_base + hdr.payload_offset + written,
                                      svcrt_loader_chunk, n) != 0)
            {
                return SVCRT_LOADER_ERR_FLASH;
            }
        }

        written = submit - hdr.payload_offset;
    }

    /* 全量复核：搬完的字节必须与「刚写下去的那份」逐字节等价。
     * 比的是本次会话攒出来的落盘形态 CRC，不是 hdr.crc32（标称形态）。 */
    if(svcrt_loader_image_crc((const uint8 *)new_base, &hdr) != crc_written)
    {
        return SVCRT_LOADER_ERR_CRC;
    }

    /* 提交：单字写 VALID。此前掉电 = 新副本作废、旧副本仍是有效的那份 */
    hdr.state = SVCRT_APP_STATE_VALID;

    if(svcrt_port_flash_write(new_base + SVCRT_APP_OFF_STATE,
                              (const uint8 *)&hdr.state, 4u) != 0)
    {
        return SVCRT_LOADER_ERR_FLASH;
    }

    /* 记录改指新落点；入口地址随落点平移 */
    if(svcrt_ptable_move(slot, new_base) != 0)
    {
        return SVCRT_LOADER_ERR_ADDR;
    }

    (void)svcrt_ptable_set_slot(slot, SVCRT_APP_SLOT_LOADED,
                                svcrt_loader_entry_addr(new_base, hdr.payload_offset,
                                                        hdr.entry_offset),
                                0u);

    return 0;
}

/* 扇区内是否还有在用记录 */
static uint32 svcrt_loader_sector_busy(const svcrt_partition_table_t *pt,
                                       uint32 s_base, uint32 s_end)
{
    uint32 i;

    for(i = 0u; (i < pt->slot_max) && (i < SVCRT_SLOT_ARRAY_MAX); i++)
    {
        uint32 b;
        uint32 sz;

        if(pt->slot_type[i] == SVCRT_SLOT_FREE)
        {
            continue;
        }

        b  = pt->slot_base[i];
        sz = pt->slot_size[i];

        if((b == 0u) || (sz == 0u))
        {
            continue;
        }

        if((b < s_end) && (s_base < (b + sz)))
        {
            return 1u;
        }
    }

    return 0u;
}

/* 全局压实：把每个活镜像搬到「池内最低、装得下的干净连续区」，反复到不动为止。
 * 每一步都走 svcrt_loader_move_slot，因此自带事务保护（新位置先写 UNCOMMITTED，
 * 校验通过才写 VALID 提交，最后擦旧位置），中途掉电只会留下可回收的残片。
 * 返回实际搬动的镜像数。 */
static uint32 svcrt_loader_compact_pool(void)
{
    svcrt_partition_table_t *pt = svcrt_ptable_get();
    uint32 moved_total = 0u;
    uint32 round;

    for(round = 0u; round < SVCRT_SLOT_ARRAY_MAX; round++)
    {
        uint32 moved = 0u;
        uint32 i;

        for(i = 0u; (i < pt->slot_max) && (i < SVCRT_SLOT_ARRAY_MAX); i++)
        {
            uint32 b;
            uint32 sz;
            uint32 nb = 0u;

            if((pt->slot_type[i] == SVCRT_SLOT_FREE) ||
               (pt->slot_state[i] != SVCRT_APP_SLOT_LOADED))
            {
                continue;
            }

            b  = pt->slot_base[i];
            sz = pt->slot_size[i];

            if((b == 0u) || (sz == 0u))
            {
                continue;           /* 空记录 */
            }

            /* 找池内最低的干净空位；不比当前位置低就不动它 */
            if(svcrt_loader_find_clean(sz, 0u, 0u, &nb) == 0)
            {
                if((nb < b) && (svcrt_loader_move_slot(i, nb) == 0))
                {
                    moved = 1u;
                    moved_total++;
                }
            }
        }

        if(moved == 0u)
        {
            break;
        }
    }

    return moved_total;
}

/* 池里还剩多少可以装镜像的物理空间。
 * @param p_largest 非空时写入「最长的一段连续空闲」字节数
 * @return 池可用区内所有 0xFF 连续段的总字节数
 * @details 这里只统计物理空闲，不做「按 2 的幂跨度折算」——镜像落点要求
 *          2 的幂对齐，实际能装下的总量会略小于该值，所以同时给出最长连续段，
 *          让调用者判断是「一个大洞」还是「同样总量但被切碎」。 */
uint32 svcrt_loader_pool_free(uint32 *p_largest)
{
    uint32 begin = IMAGE_POOL_BASE;
    uint32 limit = IMAGE_POOL_BASE + IMAGE_POOL_USABLE_SIZE;
    uint32 run = 0u;
    uint32 total = 0u;
    uint32 largest = 0u;
    uint32 p;

    for(p = begin; p < limit; p += 4u)
    {
        uint32 w = *(const volatile uint32 *)p;

        if(w == 0xFFFFFFFFu)
        {
            run += 4u;
            total += 4u;
        }
        else
        {
            if(run > largest)
            {
                largest = run;
            }

            run = 0u;
        }
    }

    if(run > largest)
    {
        largest = run;
    }

    if(p_largest != 0)
    {
        *p_largest = largest;
    }

    return total;
}

int32 svcrt_loader_reclaim(void)
{
    svcrt_partition_table_t *pt = svcrt_ptable_get();
    uint32 erased = 0u;
    uint32 sect;

    /* 回收力度是运行期决定的（宿主机在配置区里选），不再是编译期的 #if：
     * 先全局压实：把活镜像尽量往下挤，剩下的死字节再按扇区收尾。
     * 下面的逐扇区处理对两种力度都要跑：它们是两种策略，
     * 不是「全局模式下才该做的事」。 */
    if(pt->reclaim_mode == (uint32)SVCRT_CFG_RECLAIM_GLOBAL)
    {
        (void)svcrt_loader_compact_pool();
    }

    /* 逐个物理扇区处理：擦除粒度就是扇区，一个扇区里只要还有活镜像就擦不了，
     * 所以先把该扇区的活镜像向下搬（填补更低的空洞），再整体擦掉。 */
    for(sect = 0u; (sect < pt->pool_units) && (sect < 64u); sect++)
    {
        uint32 s_base = pt->pool_base + sect * pt->pool_sector;
        uint32 s_end  = s_base + pt->pool_sector;
        uint32 round;

        if(svcrt_loader_span_erased(s_base, pt->pool_sector) != 0u)
        {
            continue;               /* 本来就干净 */
        }

        /* 多轮搬移：每搬走一个，扇区里就少一个占位者；搬不动就收手 */
        for(round = 0u; round < SVCRT_SLOT_ARRAY_MAX; round++)
        {
            uint32 moved = 0u;
            uint32 i;

            for(i = 0u; (i < pt->slot_max) && (i < SVCRT_SLOT_ARRAY_MAX); i++)
            {
                uint32 b;
                uint32 sz;
                uint32 nb = 0u;

                if(pt->slot_type[i] == SVCRT_SLOT_FREE)
                {
                    continue;
                }

                b  = pt->slot_base[i];
                sz = pt->slot_size[i];

                if((b == 0u) || (sz == 0u))
                {
                    continue;       /* 空记录 */
                }

                if((b < s_end) && (s_base < (b + sz)) &&
                   (pt->slot_state[i] == SVCRT_APP_SLOT_LOADED))
                {
                    /* 避开正在清空的这个扇区，免得刚搬走又落回来 */
                    if(svcrt_loader_find_clean(sz, s_base, pt->pool_sector, &nb) == 0)
                    {
                        if(svcrt_loader_move_slot(i, nb) == 0)
                        {
                            moved = 1u;
                        }
                    }
                }
            }

            if(moved == 0u)
            {
                break;
            }
        }

        if(svcrt_loader_sector_busy(pt, s_base, s_end) == 0u)
        {
            if(svcrt_port_flash_erase(s_base, pt->pool_sector) == 0)
            {
                erased++;
            }
        }
    }

    return (int32)erased;
}

int32 svcrt_loader_uninstall(uint32 slot)
{
    svcrt_partition_table_t *pt = svcrt_ptable_get();
    uint32 state = SVCRT_APP_SLOT_EMPTY;
    uint32 task_id = 0u;
    uint32 dead_type = 0u;
    uint32 uninstall_base = 0u;
    uint32 uninstall_size = 0u;

    if((slot >= pt->slot_max) || (slot >= SVCRT_SLOT_ARRAY_MAX) ||
       (pt->slot_type[slot] == SVCRT_SLOT_FREE))
    {
        return SVCRT_LOADER_ERR_PARAM;
    }

    /* 裸镜像是开发者直接烧进池里的，没有镜像头也没有记录可言，
     * 内核不能替它做「卸载」（那会连带擦掉不属于它的区间） */
    if(pt->slot_state[slot] == SVCRT_APP_SLOT_RAW)
    {
        return SVCRT_LOADER_ERR_STATE;
    }

    if(svcrt_ptable_get_slot(slot, &state, 0, &task_id) != 0)
    {
        return SVCRT_LOADER_ERR_PARAM;
    }

    /* 正在运行的先停下：镜像字节仍在 Flash，之后还能再启动 */
    if((state == SVCRT_APP_SLOT_RUNNING) && (task_id != 0u))
    {
        svcrt_loader_halt_image(task_id);
    }

    /* Remember the slot's own range before the record is dropped: in fixed-slot
     * mode uninstall erases exactly this range and nothing else. */
    uninstall_base = pt->slot_base[slot];
    uninstall_size = pt->slot_size[slot];

    /* The authoritative uninstall action is invalidating the on-flash header.
     * Dropping the RAM record alone is not enough: erase granularity is the
     * whole pool sector, and a sector that still holds a live image can never
     * be erased (a RUNNING image is never moved). Those dead bytes would stay
     * VALID on flash and the next power-on scan would register the image again
     * - so uninstall must not depend on the erase succeeding.
     *
     * The invalidation must be a program the hardware can actually perform:
     * Flash programming only clears bits (1 -> 0). The image is committed, so
     * state is already 0 and writing UNCOMMITTED (1) would need a 0 -> 1 set
     * that the hardware silently refuses - the header would keep reading back
     * as VALID and the image would return after the next power cycle. What is
     * still writable is a field whose zero value is invalid: type = 0 is not a
     * known image type, so check_header() rejects the header and the boot scan
     * drops it. magic is left intact on purpose - it is the marker that tells
     * reclaim (erase this sector once no valid slot is left in it) and the
     * raw-image claim path that the range holds headed content. */
    if(svcrt_port_flash_write(pt->slot_base[slot] + SVCRT_APP_OFF_TYPE,
                              (const uint8 *)&dead_type, 4u) != 0)
    {
        return SVCRT_LOADER_ERR_FLASH;   /* keep the record: never report this as uninstalled */
    }

    svcrt_ptable_free(slot);

    /* 固定槽位模式：这块 Flash 永远属于这个槽，卸载就是把它自己擦干净。
     * 既不搬别人、也不把空间交回池子（池里本来就没登记它）。配置校验要求
     * 固定槽的 base/size 都是物理扇区整数倍，所以这里擦不到邻居。 */
    if(pt->layout_mode == (uint32)SVCRT_LAYOUT_MODE_FIXED)
    {
        if(uninstall_size == 0u)
        {
            return 0;
        }
        if(svcrt_port_flash_erase(uninstall_base, uninstall_size) != 0)
        {
            return SVCRT_LOADER_ERR_FLASH;
        }
        return (int32)(uninstall_size / pt->pool_sector);
    }

    /* Best effort from here on: the image is already dead even when its sector
     * stays pinned by a live neighbour. Return the reclaimed sector count so
     * the caller can tell "space back" from "marked dead, space still pinned". */
    return svcrt_loader_reclaim();
}

/* ============================================================
 * 启停
 * ============================================================ */

int32 svcrt_loader_start(uint32 slot)
{
    svcrt_partition_table_t *pt = svcrt_ptable_get();
    uint32 state = SVCRT_APP_SLOT_EMPTY;
    uint32 entry = 0u;
    uint32 ram_base = 0u;
    uint32 ram_size = 0u;
    uint32 stack_size;
    uint32 priority;
    uint32 period;
    uint32 stack_bottom;
    int32 task_id;

    if((slot >= pt->slot_max) || (slot >= SVCRT_SLOT_ARRAY_MAX))
    {
        return SVCRT_LOADER_ERR_PARAM;
    }

    /* A slot the kernel disabled for repeated faults does not come back on its
     * own. The check sits here rather than in the callers because this is the
     * only door into the scheduler: a policy that each caller has to remember
     * is a policy that one caller will eventually forget. */
    if(svcrt_crash_disabled(slot) != 0u)
    {
        return SVCRT_LOADER_ERR_STATE;
    }

    /* state / entry / task_id 一次读完，避免与故障处理路径竞争 */
    if(svcrt_ptable_get_slot(slot, &state, &entry, 0) != 0)
    {
        return SVCRT_LOADER_ERR_PARAM;
    }

    /* RAW 是开发槽位的裸镜像：没有镜像头，但落点固定、RAM 窗口由开发槽位表
     * 静态给出，具备与 LOADED 相同的启动条件。 */
    if(((state != SVCRT_APP_SLOT_LOADED) && (state != SVCRT_APP_SLOT_RAW)) ||
       (entry == 0u))
    {
        return SVCRT_LOADER_ERR_STATE;
    }

    if(pt->slot_type[slot] == SVCRT_SLOT_DRIVER)
    {
        stack_size = (uint32)DRIVER_TASK_STACK_SIZE;
        priority   = (uint32)DRIVER_TASK_PRIORITY;
        period     = (uint32)DRIVER_TASK_PERIOD_MS;
    }
    else if(pt->slot_type[slot] == SVCRT_SLOT_APP)
    {
        stack_size = (uint32)APP_TASK_STACK_SIZE;
        priority   = (uint32)APP_TASK_PRIORITY;
        period     = (uint32)APP_TASK_PERIOD_MS;
    }
    else
    {
        return SVCRT_LOADER_ERR_PARAM;
    }

    if(svcrt_ptable_ram_info(slot, &ram_base, &ram_size) != 0)
    {
        return SVCRT_LOADER_ERR_STATE;
    }

    if((ram_base == 0u) || (ram_size < stack_size))
    {
        return SVCRT_LOADER_ERR_STATE;      /* 没有 RAM 块，或块放不下栈 */
    }

    /* 栈从本镜像自己的 RAM 块顶部切出：栈顶 = 块基址 + 块大小。
     * 镜像端 .sct 的 ARM_LIB_STACK 指向同一个地址（见 tools/gen_scatter.py），
     * 因此 __main 设好 SP 之后与内核推导值一致，不存在「双份栈」。 */
    stack_bottom = (ram_base + ram_size) - stack_size;

    task_id = svcrt_task_register((void (*)(void))entry,
                                  (uint32 *)stack_bottom,
                                  stack_size,
                                  (uint8)priority,
                                  period);
    if(task_id <= 0)
    {
        return SVCRT_LOADER_ERR_TASK;
    }

    /* 修正 TCB 的区域描述：任务的可访问范围是本镜像的 ROM 跨度与 RAM 块。
     * MPU 窗口由 svcrt_mpu_build_task 按这组值算（ROM 优先精确窗口）。 */
    svcrt_task_table[task_id - 1].ram_start = ram_base;
    svcrt_task_table[task_id - 1].ram_size  = ram_size;
    svcrt_task_table[task_id - 1].rom_start = pt->slot_base[slot];
    svcrt_task_table[task_id - 1].rom_size  = pt->slot_size[slot];

    svcrt_mpu_build_task(&svcrt_task_table[task_id - 1]);

    (void)svcrt_ptable_set_slot(slot, SVCRT_APP_SLOT_RUNNING, entry, (uint32)task_id);

    svcrt_sched_activate_higher((uint8)priority);

    return task_id;
}

/* Console-facing start: the same door as svcrt_loader_start(), with one
 * addition - a slot the kernel disabled for repeated faults is treated as a
 * human retry. The disable is cleared and the count restarts, but only if the
 * image still passes the same validation the boot scan uses; if it does not,
 * the slot stays disabled and the caller is told "state", not "started". A
 * retry that quietly re-enabled a corrupt image would be worse than the
 * disable it is undoing. */
int32 svcrt_loader_start_manual(uint32 slot)
{
    svcrt_partition_table_t *pt = svcrt_ptable_get();
    uint32 total = 0u;
    uint32 entry = 0u;
    uint32 autostart = 0u;

    if((slot >= pt->slot_max) || (slot >= SVCRT_SLOT_ARRAY_MAX))
    {
        return SVCRT_LOADER_ERR_PARAM;
    }

    if(svcrt_crash_disabled(slot) != 0u)
    {
        uint32 base = pt->slot_base[slot];

        if((base == 0u) || (svcrt_loader_accept(base, &total, &entry, &autostart) != 0))
        {
            SVCRT_LOGE("CRASH", "slot %u stays disabled: the image no longer"
                       " validates (reinstall it)", (unsigned)slot);
            return SVCRT_LOADER_ERR_STATE;
        }

        /* Valid image, explicit request: this is the operator starting over. */
        svcrt_crash_forget(slot);
        (void)svcrt_ptable_set_slot(slot, SVCRT_APP_SLOT_LOADED, entry, 0u);
        SVCRT_LOGI("CRASH", "slot %u re-armed by an explicit start (fault count cleared)",
                   (unsigned)slot);
    }

    return svcrt_loader_start(slot);
}

int32 svcrt_loader_stop(uint32 slot)
{
    svcrt_partition_table_t *pt = svcrt_ptable_get();
    uint32 entry = 0u;
    uint32 task_id = 0u;
    uint32 rest_state = SVCRT_APP_SLOT_LOADED;

    if((slot >= pt->slot_max) || (slot >= SVCRT_SLOT_ARRAY_MAX))
    {
        return SVCRT_LOADER_ERR_PARAM;
    }

    if(svcrt_ptable_get_slot(slot, 0, &entry, &task_id) != 0)
    {
        return SVCRT_LOADER_ERR_PARAM;
    }

    if((task_id == 0u) || (task_id > (uint32)svcrt_task_count))
    {
        return SVCRT_LOADER_ERR_STATE;
    }

    /* 停下来的裸镜像要退回 RAW，不能落成 LOADED：LOADED 在压实/回收眼里是
     * 「可搬移的安装镜像」，而裸镜像是开发者烧进固定槽位的，不能被搬。
     * 判据与认领时一致——基址处没有镜像头就是裸镜像。 */
    if((pt->slot_base[slot] == 0u) ||
       (*(const volatile uint32 *)(pt->slot_base[slot]) != (uint32)SVCRT_APP_MAGIC))
    {
        rest_state = SVCRT_APP_SLOT_RAW;
    }

    svcrt_loader_halt_image(task_id);

    (void)svcrt_ptable_set_slot(slot, rest_state, entry, 0u);

    SVCRT_SWITCH_TASK();

    return 0;
}

uint32 svcrt_loader_state(uint32 slot)
{
    svcrt_partition_table_t *pt = svcrt_ptable_get();

    if((slot >= pt->slot_max) || (slot >= SVCRT_SLOT_ARRAY_MAX))
    {
        return 0xffffffffu;
    }

    return pt->slot_state[slot];
}

/* 驱动专用包装：统一池里 App 与驱动同表，驱动接口只在入口处校验一次类型 */
int32 svcrt_loader_start_driver_slot(uint32 slot)
{
    svcrt_partition_table_t *pt = svcrt_ptable_get();

    if((slot >= pt->slot_max) || (slot >= SVCRT_SLOT_ARRAY_MAX) ||
       (pt->slot_type[slot] != SVCRT_SLOT_DRIVER))
    {
        return SVCRT_LOADER_ERR_PARAM;
    }

    return svcrt_loader_start(slot);
}

/* Driver-side twin of svcrt_loader_start_manual(): the type check belongs
 * to the driver door, the restart semantics belong to the manual one. */
int32 svcrt_loader_start_driver_manual(uint32 slot)
{
    svcrt_partition_table_t *pt = svcrt_ptable_get();

    if((slot >= pt->slot_max) || (slot >= SVCRT_SLOT_ARRAY_MAX) ||
       (pt->slot_type[slot] != SVCRT_SLOT_DRIVER))
    {
        return SVCRT_LOADER_ERR_PARAM;
    }

    return svcrt_loader_start_manual(slot);
}

int32 svcrt_loader_start_driver(void)
{
    return svcrt_loader_start_driver_slot(0u);
}

int32 svcrt_loader_stop_driver_slot(uint32 slot)
{
    svcrt_partition_table_t *pt = svcrt_ptable_get();

    if((slot >= pt->slot_max) || (slot >= SVCRT_SLOT_ARRAY_MAX) ||
       (pt->slot_type[slot] != SVCRT_SLOT_DRIVER))
    {
        return SVCRT_LOADER_ERR_PARAM;
    }

    return svcrt_loader_stop(slot);
}

uint32 svcrt_loader_state_driver(uint32 slot)
{
    svcrt_partition_table_t *pt = svcrt_ptable_get();

    if((slot >= pt->slot_max) || (slot >= SVCRT_SLOT_ARRAY_MAX) ||
       (pt->slot_type[slot] != SVCRT_SLOT_DRIVER))
    {
        return 0xffffffffu;
    }

    return pt->slot_state[slot];
}

int32 svcrt_loader_on_fault(int32 task_id)
{
    svcrt_partition_table_t *pt = svcrt_ptable_get();
    int32 slot;

    if(task_id <= 0)
    {
        return -1;
    }

    slot = svcrt_ptable_find_task((uint32)task_id);

    if(slot < 0)
    {
        return -1;              /* 内核任务：沿用默认故障处理 */
    }

    /* Counting and the disable decision live in the crash journal, not here.
     * The same rule has to hold for a fault that is followed by a reset (the
     * guard charges the slot right before it starves the watchdog, and then
     * only the journal is still around to carry the count), and two copies of
     * "how many crashes is too many" would eventually disagree. The partition
     * table keeps this power cycle's mirror of the count; the journal is the
     * copy that survives. */
    (void)svcrt_crash_fault((uint32)slot, SVCRT_CRASH_REASON_FAULT);

    if(svcrt_crash_disabled((uint32)slot) != 0u)
    {
        uint32 entry = 0u;

        /* 连续故障达到上限：禁用该镜像 —— 不再重启，并让它脱离调度。
         * 槽位置 INVALID（而非 EMPTY），以便上位机区分「被禁用的镜像」与
         * 「空槽位」；重新安装即可清零计数、重新启用。 */
        (void)svcrt_ptable_get_slot((uint32)slot, 0, &entry, 0);

        svcrt_loader_halt_image((uint32)task_id);

        (void)svcrt_ptable_set_slot((uint32)slot, SVCRT_APP_SLOT_INVALID, entry, 0u);

        /* 这个任务再也不会被调度：立刻撤掉它的 MPU 窗口，
         * 免得硬件里留着一份过期的上下文直到下次任务切换。 */
        svcrt_mpu_reset();

        svcrt_fault_record(SVCRT_FAULT_APPDISABLED, task_id);

        /* Name the slot and the count on the console as well: the fault ring
         * records the event, but "which slot, why, how many times" is what the
         * operator needs to decide whether to reinstall or to fix the image. */
        SVCRT_LOGE("CRASH", "slot %d disabled: %s, %u consecutive faults",
                   (int)slot,
                   svcrt_crash_reason_name(svcrt_crash_disabled((uint32)slot)),
                   (unsigned)pt->slot_crash_cnt[(uint32)slot]);

        return 1;
    }

    return 0;                   /* 未达上限：由调用方安排恢复（重启该镜像） */
}

uint32 svcrt_loader_start_autostart_driver(void)
{
    svcrt_partition_table_t *pt = svcrt_ptable_get();
    uint32 started = 0u;
    uint32 i;

    for(i = 0u; (i < pt->slot_max) && (i < SVCRT_SLOT_ARRAY_MAX); i++)
    {
        if((pt->slot_type[i] != SVCRT_SLOT_DRIVER) || (pt->slot_autostart[i] == 0u))
        {
            continue;
        }

        if(svcrt_loader_start(i) > 0)
        {
            started++;
        }
    }

    return started;
}

uint32 svcrt_loader_start_autostart(void)
{
    svcrt_partition_table_t *pt = svcrt_ptable_get();
    uint32 started = 0u;
    uint32 i;

    for(i = 0u; (i < pt->slot_max) && (i < SVCRT_SLOT_ARRAY_MAX); i++)
    {
        if((pt->slot_type[i] != SVCRT_SLOT_APP) || (pt->slot_autostart[i] == 0u))
        {
            continue;
        }

        if(svcrt_loader_start(i) > 0)
        {
            started++;
        }
    }

    return started;
}
