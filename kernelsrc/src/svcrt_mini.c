/**
* @file svcrt_mini.c
* @brief SVCrtOS 小程序（MiniApp）：从文件系统按需 load 进 RAM 执行
* @details 装载全程只有六步，每一步都在处理「RAM 是借来的、随时要还」这件事：
*            1. 读 256 字节镜像头 → 只校字段；
*            2. 读重定位表（<= 4KB）→ 再校一次表体；
*            3. 向 RAM 池借两块 pow2 的块：代码块 + （RW/ZI + 栈）块；
*            4. 负载「边读边落 RAM + 边打补丁」，同一份字节只过一遍
*               （头与表不落 RAM，也没有第二份中转缓冲）；
*            5. CRC32 复核（只算标称形态，与打包工具算的一致）；
*            6. 注册任务 → 回填 TCB 的代码/数据窗口为这两块 → 建 MPU 上下文 → 起调度。
*          退出/停止时把两块还回池：下一次运行重新从文件系统读一遍。
*          两块而不是一块的理由见 svcrt_mini.h：池的分配粒度是 2 的幂，把代码与
*          RAM 塞进同一个 pow2 块会让「代码远小于 RAM」的常见情形接近翻倍。
*
*          **同一时刻可以有多个**（数量编译期 SVCRT_MINI_MAX，运行期只能收紧）：
*          实例表 g_mini[] 一项一个小程序，每项自己一组块（借还都带 ptable 下标，
*          互不共用）。但**装载是串行的**——装载期间持调度器锁（文件系统只有一份
*          缓存，两个任务交错读镜像会把彼此的窗口顶偏），所以「多个」是同时**运行**
*          多个，不是同时装载多个；串行由 g_mini_busy 这道全局闸门保证。
*
*          崩溃禁用：与带槽位的 App 用**同一套**阈值（分区表 cfg_restart_max），
*          不新造数字。小程序没有槽位，身份改用**镜像路径的 FNV-1a 哈希**，
*          记录落在 svcrt_crash.c 的日记里（跨复位存活）。到上限后 svcrt_mini_run
*          直接回 SVCRT_MINI_ERR_DISABLED，直到 `mini forget <路径>` 放行或换镜像。
*
*          校验与重定位不在此另写一份：字段规则（svcrt_loader_check_header）、
*          MOVW/MOVT 的编码细节（svcrt_loader_reloc_apply）、头部 CRC 的算法
*          （svcrt_loader_crc_header）都只有 loader 那一份实现。两条装载路径
*          （串口→池、文件系统→RAM）解析的是同一种容器，判定必须一致。
*
* @author xw
* @date 2026.09.23
*/

#include "svcrt_mini.h"
#include "svcrt_config.h"
#include "svcrt_partition.h"    /* 内核专属：APP_TASK_* / SLOT_RAM_* 的唯一地址源头 */
#include "svcrt_app_image.h"
#include "svcrt_loader.h"
#include "svcrt_ptable.h"
#include "svcrt_fs.h"
#include "svcrt_task.h"
#include "svcrt_cfg.h"
#include "svcrt_mpu.h"
#include "svcrt_crash.h"
#include "svcrt_log.h"
#include "svcrt_spin.h"

#if (SVCRT_USE_MINIAPP == 1)

/* 负载一次读进来的字节数：与文件系统的单文件读缓存（SVCRT_FS_CACHE_SIZE）
 * 同宽，每段恰好喂满一次小缓存，不制造第二个中转缓冲。 */
#define SVCRT_MINI_CHUNK        (SVCRT_FS_CACHE_SIZE)

/* 实例状态。LOAD 有两层含义，都归在「已占项但不在跑」里：
 *   - 正在装载（已占项，尚未起任务）；
 *   - 正在拆除（停止/回收中，任务已被摘除、块待归还）。
 * 两者都不出现在 svcrt_mini_running() / info_at() / stop_at() 的「在跑」集合里，
 * 但都占着一个并发名额，所以别的装载不会挑中它。 */
#define SVCRT_MINI_ST_FREE      (0u)
#define SVCRT_MINI_ST_LOAD      (1u)
#define SVCRT_MINI_ST_RUN       (2u)

/** @brief 一个在跑（或在装载/拆除）的小程序 */
typedef struct {
    uint32 state;                       /* SVCRT_MINI_ST_x */
    uint32 pt_index;                    /* 共享表记账下标（state != FREE 时有效） */
    uint32 ram_block;                   /* RAM 块字节数（含栈）—— 故障归属的任务窗口匹配 */
    char   path[SVCRT_MINI_PATH_MAX];   /* 镜像路径：身份（哈希）与展示都用它 */
    svcrt_mini_info_t info;             /* 对外的状态快照（state == RUN 时有效） */
} svcrt_mini_slot_t;

static svcrt_mini_slot_t g_mini[SVCRT_MINI_MAX];
static uint32 g_mini_busy;              /* 1 = 有一次装载/拆除在进行（装载串行） */
static uint32 g_mini_max_count = SVCRT_MINI_MAX;
static svcrt_spinlock_t g_mini_lock = SVCRT_SPINLOCK_INIT;

/* 重定位表：ABI 的上限就是 SVCRT_APP_RELOC_MAX 条（4KB），一次读完再校验，
 * 不做滑动窗口。窗口能省的静态内存只有几 KB，却要让「一次 reloc_apply 会碰到
 * 多少条目」这类边界推理跟着负载分段走——省下的内存远小于推理出错的代价。
 * 这两块缓冲是**装载期**的（装载串行共用一份），与小程序的运行期占用无关。 */
static uint32 g_mini_reloc[SVCRT_APP_RELOC_MAX];

