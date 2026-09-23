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
#include "svcrt_log.h"
#include "svcrt_spin.h"

#if (SVCRT_USE_MINIAPP == 1)

/* 负载一次读进来的字节数：与文件系统的单文件读缓存（SVCRT_FS_CACHE_SIZE）
 * 同宽，每段恰好喂满一次小缓存，不制造第二个中转缓冲。 */
#define SVCRT_MINI_CHUNK        (SVCRT_FS_CACHE_SIZE)

/* 重定位表：ABI 的上限就是 SVCRT_APP_RELOC_MAX 条（4KB），一次读完再校验，
 * 不做滑动窗口。窗口能省的静态内存只有几 KB，却要让「一次 reloc_apply 会碰到
 * 多少条目」这类边界推理跟着负载分段走——省下的内存远小于推理出错的代价。
 * 这块缓冲是**装载期**的，与小程序的运行期占用（那块 RAM 块）无关。 */
static uint32 g_mini_reloc[SVCRT_APP_RELOC_MAX];

/* 镜像头：要当结构体用就必须 4 字节对齐，所以按 uint32 数组开 */
static uint32 g_mini_hdr[SVCRT_APP_HEADER_SIZE / 4u];

static svcrt_mini_info_t g_mini;        /* 全 0 = 没有在跑 */
static uint8  g_mini_busy;              /* 1 = 正在装载 / 正在回收（挡并发入口） */
static svcrt_spinlock_t g_mini_lock = SVCRT_SPINLOCK_INIT;

/* 单个小程序的体积上限（块的字节数）。默认取编译期 SLOT_RAM_MAX_BLOCK，运行期
 * 可以在 [SLOT_RAM_MIN_BLOCK, SLOT_RAM_MAX_BLOCK] 之间收紧（shell: mini limit）。
 * 只能收紧不能放宽的理由写在 svcrt_mini.h 的 svcrt_mini_max_bytes() 上：
 * SLOT_RAM_MAX_BLOCK 同时是池单块上限与 MPU 数据窗口上限。 */
static uint32 g_mini_max_block = SLOT_RAM_MAX_BLOCK;

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