/* 镜像头：要当结构体用就必须 4 字节对齐，所以按 uint32 数组开 */
static uint32 g_mini_hdr[SVCRT_APP_HEADER_SIZE / 4u];

/* 单个小程序的体积上限（块的字节数）。默认取编译期 SLOT_RAM_MAX_BLOCK，运行期
 * 可以在 [SLOT_RAM_MIN_BLOCK, SLOT_RAM_MAX_BLOCK] 之间收紧（shell: mini limit）。
 * 只能收紧不能放宽的理由写在 svcrt_mini.h 的 svcrt_mini_max_bytes() 上：
 * SLOT_RAM_MAX_BLOCK 同时是池单块上限与 MPU 数据窗口上限。 */
static uint32 g_mini_max_block = SLOT_RAM_MAX_BLOCK;

/* 自启清单的两个缓冲：一份装「读到的原文」，一份装「重建后的新文本」。
 * 各 1KB，只在控制台/挂载这类非装载路径上用，不与装载期的 g_mini_hdr /
 * g_mini_reloc 混用（那两块在装载串行闸门内，语义是「正在装的这一份」）。 */
static uint8  g_mini_auto_in[SVCRT_MINI_AUTOSTART_MAX];
static uint8  g_mini_auto_out[SVCRT_MINI_AUTOSTART_MAX];

/* 本次开机是否已经做过自启。先置位再干活：启动过程中若有东西又把卷挂一遍，
 * 不会递归回来再启动一轮。 */
static uint32 g_mini_auto_boot_done;

/* 不小于 v 的最小 2 的幂。只在本模块用，所以不去动公共头。 */
static uint32 svcrt_mini_pow2_ceil(uint32 v)
{
    uint32 r = 1u;

    while(r < v)
    {
        r <<= 1u;
    }

    return r;
}

/* 清空一个实例。不用 memset，避免为一个结构体再引一层依赖。 */
static void svcrt_mini_slot_clear(svcrt_mini_slot_t *s)
{
    uint32 i;

    s->state     = SVCRT_MINI_ST_FREE;
    s->pt_index  = 0u;
    s->ram_block = 0u;

    for(i = 0u; i < (uint32)SVCRT_MINI_PATH_MAX; i++)
    {
        s->path[i] = '\0';
    }

    s->info.task_id    = 0u;
    s->info.code_base  = 0u;
    s->info.code_block = 0u;
    s->info.code_size  = 0u;
    s->info.ram_base   = 0u;
    s->info.ram_size   = 0u;
    s->info.entry      = 0u;
    s->info.crc        = 0u;
}

/* 把 path 抄进实例（长度已在 svcrt_mini_run 入口限过） */
static void svcrt_mini_slot_set_path(svcrt_mini_slot_t *s, const char *path)
{
    uint32 i;

    for(i = 0u; (i + 1u) < (uint32)SVCRT_MINI_PATH_MAX; i++)
    {
        s->path[i] = path[i];
        if(path[i] == '\0')
        {
            return;
        }
    }
    s->path[SVCRT_MINI_PATH_MAX - 1u] = '\0';
}

/* 取第 index 个「在跑」的实例（密集编号，跳过空项 / 装载中 / 拆除中）。
 * 调用者必须持 g_mini_lock。 */
static svcrt_mini_slot_t *svcrt_mini_nth_running(uint32 index)
{
    uint32 seen = 0u;
    uint32 i;

    for(i = 0u; i < (uint32)SVCRT_MINI_MAX; i++)
    {
        if(g_mini[i].state == SVCRT_MINI_ST_RUN)
        {
            if(seen == index)
            {
                return &g_mini[i];
            }
            seen++;
        }
    }

    return 0;
}

/* 从镜像文件里精确读 len 字节。少读一个字节就算失败：短文件就是短文件，
 * 不能靠块里的残留内容把一次装载"补全"。 */
static int32 svcrt_mini_read_exact(const char *path, uint8 *buf, uint32 len, uint32 off)
{
    uint32 got = 0u;

    if(len == 0u)
    {
        return 0;
    }

    if(svcrt_fs_read_at(path, buf, len, off, &got) != 0)
    {
        return -1;
    }

    return (got == len) ? 0 : -1;
}

int32 svcrt_mini_run(const char *path)
{
    svcrt_app_header_t *p_hdr = (svcrt_app_header_t *)(void *)g_mini_hdr;
    svcrt_mini_slot_t *slot = 0;
    uint32 irq;
    uint32 path_len = 0u;
    uint32 hash;
    uint32 in_use = 0u;
    uint32 code_size;
    uint32 code_span;
    uint32 ram_size_hdr;
    uint32 code_base = 0u;
    uint32 code_block = 0u;
    uint32 code_want;
    uint32 ram_base = 0u;
    uint32 ram_block = 0u;
    uint32 pt_index = 0u;
    uint32 max_block = 0u;
    uint32 entry;
    uint32 total;
    uint32 cur;
    uint32 crc_done;
    uint32 idx = 0u;
    uint32 crc = 0u;
    uint32 delta_rom;
    uint32 delta_ram;
    uint32 i;
    int32  free_i = -1;
    int32  ret;
    int32  task_id;

    if(path == 0)
    {
        return SVCRT_LOADER_ERR_PARAM;
    }

    /* 路径长度先量出来：实例表按 SVCRT_MINI_PATH_MAX 存路径，超了就不能
     * 悄悄截断——截断会让两个不同的路径撞成同一个身份。 */
    while((path_len < (uint32)SVCRT_MINI_PATH_MAX) && (path[path_len] != '\0'))
    {
        path_len++;
    }
    if((path_len == 0u) || (path_len >= (uint32)SVCRT_MINI_PATH_MAX))
    {
        return SVCRT_LOADER_ERR_PARAM;
    }

    hash = svcrt_crash_hash_path(path);

    /* 先占位：装载期间项就要被占住，而不是等文件读完才发现没名额——那时
     * 两块 RAM 已经借出去了。同时把「装载串行」和「并发数上限」一次判掉。 */
    svcrt_spin_lock_irqsave(&g_mini_lock, &irq);

    if(svcrt_crash_mini_disabled(hash) != 0u)
    {
        svcrt_spin_unlock_irqrestore(&g_mini_lock, irq);
        return SVCRT_MINI_ERR_DISABLED;     /* 连续故障到上限，已被禁用 */
    }

    if(g_mini_busy != 0u)
    {
        svcrt_spin_unlock_irqrestore(&g_mini_lock, irq);
        return SVCRT_LOADER_ERR_BUSY;       /* 另一次装载/拆除还在进行 */
    }

    for(i = 0u; i < (uint32)SVCRT_MINI_MAX; i++)
    {
        if(g_mini[i].state != SVCRT_MINI_ST_FREE)
        {
            in_use++;
        }
        else if(free_i < 0)
        {
            free_i = (int32)i;
        }
    }

    /* 没有空项，或已经到运行期并发上限：都是 BUSY，不挤掉任何一个在跑的。 */
    if((free_i < 0) || (in_use >= g_mini_max_count))
    {
        svcrt_spin_unlock_irqrestore(&g_mini_lock, irq);
        return SVCRT_LOADER_ERR_BUSY;
    }

    slot = &g_mini[free_i];
    svcrt_mini_slot_clear(slot);
    slot->state = SVCRT_MINI_ST_LOAD;
    svcrt_mini_slot_set_path(slot, path);
    max_block  = g_mini_max_block;          /* 本次装载用哪条上限，此刻定下来 */
    g_mini_busy = 1u;

    svcrt_spin_unlock_irqrestore(&g_mini_lock, irq);

    if(svcrt_fs_mounted() == 0u)
    {
        ret = SVCRT_MINI_ERR_FS;
        goto out;
    }

    /* 文件系统只有一份缓存，读镜像的整个过程不能与别的任务的读写交错：
     * 与 SVC 层访问文件系统用同一套约定（调度器锁）。 */
    svcrt_sched_lock_internal();

    if(svcrt_mini_read_exact(path, (uint8 *)(void *)g_mini_hdr,
                             SVCRT_APP_HEADER_SIZE, 0u) != 0)
    {
        ret = SVCRT_MINI_ERR_FS;
        goto unlock_out;
    }

    /* 先只校字段：此刻重定位表还没读进来，p_rel 传 0。
     * 掩码传 MASK_MINI：池安装器拒收的 type，小程序路径也只认小程序，两个
     * 方向都不放宽——让"能装进池"与"能当小程序跑"是同一套类型判定。 */
    ret = svcrt_loader_check_header(p_hdr, 0, SVCRT_APP_TYPE_MASK_MINI);
    if(ret != 0)
    {
        goto unlock_out;
    }

    if(svcrt_mini_read_exact(path, (uint8 *)(void *)g_mini_reloc,
                             p_hdr->reloc_count * SVCRT_APP_RELOC_SIZE,
                             SVCRT_APP_RELOC_OFFSET) != 0)
    {
        ret = SVCRT_MINI_ERR_FS;
        goto unlock_out;
    }

    /* 表体到手再校一次：字段自洽不等于表合法。越界/乱序/未对齐的表会让
     * 后面的打补丁静默漏改，必须在这里挡掉，不能带着它去执行。 */
    ret = svcrt_loader_check_header(p_hdr, g_mini_reloc, SVCRT_APP_TYPE_MASK_MINI);
    if(ret != 0)
    {
        goto unlock_out;
    }

    code_size    = p_hdr->image_size;
    code_span    = (code_size + 7u) & ~7u;      /* 负载按 8 字节上取整 */
    ram_size_hdr = p_hdr->ram_size;
    code_want    = svcrt_mini_pow2_ceil(code_span);

    /* 回绕自证：image_size 与 ram_size 各自都在 check_header 里被限过，按说不该
     * 回绕；但「不该」不是证据。pow2_ceil 的结果若比负载还小，只可能是加法回绕
     * ——那时块会小于负载，后面的分段读取会写到块外。宁可在这里报 SIZE。 */
    if(code_want < code_span)
    {
        ret = SVCRT_LOADER_ERR_SIZE;
        goto unlock_out;
    }

    /* 代码块与 RAM 块各自取 2 的幂（池的分配粒度），各自超过上限就明确报错：
     * 不截断、不缩 RAM 区——悄悄缩掉的那块 RAM 正是小程序自己的栈；把代码块
     * 截到负载以下则会让分段读取写到块外。上限是运行期可配的
     * （默认 = 编译期 SLOT_RAM_MAX_BLOCK，见 g_mini_max_block）。 */
    if((code_want > max_block) || (ram_size_hdr > max_block) ||
       (ram_size_hdr < APP_TASK_STACK_SIZE))
    {
        ret = SVCRT_LOADER_ERR_SIZE;
        goto unlock_out;
    }

    /* 两块一起借（同一把锁、同生同死）：借代码块、再借 RAM 块，任一步失败都
     * 不做「半借」——归还路径只认一种状态，少一种状态就少一类现场查不出来的错。
     * 归还凭据是 ptable 交回来的下标，多实例下每项各记一组。 */
    if(svcrt_ptable_mini_alloc(code_span, ram_size_hdr, &pt_index,
                               &code_base, &code_block,
                               &ram_base, &ram_block) != 0)
    {
        ret = SVCRT_LOADER_ERR_NOSPACE;
        goto unlock_out;
    }

    /* 增量就是「运行时地址 − 标称链接地址」：负载落在代码块基址，RAM 区就是
     * RAM 块本身。两个 delta 里的 payload_offset 都已抵消——标称链接基址
     * nominal_base 就是负载自己的链接基址（pack_app 写的是 payload_link_base），
     * 而 nominal_ram_base 就是镜像链接 RW/ZI 时用的 RAM 起点。 */
    delta_rom = code_base - p_hdr->nominal_base;
    delta_ram = ram_base - p_hdr->nominal_ram_base;

    crc = svcrt_loader_crc_header(p_hdr);
    if(p_hdr->reloc_count != 0u)
    {
        crc = svcrt_crc32((const void *)g_mini_reloc,
                          p_hdr->reloc_count * SVCRT_APP_RELOC_SIZE, crc);
    }

    total    = SVCRT_APP_TOTAL_LEN(p_hdr);
    cur      = p_hdr->payload_offset;
    crc_done = cur;

    while(cur < total)
    {
        uint32 n = total - cur;
        uint32 submit;
        uint32 next;

        if(n > SVCRT_MINI_CHUNK)
        {
            n = SVCRT_MINI_CHUNK;
        }

        /* 负载直接读进它在块里的最终位置（镜像偏移 X 的字节 = 块地址
         * code_base + (X - payload_offset)），因此没有中转缓冲。 */
        if(svcrt_mini_read_exact(path,
                                 (uint8 *)(void *)(code_base + (cur - p_hdr->payload_offset)),
                                 n, cur) != 0)
        {
            ret = SVCRT_MINI_ERR_FS;
            goto free_out;
        }

        /* CRC 只算「标称形态」（打包工具算的那个）：一段字节刚读进来、还没打
         * 补丁时算一次。段尾被切开时下一轮会回退重读几个字节，那部分已经算过，
         * 不能重复计入，否则 CRC 必然对不上。 */
        if((cur + n) > crc_done)
        {
            uint32 from = (cur > crc_done) ? cur : crc_done;

            crc = svcrt_crc32((const void *)(code_base + (from - p_hdr->payload_offset)),
                              (cur + n) - from, crc);
            crc_done = cur + n;
        }

        /* 补丁缓冲的基点要满足「镜像偏移 X 的字节在 buf[X - buf_off]」 */
        submit = svcrt_loader_reloc_apply((uint8 *)(void *)(code_base + (cur - p_hdr->payload_offset)),
                                          cur, n, p_hdr, g_mini_reloc, &idx,
                                          delta_rom, delta_ram);

        next = (submit < (cur + n)) ? submit : (cur + n);

        if(next <= cur)
        {
            /* 表体校验保证每条表项完整落在负载内，正常走不到这里。真到了就报错：
             * 带着"少打了一次补丁"的镜像去执行，比装载失败危险得多。 */
            ret = SVCRT_LOADER_ERR_RELOC;
            goto free_out;
        }

        /* 段尾切开了一条 8 字节指令类表项：回退到该表项起点重读一次（<= 8 字节），
         * 让它在这一轮里完整地被打上补丁。块里尾部那几个字节此刻还是标称形态，
         * 重读覆盖它没有副作用。 */
        cur = next;
    }

    if(crc != p_hdr->crc32)
    {
        ret = SVCRT_LOADER_ERR_CRC;
        goto free_out;
    }

    entry = code_base + p_hdr->entry_offset;    /* 入口偏移是相对「负载起始」的 */
    #if (SVCRT_ARCH_IS_ARM == 1)
    entry |= 1u;                            /* Cortex-M：Thumb 指令集标志位 */
    #endif

    /* 调度器锁必须在起任务之前放开：svcrt_sched_activate_higher() 在锁内只
     * 记账不切换，本函数返回后要等谁解锁就说不准了。 */
    svcrt_sched_unlock_internal();

    task_id = svcrt_task_register((void (*)(void))entry,
                                  (uint32 *)(void *)(ram_base + ram_block - APP_TASK_STACK_SIZE),
                                  (uint32)APP_TASK_STACK_SIZE,
                                  (uint8)APP_TASK_PRIORITY,
                                  (uint32)APP_TASK_PERIOD_MS);
    if(task_id <= 0)
    {
        (void)svcrt_ptable_mini_free(pt_index);     /* 任务表满了：两块还在我们手里 */
        ret = SVCRT_LOADER_ERR_TASK;
        goto out;
    }

    /* TCB 的区域描述就是这两块：rom_* = 代码块（在池里 -> 代码窗口用可执行的
     * RAMX 属性），ram_* = RAM 块（RW/ZI + 栈，走常规数据窗口，不可执行）。
     * 两块基址不同、大小不同，因此 region0 / region1 是两个独立的精确窗口，
     * 不需要「两个窗口必须同属性」那种妥协（见 svcrt_mpu.c）。 */
    svcrt_task_table[task_id - 1].rom_start = code_base;
    svcrt_task_table[task_id - 1].rom_size  = code_block;
    svcrt_task_table[task_id - 1].ram_start = ram_base;
    svcrt_task_table[task_id - 1].ram_size  = ram_block;
    svcrt_task_table[task_id - 1].is_priv   = 0u;   /* 与 App 同级：非特权 */

    svcrt_mpu_build_task(&svcrt_task_table[task_id - 1]);

    svcrt_spin_lock_irqsave(&g_mini_lock, &irq);
    slot->state     = SVCRT_MINI_ST_RUN;
    slot->pt_index  = pt_index;
    slot->ram_block = ram_block;
    slot->info.task_id    = (uint32)task_id;
    slot->info.code_base  = code_base;
    slot->info.code_block = code_block;
    slot->info.code_size  = code_size;
    slot->info.ram_base   = ram_base;
    slot->info.ram_size   = ram_size_hdr;
    slot->info.entry      = entry;
    slot->info.crc        = crc;
    g_mini_busy = 0u;
    svcrt_spin_unlock_irqrestore(&g_mini_lock, irq);

    SVCRT_LOGI("MINI", "%s: %u B payload -> code 0x%08X (%u B block), RW/stack 0x%08X (%u B), task %d",
               path, (unsigned)code_size, (unsigned)code_base, (unsigned)code_block,
               (unsigned)ram_base, (unsigned)ram_size_hdr, (int)task_id);

    svcrt_sched_activate_higher((uint8)APP_TASK_PRIORITY);
    return 0;

free_out:
    (void)svcrt_ptable_mini_free(pt_index);
unlock_out:
    svcrt_sched_unlock_internal();
out:
    {
        uint32 irq2;

        svcrt_spin_lock_irqsave(&g_mini_lock, &irq2);
        if(slot != 0)
        {
            svcrt_mini_slot_clear(slot);
        }
        g_mini_busy = 0u;
        svcrt_spin_unlock_irqrestore(&g_mini_lock, irq2);
    }
    return ret;
}