/* 清空状态：不用 memset，避免为一个结构体再引一层依赖 */
static void svcrt_mini_clear(void)
{
    g_mini.task_id    = 0u;
    g_mini.code_base  = 0u;
    g_mini.code_block = 0u;
    g_mini.code_size  = 0u;
    g_mini.ram_base   = 0u;
    g_mini.ram_size   = 0u;
    g_mini.entry      = 0u;
    g_mini.crc        = 0u;
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
    uint32 irq;
    uint32 code_size;
    uint32 code_span;
    uint32 ram_size_hdr;
    uint32 code_base = 0u;
    uint32 code_block = 0u;
    uint32 code_want;
    uint32 ram_base = 0u;
    uint32 ram_block = 0u;
    uint32 max_block = 0u;
    uint32 entry;
    uint32 total;
    uint32 cur;
    uint32 crc_done;
    uint32 idx = 0u;
    uint32 crc = 0u;
    uint32 delta_rom;
    uint32 delta_ram;
    int32  ret;
    int32  task_id;

    if(path == 0)
    {
        return SVCRT_LOADER_ERR_PARAM;
    }

    /* 入口先占位：装载期间即使是「已有一个在跑」也要在这里就挡住，而不是等
     * 文件读完才发现块没了——那时已经把块借出去了。 */
    svcrt_spin_lock_irqsave(&g_mini_lock, &irq);
    if((g_mini_busy != 0u) || (g_mini.code_base != 0u) || (g_mini.task_id != 0u))
    {
        svcrt_spin_unlock_irqrestore(&g_mini_lock, irq);
        return SVCRT_LOADER_ERR_BUSY;
    }
    g_mini_busy    = 1u;
    max_block      = g_mini_max_block;      /* 本次装载用哪条上限，此刻定下来 */
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
     * 不做「半借」——归还路径只认一种状态，少一种状态就少一类现场查不出来的错。 */
    if(svcrt_ptable_mini_alloc(code_span, ram_size_hdr,
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
        (void)svcrt_ptable_mini_free();         /* 任务表满了：两块还在我们手里 */
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
    g_mini.task_id   = (uint32)task_id;
    g_mini.code_base = code_base;
    g_mini.code_block = code_block;
    g_mini.code_size = code_size;
    g_mini.ram_base  = ram_base;
    g_mini.ram_size  = ram_size_hdr;
    g_mini.entry     = entry;
    g_mini.crc       = crc;
    g_mini_busy      = 0u;
    svcrt_spin_unlock_irqrestore(&g_mini_lock, irq);

    SVCRT_LOGI("MINI", "%s: %u B payload -> code 0x%08X (%u B block), RW/stack 0x%08X (%u B), task %d",
               path, (unsigned)code_size, (unsigned)code_base, (unsigned)code_block,
               (unsigned)ram_base, (unsigned)ram_size_hdr, (int)task_id);

    svcrt_sched_activate_higher((uint8)APP_TASK_PRIORITY);
    return 0;

free_out:
    (void)svcrt_ptable_mini_free();
unlock_out:
    svcrt_sched_unlock_internal();
out:
    {
        uint32 irq2;

        svcrt_spin_lock_irqsave(&g_mini_lock, &irq2);
        g_mini_busy = 0u;
        svcrt_spin_unlock_irqrestore(&g_mini_lock, irq2);
    }
    return ret;
}

int32 svcrt_mini_stop(void)
{
    uint32 irq;
    uint32 task_id;

    svcrt_spin_lock_irqsave(&g_mini_lock, &irq);
    if((g_mini.code_base == 0u) || (g_mini_busy != 0u))
    {
        svcrt_spin_unlock_irqrestore(&g_mini_lock, irq);
        return SVCRT_LOADER_ERR_STATE;
    }
    g_mini_busy = 1u;
    task_id     = g_mini.task_id;
    svcrt_spin_unlock_irqrestore(&g_mini_lock, irq);

    /* 先把镜像（含它自己创建的线程）踢出调度，再还块。顺序不能反：还完块，
     * 那块 RAM 可能立刻被下一个装载者拿去写代码，而原任务还在跑。 */
    if(task_id != 0u)
    {
        svcrt_loader_halt_image(task_id);
    }

    (void)svcrt_ptable_mini_free();

    svcrt_spin_lock_irqsave(&g_mini_lock, &irq);
    svcrt_mini_clear();
    g_mini_busy = 0u;
    svcrt_spin_unlock_irqrestore(&g_mini_lock, irq);

    SVCRT_LOGI("MINI", "stopped: task %u, code and RAM blocks returned to the pool",
               (unsigned)task_id);
    return 0;
}

int32 svcrt_mini_info(svcrt_mini_info_t *p_out)
{
    uint32 irq;

    if(p_out == 0)
    {
        return -1;
    }

    svcrt_spin_lock_irqsave(&g_mini_lock, &irq);
    if(g_mini.code_base == 0u)
    {
        svcrt_spin_unlock_irqrestore(&g_mini_lock, irq);
        return -1;
    }
    *p_out = g_mini;
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

int32 svcrt_mini_on_task_exit(uint32 task_id)
{
    uint32 irq;

    if(task_id == 0u)
    {
        return 0;
    }

    svcrt_spin_lock_irqsave(&g_mini_lock, &irq);
    if((g_mini.code_base == 0u) || (g_mini.task_id != task_id) || (g_mini_busy != 0u))
    {
        /* 不是小程序的主任务（内核任务、App 任务，或小程序内部的子线程）：
         * 子线程退出不动块——小程序可能只是收掉了一个 worker。 */
        svcrt_spin_unlock_irqrestore(&g_mini_lock, irq);
        return 0;
    }
    g_mini_busy = 1u;
    svcrt_spin_unlock_irqrestore(&g_mini_lock, irq);

    /* 主任务退出 = 整个小程序结束：它自己创建的线程也必须一起停，停完才还块。
     * 此刻调用者自身已被置为 INVALID，因此 halt_image 不会重复收尸它。 */
    svcrt_loader_halt_image(task_id);
    (void)svcrt_ptable_mini_free();

    svcrt_spin_lock_irqsave(&g_mini_lock, &irq);
    svcrt_mini_clear();
    g_mini_busy = 0u;
    svcrt_spin_unlock_irqrestore(&g_mini_lock, irq);

    SVCRT_LOGI("MINI", "task %u returned: MiniApp finished, code and RAM blocks back in the pool",
               (unsigned)task_id);
    return 1;
}

#endif /* SVCRT_USE_MINIAPP */