/* 停掉一个实例：先把镜像（含它自建的线程）踢出调度，再还块。顺序不能反——
 * 还完块，那块 RAM 可能立刻被下一个装载者拿去写代码，而原任务还在跑。
 * 不持 g_mini_lock（会做 halt_image / ptable_free）。 */
static void svcrt_mini_teardown(svcrt_mini_slot_t *slot)
{
    uint32 task_id;

    task_id = slot->info.task_id;

    if(task_id != 0u)
    {
        svcrt_loader_halt_image(task_id);
    }

    (void)svcrt_ptable_mini_free(slot->pt_index);
}

int32 svcrt_mini_stop_at(uint32 index)
{
    uint32 irq;
    svcrt_mini_slot_t *slot;

    svcrt_spin_lock_irqsave(&g_mini_lock, &irq);
    slot = svcrt_mini_nth_running(index);
    if(slot == 0)
    {
        svcrt_spin_unlock_irqrestore(&g_mini_lock, irq);
        return SVCRT_LOADER_ERR_STATE;
    }
    /* 立刻离开「在跑」集合：并发进来的 stop / run 都不会再选中它 */
    slot->state = SVCRT_MINI_ST_LOAD;
    svcrt_spin_unlock_irqrestore(&g_mini_lock, irq);

    svcrt_mini_teardown(slot);

    svcrt_spin_lock_irqsave(&g_mini_lock, &irq);
    {
        uint32 task_id = slot->info.task_id;

        svcrt_mini_slot_clear(slot);
        SVCRT_LOGI("MINI", "stopped: task %u, code and RAM blocks returned to the pool",
                   (unsigned)task_id);
    }
    svcrt_spin_unlock_irqrestore(&g_mini_lock, irq);

    return 0;
}

int32 svcrt_mini_stop(void)
{
    int32 stopped = 0;

    /* 逐个停：每次都停「当前第 0 个」，停掉后原来的第 1 个就变成第 0 个。
     * 上限用 SVCRT_MINI_MAX 兜底，避免状态异常时在这里空转。 */
    while((svcrt_mini_running() > 0u) && (stopped < (int32)SVCRT_MINI_MAX))
    {
        if(svcrt_mini_stop_at(0u) != 0)
        {
            break;
        }
        stopped++;
    }

    return (stopped > 0) ? 0 : SVCRT_LOADER_ERR_STATE;
}

uint32 svcrt_mini_running(void)
{
    uint32 irq;
    uint32 n = 0u;
    uint32 i;

    svcrt_spin_lock_irqsave(&g_mini_lock, &irq);
    for(i = 0u; i < (uint32)SVCRT_MINI_MAX; i++)
    {
        if(g_mini[i].state == SVCRT_MINI_ST_RUN)
        {
            n++;
        }
    }
    svcrt_spin_unlock_irqrestore(&g_mini_lock, irq);

    return n;
}

int32 svcrt_mini_info_at(uint32 index, svcrt_mini_info_t *p_out)
{
    uint32 irq;
    svcrt_mini_slot_t *slot;

    if(p_out == 0)
    {
        return -1;
    }

    svcrt_spin_lock_irqsave(&g_mini_lock, &irq);
    slot = svcrt_mini_nth_running(index);
    if(slot == 0)
    {
        svcrt_spin_unlock_irqrestore(&g_mini_lock, irq);
        return -1;
    }
    *p_out = slot->info;
    svcrt_spin_unlock_irqrestore(&g_mini_lock, irq);

    return 0;
}

int32 svcrt_mini_info(svcrt_mini_info_t *p_out)
{
    if(p_out == 0)
    {
        return -1;
    }

    if(svcrt_mini_info_at(0u, p_out) != 0)
    {
        return -1;
    }

    return 0;
}

uint32 svcrt_mini_max_count(void)
{
    uint32 irq;
    uint32 v;

    svcrt_spin_lock_irqsave(&g_mini_lock, &irq);
    v = g_mini_max_count;
    svcrt_spin_unlock_irqrestore(&g_mini_lock, irq);

    return v;
}

int32 svcrt_mini_set_max_count(uint32 n)
{
    uint32 irq;

    /* 超范围一律报 PARAM：不夹取。「设了 8 却只跑 2 个」是看不出来的谎话，
     * 上限本来就是为了「说不清就拒绝」才存在的。 */
    if((n < 1u) || (n > (uint32)SVCRT_MINI_MAX))
    {
        return SVCRT_LOADER_ERR_PARAM;
    }

    svcrt_spin_lock_irqsave(&g_mini_lock, &irq);
    g_mini_max_count = n;
    svcrt_spin_unlock_irqrestore(&g_mini_lock, irq);

    return 0;
}

uint32 svcrt_mini_max_bytes(void)
{
    uint32 irq;
    uint32 v;

    svcrt_spin_lock_irqsave(&g_mini_lock, &irq);
    v = g_mini_max_block;
    svcrt_spin_unlock_irqrestore(&g_mini_lock, irq);

    return v;
}

int32 svcrt_mini_set_max_bytes(uint32 bytes)
{
    uint32 irq;

    if(bytes == 0u)
    {
        bytes = SLOT_RAM_MAX_BLOCK;         /* 0 = 恢复编译期默认 */
    }

    /* 不是 2 的幂 / 越界一律报 PARAM：不夹取。夹取会让「我设了 64K 却拿到 32K」
     * 变成一句看不出来的谎话——上限本来就是为了「说不清就拒绝」才存在的。 */
    if((bytes < SLOT_RAM_MIN_BLOCK) || (bytes > SLOT_RAM_MAX_BLOCK) ||
       ((bytes & (bytes - 1u)) != 0u))
    {
        return SVCRT_LOADER_ERR_PARAM;
    }

    svcrt_spin_lock_irqsave(&g_mini_lock, &irq);
    g_mini_max_block = bytes;
    svcrt_spin_unlock_irqrestore(&g_mini_lock, irq);

    return 0;
}

int32 svcrt_mini_forget(const char *path)
{
    if((path == 0) || (path[0] == '\0'))
    {
        return -1;
    }

    svcrt_crash_mini_forget(svcrt_crash_hash_path(path));

    return 0;
}

int32 svcrt_mini_on_task_exit(uint32 task_id)
{
    uint32 irq;
    uint32 i;
    uint32 task_id_saved;
    svcrt_mini_slot_t *slot = 0;

    if(task_id == 0u)
    {
        return 0;
    }

    svcrt_spin_lock_irqsave(&g_mini_lock, &irq);
    for(i = 0u; i < (uint32)SVCRT_MINI_MAX; i++)
    {
        /* 只认主任务：子线程退出不动块——小程序可能只是收掉了一个 worker，
         * 主任务还要继续跑。 */
        if((g_mini[i].state == SVCRT_MINI_ST_RUN) && (g_mini[i].info.task_id == task_id))
        {
            slot = &g_mini[i];
            break;
        }
    }
    if(slot == 0)
    {
        /* 不是小程序的主任务（内核任务、App 任务，或小程序内部的子线程） */
        svcrt_spin_unlock_irqrestore(&g_mini_lock, irq);
        return 0;
    }
    task_id_saved = slot->info.task_id;
    slot->state = SVCRT_MINI_ST_LOAD;       /* 离开「在跑」集合 */
    svcrt_spin_unlock_irqrestore(&g_mini_lock, irq);

    /* 主任务退出 = 整个小程序结束：它自己创建的线程也必须一起停，停完才还块。
     * 此刻调用者自身已被置为 INVALID，因此 halt_image 不会重复收尸它。 */
    svcrt_mini_teardown(slot);

    svcrt_spin_lock_irqsave(&g_mini_lock, &irq);
    svcrt_mini_slot_clear(slot);
    svcrt_spin_unlock_irqrestore(&g_mini_lock, irq);

    SVCRT_LOGI("MINI", "task %u returned: MiniApp finished, code and RAM blocks back in the pool",
               (unsigned)task_id_saved);
    return 1;
}

int32 svcrt_mini_on_fault(int32 task_id)
{
    uint32 irq;
    uint32 i;
    uint32 hash;
    uint32 disabled;
    uint32 task_id_saved;
    svcrt_mini_slot_t *slot = 0;

    if((task_id <= 0) || ((uint32)task_id > (uint32)svcrt_task_count))
    {
        return 0;
    }

    /* 归属判定用 RAM 窗口：小程序的子线程也跑在这块 RAM 里，任何一个线程的
     * 故障都记在整个小程序名下——与 svcrt_loader_halt_image 停整个窗口是
     * 同一套身份。内核任务的窗口在别处，永不匹配。 */
    svcrt_spin_lock_irqsave(&g_mini_lock, &irq);
    for(i = 0u; i < (uint32)SVCRT_MINI_MAX; i++)
    {
        uint32 rb;
        uint32 re;
        uint32 tb;
        uint32 te;

        if(g_mini[i].state != SVCRT_MINI_ST_RUN)
        {
            continue;
        }

        rb = g_mini[i].info.ram_base;
        re = rb + g_mini[i].ram_block;
        tb = svcrt_task_table[(uint32)task_id - 1u].ram_start;
        te = tb + svcrt_task_table[(uint32)task_id - 1u].ram_size;

        if((rb != 0u) && (re > rb) && (tb >= rb) && (te <= re))
        {
            slot = &g_mini[i];
            break;
        }
    }
    if(slot == 0)
    {
        svcrt_spin_unlock_irqrestore(&g_mini_lock, irq);
        return 0;                   /* 与本模块无关：调用方按默认路径处理 */
    }

    hash = svcrt_crash_hash_path(slot->path);
    task_id_saved = slot->info.task_id;
    svcrt_spin_unlock_irqrestore(&g_mini_lock, irq);

    /* 与带槽位的 App 同一套阈值（分区表 cfg_restart_max）：计数落在跨复位的
     * 日记里。这一步可能从异常上下文调用，只用临界区，不做文件 I/O。 */
    (void)svcrt_crash_mini_fault(hash, SVCRT_CRASH_REASON_FAULT);

    disabled = svcrt_crash_mini_disabled(hash);
    if(disabled == 0u)
    {
        return 0;                   /* 未到上限：交给默认故障路径（原地重启） */
    }

    /* 到上限：不再让它跑。停掉整个小程序、归还两块，并记一条禁用。
     * 此后 svcrt_mini_run 对同一路径（同一哈希）直接回 DISABLED。 */
    SVCRT_LOGE("MINI", "disabled: %s, %u consecutive faults (retry with 'mini forget %s')",
               svcrt_crash_reason_name(disabled),
               (unsigned)svcrt_crash_mini_count(hash), slot->path);

    svcrt_spin_lock_irqsave(&g_mini_lock, &irq);
    if(slot->state == SVCRT_MINI_ST_RUN)
    {
        slot->state = SVCRT_MINI_ST_LOAD;       /* 离开「在跑」集合 */
    }
    svcrt_spin_unlock_irqrestore(&g_mini_lock, irq);

    svcrt_mini_teardown(slot);

    /* 这个任务再也不会被调度：立刻撤掉它的 MPU 窗口，免得硬件里留着一份
     * 过期的上下文直到下次任务切换（与带槽位的禁用路径同一处理）。 */
    svcrt_mpu_reset();

    svcrt_spin_lock_irqsave(&g_mini_lock, &irq);
    svcrt_mini_slot_clear(slot);
    svcrt_spin_unlock_irqrestore(&g_mini_lock, irq);

    (void)task_id_saved;
    return 1;                       /* 已按小程序规则处理：不要恢复它 */
}

/* ============================================================================
 * 开机自启清单（卷内文本文件 SVCRT_MINI_AUTOSTART_FILE）
 *
 * 解析只写一份：svcrt_mini_auto_next() 是「什么算一个有效条目」的唯一判据
 * （行尾分隔、行内去首尾空白、空行跳过、'#' 注释），遍历、比对、重建都从它取
 * 行区间。两份解析迟早会分歧，而分歧的表现是「清单里明明有、却没启起来」。
 * ==========================================================================*/

/* 读清单到 g_mini_auto_in。冷启动时文件不存在是正常状态（= 一条自启都没有），
 * 但那与「存在却读不了」不是同一件事，所以返回值两者都报 -1、由调用方决定怎么措辞。 */
static int32 svcrt_mini_auto_read(uint32 *out_len)
{
    uint32 len = 0u;

    *out_len      = 0u;
    g_mini_auto_in[0] = '\0';

    if(svcrt_fs_read_file(SVCRT_MINI_AUTOSTART_FILE, g_mini_auto_in,
                          (uint32)SVCRT_MINI_AUTOSTART_MAX - 1u, &len) != 0)
    {
        return -1;
    }

    g_mini_auto_in[len] = '\0';
    *out_len = len;

    return 0;
}

/* 从「读到的原文」里取下一条有效条目。cursor 是字节游标（调用方从 0 开始）。
 * 返回 1=取到（[s,e) 是去空白后的区间），0=没有更多。 */
static uint32 svcrt_mini_auto_next(uint32 *cursor, uint32 len, uint32 *s, uint32 *e)
{
    uint32 i = *cursor;

    while(i < len)
    {
        uint32 a = i;
        uint32 b;

        while((i < len) && (g_mini_auto_in[i] != '\n'))
        {
            i++;                        /* a..i 是一整行 */
        }
        b = i;
        if(i < len)
        {
            i++;                        /* 吃掉 '\n' */
        }
        *cursor = i;

        /* 行内去首尾空白。'\r' 也算空白：清单文件可能被 Windows 侧编辑器
         * 存成 CRLF，那样的行尾不该被当成路径的一部分。 */
        while((a < b) && ((g_mini_auto_in[a] == ' ') ||
                          (g_mini_auto_in[a] == '\t') ||
                          (g_mini_auto_in[a] == '\r')))
        {
            a++;
        }
        while((b > a) && ((g_mini_auto_in[b - 1u] == ' ') ||
                          (g_mini_auto_in[b - 1u] == '\t') ||
                          (g_mini_auto_in[b - 1u] == '\r')))
        {
            b--;
        }

        if(b <= a)
        {
            continue;                   /* 空行 */
        }
        if(g_mini_auto_in[a] == '#')
        {
            continue;                   /* 注释 */
        }

        *s = a;
        *e = b;

        return 1u;
    }

    return 0u;
}

/* [s,e) 是否就是 path（逐字节比，两边都要求刚好结束）。 */
static uint32 svcrt_mini_auto_line_is(uint32 s, uint32 e, const char *path)
{
    uint32 n = e - s;
    uint32 k;

    for(k = 0u; k < n; k++)
    {
        if(path[k] == '\0')
        {
            return 0u;
        }
        if((char)g_mini_auto_in[s + k] != path[k])
        {
            return 0u;
        }
    }

    return (path[n] == '\0') ? 1u : 0u;
}

/* 由遍历统一回调。超长条目在这里被拦下：不截断、也不回调，只报一条错误日志。 */
static int32 svcrt_mini_auto_walk(svcrt_mini_auto_cb_t cb, void *arg)
{
    char   path[SVCRT_MINI_PATH_MAX];
    uint32 len = 0u;
    uint32 cur = 0u;
    uint32 s   = 0u;
    uint32 e   = 0u;

    if(cb == 0)
    {
        return -1;
    }

    if(svcrt_mini_auto_read(&len) != 0)
    {
        return -1;
    }

    while(svcrt_mini_auto_next(&cur, len, &s, &e) != 0u)
    {
        uint32 n = e - s;
        uint32 k;

        if(n > ((uint32)SVCRT_MINI_PATH_MAX - 1u))
        {
            /* 截断会让两个不同路径共享同一个身份（崩溃记账会串号），
             * 静默丢一条会让「清单里写了却没启起来」无从解释。都不做。 */
            SVCRT_LOGE("MINI", "%s: entry longer than %u bytes, skipped",
                       SVCRT_MINI_AUTOSTART_FILE,
                       (unsigned)(SVCRT_MINI_PATH_MAX - 1u));
            continue;
        }

        for(k = 0u; k < n; k++)
        {
            path[k] = (char)g_mini_auto_in[s + k];
        }
        path[n] = '\0';

        if(cb(path, arg) != 0)
        {
            break;
        }
    }

    return 0;
}

int32 svcrt_mini_autostart_foreach(svcrt_mini_auto_cb_t cb, void *arg)
{
    return svcrt_mini_auto_walk(cb, arg);
}

/* 启动一条：判据全部复用 svcrt_mini_run（并发上限、崩溃禁用、体积上限都不另写），
 * 成功与否逐条报出来——自启是「没人看着的时候发生的事」，日志就是它唯一的交代。 */
static int svcrt_mini_auto_start_one(const char *path, void *arg)
{
    uint32 *started = (uint32 *)arg;
    int32   rc      = svcrt_mini_run(path);

    if(rc == 0)
    {
        (*started)++;
        SVCRT_LOGI("MINI", "autostart: %s started", path);
    }
    else
    {
        SVCRT_LOGE("MINI", "autostart: %s not started, rc=%d", path, (int)rc);
    }

    return 0;
}

int32 svcrt_mini_autostart_now(void)
{
    uint32 started = 0u;

    if(svcrt_mini_auto_walk(svcrt_mini_auto_start_one, &started) != 0)
    {
        return SVCRT_MINI_ERR_FS;
    }

    return (int32)started;
}

uint32 svcrt_mini_boot_autostart(void)
{
    int32 rc;

    if(g_mini_auto_boot_done != 0u)
    {
        return 0u;
    }
    g_mini_auto_boot_done = 1u;         /* 先置位：启动过程中再次挂载不会递归 */

    rc = svcrt_mini_autostart_now();
    if(rc < 0)
    {
        /* 「没有清单」是启动的常见状态（开机不启动才是默认），用 info 而不是
         * error：它不是故障，只是没配。 */
        SVCRT_LOGI("MINI", "no autostart list (%s)", SVCRT_MINI_AUTOSTART_FILE);
        return 0u;
    }

    SVCRT_LOGI("MINI", "boot autostart: %d started from %s",
               (int)rc, SVCRT_MINI_AUTOSTART_FILE);

    return (uint32)rc;
}

int32 svcrt_mini_autostart_set(const char *path, uint32 on)
{
    uint32 len    = 0u;
    uint32 cur    = 0u;
    uint32 s      = 0u;
    uint32 e      = 0u;
    uint32 o      = 0u;
    uint32 plen   = 0u;
    uint32 listed = 0u;
    uint32 k;

    if((path == 0) || (path[0] != '/'))
    {
        /* 与 `mini run` 必须是同一种写法：带不带 '/' 是两条不同的身份串，
         * 这里自作主张补一个，会让清单里的路径与手工运行的那条哈希不一致。 */
        return SVCRT_LOADER_ERR_PARAM;
    }

    while(path[plen] != '\0')
    {
        plen++;
    }
    if(plen > ((uint32)SVCRT_MINI_PATH_MAX - 1u))
    {
        return SVCRT_LOADER_ERR_PARAM;
    }

    (void)svcrt_mini_auto_read(&len);   /* 读不到 = 空清单，仍然可以新增 */

    while(svcrt_mini_auto_next(&cur, len, &s, &e) != 0u)
    {
        if(svcrt_mini_auto_line_is(s, e, path) != 0u)
        {
            listed = 1u;
        }
    }

    /* 空改动不落盘：本来就在清单里再加、本来就不在再删，都不去动那个文件。 */
    if((listed != 0u) == (on != 0u))
    {
        return 0;
    }

    cur = 0u;
    while(svcrt_mini_auto_next(&cur, len, &s, &e) != 0u)
    {
        if(svcrt_mini_auto_line_is(s, e, path) != 0u)
        {
            continue;                   /* 旧行丢掉，后面按需要重新追加 */
        }
        if((o + (e - s) + 1u) >= (uint32)SVCRT_MINI_AUTOSTART_MAX)
        {
            return SVCRT_LOADER_ERR_NOSPACE;
        }
        for(k = s; k < e; k++)
        {
            g_mini_auto_out[o++] = g_mini_auto_in[k];
        }
        g_mini_auto_out[o++] = '\n';
    }

    if(on != 0u)
    {
        if((o + plen + 1u) >= (uint32)SVCRT_MINI_AUTOSTART_MAX)
        {
            return SVCRT_LOADER_ERR_NOSPACE;
        }
        for(k = 0u; k < plen; k++)
        {
            g_mini_auto_out[o++] = (uint8)path[k];
        }
        g_mini_auto_out[o++] = '\n';
    }

    if(o == 0u)
    {
        /* 清单空了：删掉文件，而不是留一个 0 字节文件。两者「一条自启都没有」
         * 的语义一样，但一个残留的空文件会让人以为自己配过。 */
        (void)svcrt_fs_remove(SVCRT_MINI_AUTOSTART_FILE);
        return 0;
    }

    if(svcrt_fs_write_file(SVCRT_MINI_AUTOSTART_FILE, g_mini_auto_out, o) != 0)
    {
        return SVCRT_MINI_ERR_FS;
    }

    return 0;
}

#endif /* SVCRT_USE_MINIAPP */
