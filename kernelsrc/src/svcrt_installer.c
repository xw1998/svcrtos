/**
* @file svcrt_installer.c
* @brief SVCrtOS 内核内安装模块实现（常驻任务 + 命令触发一次性窗口）
* @details 安装流程：
*          1) 打开镜像接收设备（默认 COM1）；
*          2) 在字节流中搜索镜像头魔数 "SVCA" 完成逐字节同步；
*          3) 收满 256 字节镜像头；
*          4) 交给 svcrt_loader_load_dev_hdr() / svcrt_loader_load_driver_dev()
*             流式写入空闲槽位（长度/兼容签名校验 → 擦除 → 分块写入 → 回读复算 CRC）；
*          5) 按镜像头里的自启标志决定是否立即启动。
*
*          掉电/中断安全：写入期间槽位为 INSTALLING，只有全部写完且 CRC 复核通过
*          才会置 LOADED；因此中断安装不会留下一个「看似可用」的坏槽位。
*
*          两种使用形态：
*          - 常驻任务：svcrt_installer_init() 注册，适合没有控制台的最小系统；
*          - 一次性窗口：svcrt_installer_run_once() 由内核 Shell 的 `install`
*            命令调用，串行占用串口，避免「两个读者把同一串口 FIFO 分掉」。
*            开启 Shell（SHELL_ENABLE == 1）时上层只用后者。
*
* @note 本模块只能由内核特权态调用。
*/

#include "svcrt_installer.h"
#include "svcrt_loader.h"
#include "svcrt_dev.h"
#include "svcrt_task.h"
#include "svcrt_cfg.h"
#include "svcrt_fault.h"
#include "svcrt_ptable.h"
#include "svcrt_share.h"
#include "svcrt_partition.h"
#include "svcrt_log.h"
#include "svcrt_fs.h"

#if (INSTALLER_ENABLE == 1)

/* 空转上限：达到后先让出 CPU，下一轮继续在同一位置收（不丢已收字节） */
#define SVCRT_INSTALLER_IDLE_LIMIT   (20000u)

/* run_once 每轮让出 CPU 的步长（ms）：一次 pump 空转即约等于这个粒度 */
#define SVCRT_INSTALLER_POLL_MS      (5u)

/* 安装任务的静态栈（内核任务，取自内核 RAM；与 App 无关） */
static uint32 svcrt_installer_stack[INSTALLER_TASK_STACK_SIZE / 4u];

/* 收头进度（跨轮次保留，避免发送端中途停顿导致整帧作废） */
static svcrt_app_header_t svcrt_installer_hdr;
static uint32 svcrt_installer_got = 0u;

/**
* @brief 推进接收状态机
* @param dev 设备句柄
* @return 0=已收满一个完整镜像头（内容在 svcrt_installer_hdr），-1=本轮还没收完
* @details 先用 4 字节窗口匹配魔数，不匹配则逐字节左移继续找，
*          因此串口上混杂的杂散字节会被自动跳过。
*          达到空转上限时**保留已收字节**直接返回，由调用方让出 CPU 后继续。
*/
static int32 svcrt_installer_pump(int32 dev)
{
    uint8 *p = (uint8 *)&svcrt_installer_hdr;
    uint32 idle = 0u;

    while(svcrt_installer_got < SVCRT_APP_HEADER_SIZE)
    {
        uint8 b;

        if(svcrt_dev_read_internal(dev, &b, 1) != 1)
        {
            idle++;
            if(idle > SVCRT_INSTALLER_IDLE_LIMIT)
            {
                return -1;
            }
            continue;
        }

        idle = 0u;

        if(svcrt_installer_got < 4u)
        {
            p[svcrt_installer_got++] = b;

            if((svcrt_installer_got == 4u) &&
               (svcrt_installer_hdr.magic != SVCRT_APP_MAGIC))
            {
                /* 魔数不匹配：整体左移一字节，继续向后匹配 */
                p[0] = p[1];
                p[1] = p[2];
                p[2] = p[3];
                svcrt_installer_got = 3u;
            }
        }
        else
        {
            p[svcrt_installer_got++] = b;
        }
    }

    return 0;
}

/**
* @brief 把已收全的镜像头对应的负载落盘，并按自启标志决定是否立即启动
* @param dev       设备句柄（位置正好在镜像头之后）
* @param image_len 这一帧可用的总字节数：0 = 未知，按镜像头声明的尺寸读
*                  （串口路径传 0，文件路径传文件的真实长度）
* @return 成功返回槽位号（>=0），失败返回 SVCRT_LOADER_ERR_x
* @details 「开机是否自启」只由镜像头 flags 决定（tools/pack_app.py --autostart /
*          --no-autostart 写入，裸镜像走 svcrt_loader_identify() 的全局默认）。
*          安装路径与开机扫描路径用同一套判据，避免同一个镜像「烧录自启、
*          安装不自启」这种不一致。
*/
static int32 svcrt_installer_load(int32 dev, uint32 image_len)
{
    uint32 autostart = ((svcrt_installer_hdr.flags & SVCRT_APP_FLAG_AUTOSTART) != 0u) ? 1u : 0u;
    svcrt_partition_table_t *pt = svcrt_ptable_get();
    int32 r;

    /* 提交点之前的版本把关（版本单调 + 重复副本）。放在这里而不是放进
     * svcrt_loader_load_dev_hdr()：那个函数在写入第一个字节之前就已经需要
     * 决定要不要收这一帧，而它同时被「上电扫描 / 搬移」之外的路径复用；
     * 安装语义（升级 / 回灌 / 重装）只属于安装路径。 */
    r = svcrt_loader_check_install(&svcrt_installer_hdr);

    if(r == SVCRT_LOADER_ERR_DUP)
    {
        SVCRT_LOGE("INSTALL",
                   "refused: identical image already installed "
                   "(id 0x%08X, v%u.%u.%u.%u, %u B) - nothing to do",
                   (unsigned)svcrt_installer_hdr.image_id,
                   (unsigned)((svcrt_installer_hdr.version >> 24) & 0xFFu),
                   (unsigned)((svcrt_installer_hdr.version >> 16) & 0xFFu),
                   (unsigned)((svcrt_installer_hdr.version >> 8) & 0xFFu),
                   (unsigned)(svcrt_installer_hdr.version & 0xFFu),
                   (unsigned)svcrt_installer_hdr.image_size);
    }
    else if(r == SVCRT_LOADER_ERR_VERSION)
    {
        SVCRT_LOGE("INSTALL",
                   "refused: version not increasing "
                   "(id 0x%08X, incoming v%u.%u.%u.%u <= installed) - bump the version",
                   (unsigned)svcrt_installer_hdr.image_id,
                   (unsigned)((svcrt_installer_hdr.version >> 24) & 0xFFu),
                   (unsigned)((svcrt_installer_hdr.version >> 16) & 0xFFu),
                   (unsigned)((svcrt_installer_hdr.version >> 8) & 0xFFu),
                   (unsigned)(svcrt_installer_hdr.version & 0xFFu));
    }
    else if(r != 0)
    {
        SVCRT_LOGE("INSTALL", "refused: image identity missing (id 0x%08X, err %d)",
                   (unsigned)svcrt_installer_hdr.image_id, (int)r);
    }
    else
    {
        /* App and driver share one install path; the landing slot is picked by
         * the pool allocator at install time (highest-density append), it is
         * not declared by the image header any more. A driver entry only
         * additionally requires type = DRIVER, which keeps the by-type
         * routing explicit. */
        if(svcrt_installer_hdr.type == SVCRT_APP_TYPE_DRIVER)
        {
            r = svcrt_loader_load_driver_dev(dev, &svcrt_installer_hdr, image_len);
        }
        else
        {
            r = svcrt_loader_load_dev_hdr(dev, &svcrt_installer_hdr, image_len);
        }
    }

    svcrt_installer_got = 0u;

    if(r >= 0)
    {
        /* r 是刚写入的槽位记录号：只拉起刚落盘的那个槽，
         * 不能再去启动 0 号槽（否则多槽并存时永远只启第一个）。
         * svcrt_loader_start() 按槽位类型自动分流 App / 驱动。 */
        pt->slot_autostart[r] = autostart;

        if(autostart != 0u)
        {
            svcrt_loader_start((uint32)r);
        }
    }

    if(r < 0)
    {
        /* 安装失败不能静默：既上控制台（当场看得见错误码），也记录故障，
         * 便于用故障读数接口（shell 的 fault）事后定位。
         * 错误码含义见 svcrt_loader.h 的 SVCRT_LOADER_ERR_x。 */
        SVCRT_LOGE("INSTALL", "rejected: err %d, type %u, payload %u B, reloc %u",
                   (int)r,
                   (unsigned)svcrt_installer_hdr.type,
                   (unsigned)svcrt_installer_hdr.image_size,
                   (unsigned)svcrt_installer_hdr.reloc_count);

        /* Close the frame for the sender. The loader only NAKs once it has
         * reached the relocation-table stage; failures decided earlier
         * (compat id, size, reserve, flash) returned with no flow byte at
         * all, so the host sat out its whole ACK timeout without knowing it
         * had already lost. NAKing here covers every failure path - and when
         * the loader did send one, the duplicate is harmless: the host stops
         * at the first flow byte it sees. */
        {
            uint8 nak = 0x15u;          /* SVCRT_LOADER_NAK_BYTE */

            (void)svcrt_dev_write_internal(dev, &nak, 1u);
        }

        svcrt_fault_record(SVCRT_FAULT_INSTALLFAIL, svcrt_current_task_id);
    }
    else
    {
        SVCRT_LOGI("INSTALL", "slot %d committed: type %u, payload %u B, autostart %u",
                   (int)r,
                   (unsigned)svcrt_installer_hdr.type,
                   (unsigned)svcrt_installer_hdr.image_size,
                   (unsigned)autostart);
    }

    return r;
}

/* Serial frame: the length is whatever the header declares. The sender
 * has nothing else to tell us, so 0 means "unknown, trust the header"
 * (see svcrt_loader_load_dev_hdr). */
static int32 svcrt_installer_commit(int32 dev)
{
    return svcrt_installer_load(dev, 0u);
}

static void svcrt_installer_task(void)
{
    char dev_name[] = INSTALLER_DEV_NAME;
    int32 dev = -1;

    for(;;)
    {
        /* 设备只在需要时打开一次，之后常驻持有句柄 */
        if(dev < 0)
        {
            dev = svcrt_dev_open_internal(dev_name, (uint32)INSTALLER_DEV_ARG);
        }

        if(dev >= 0)
        {
            if(svcrt_installer_pump(dev) == 0)
            {
                (void)svcrt_installer_commit(dev);
            }
        }

        /* 单轮结束（收完/未收完/设备未就绪）都让出 CPU，避免抢占 App 运行 */
        svcrt_task_wait_internal((uint32)INSTALLER_TASK_PERIOD_MS);
    }
}

int32 svcrt_installer_init(void)
{
    return svcrt_task_register(svcrt_installer_task,
                               svcrt_installer_stack,
                               (uint32)sizeof(svcrt_installer_stack),
                               (uint8)INSTALLER_TASK_PRIORITY,
                               (uint32)INSTALLER_TASK_PERIOD_MS);
}

int32 svcrt_installer_run_once(int32 dev, uint32 timeout_ms)
{
    uint32 waited = 0u;

    if(dev < 0)
    {
        return SVCRT_LOADER_ERR_PARAM;
    }

    /* 每次窗口从干净的同步状态开始，避免上一次窗口残留的半截头污染本帧 */
    svcrt_installer_got = 0u;

    for(;;)
    {
        if(svcrt_installer_pump(dev) == 0)
        {
            break;
        }

        if(waited >= timeout_ms)
        {
            SVCRT_LOGE("INSTALL", "window timed out after %u ms (got %u B)",
                       (unsigned)waited, (unsigned)svcrt_installer_got);
            svcrt_installer_got = 0u;
            svcrt_fault_record(SVCRT_FAULT_INSTALLFAIL, svcrt_current_task_id);
            return SVCRT_LOADER_ERR_PARAM;
        }

        /* 让出 CPU：窗口期内控制台任务必须是「有礼貌的轮询」，不能独占调度 */
        svcrt_task_wait_internal(SVCRT_INSTALLER_POLL_MS);
        waited += SVCRT_INSTALLER_POLL_MS;
    }

    return svcrt_installer_commit(dev);
}

/* ============================================================
 * Installing straight out of the file system
 *
 * The streaming loader reads a frame from a *device*, so a file is presented
 * as one: a private device whose reads pull from the file stream svcrt_fs
 * opened. That keeps one install implementation - the same header checks, the
 * same relocation carry handling, the same single commit point - for the
 * serial path and the file path, instead of growing a second one that would
 * drift away from the first.
 * ============================================================ */
#define SVCRT_INSTALLER_FILE_DEV   "fsimg"

/* Non-NULL placeholder handle. The device core stores whatever drv_open
 * returns and hands it back on every read, so the state that matters - which
 * file, at which offset - stays where it belongs, in the file stream. */
static svcrt_dev_hdr_t svcrt_installer_file_obj;
static uint8 svcrt_installer_file_reg = 0u;

static svcrt_dev_hdr_t *svcrt_installer_file_open(uint32 dev_id, uint32 param)
{
    (void)dev_id;
    (void)param;
    return &svcrt_installer_file_obj;
}

static int32 svcrt_installer_file_close(svcrt_dev_hdr_t *obj)
{
    (void)obj;
    return 0;
}

static int32 svcrt_installer_file_read(svcrt_dev_hdr_t *obj, uint8 *pdata, int32 len)
{
    (void)obj;

    if((pdata == 0) || (len <= 0))
    {
        return -1;
    }

    return svcrt_fs_read_next(pdata, (uint32)len);
}

static int32 svcrt_installer_file_write(svcrt_dev_hdr_t *obj, uint8 *pdata, int32 len)
{
    /* The loader ACKs every chunk it has landed so that a *sender* knows to
     * send the next one. A file has no sender to pace and nothing to be told,
     * so the flow byte is dropped and reported as accepted. */
    (void)obj;
    (void)pdata;
    return len;
}

static svcrt_dev_drv_t svcrt_installer_file_drv =
{
    svcrt_installer_file_open,
    svcrt_installer_file_close,
    svcrt_installer_file_read,
    svcrt_installer_file_write,
    0
};

/* Register the shim once and hand back a handle. It serves whichever file
 * svcrt_fs_open_read() has open - there is exactly one, by design. */
static int32 svcrt_installer_file_dev(void)
{
    if(svcrt_installer_file_reg == 0u)
    {
        if(svcrt_dev_register(SVCRT_INSTALLER_FILE_DEV,
                              &svcrt_installer_file_drv, 0u) != 0)
        {
            return -1;
        }

        svcrt_installer_file_reg = 1u;
    }

    return svcrt_dev_open_internal(SVCRT_INSTALLER_FILE_DEV, 0u);
}

/* Read the 256-byte image header through the file stream that is already
 * open, so the header and the rest of the frame share one position. */
static int32 svcrt_installer_file_hdr(void)
{
    uint32 got = 0u;

    while(got < SVCRT_APP_HEADER_SIZE)
    {
        int32 n = svcrt_fs_read_next((uint8 *)&svcrt_installer_hdr + got,
                                     (uint32)(SVCRT_APP_HEADER_SIZE - got));

        if(n <= 0)
        {
            return -1;
        }

        got += (uint32)n;
    }

    return 0;
}

int32 svcrt_installer_from_file(const char *path)
{
    uint32 size = 0u;
    uint32 is_dir = 0u;
    int32 dev;
    int32 r;

    if((path == 0) || (svcrt_fs_mounted() == 0u))
    {
        return SVCRT_LOADER_ERR_PARAM;
    }

    /* Size first: a file that cannot possibly fit is refused before a single
     * byte of it is read, and the report can name the real numbers. */
    if(svcrt_fs_stat_path(path, &size, &is_dir) != 0)
    {
        SVCRT_LOGE("INSTALL", "cannot stat %s (fs err %d)",
                   path, (int)svcrt_fs_last_error());
        svcrt_fault_record(SVCRT_FAULT_INSTALLFAIL, svcrt_current_task_id);
        return SVCRT_LOADER_ERR_PARAM;
    }

    if(is_dir != 0u)
    {
        SVCRT_LOGE("INSTALL", "%s is a directory", path);
        svcrt_fault_record(SVCRT_FAULT_INSTALLFAIL, svcrt_current_task_id);
        return SVCRT_LOADER_ERR_PARAM;
    }

    if((size < SVCRT_APP_HEADER_SIZE) || (size > IMAGE_POOL_USABLE_SIZE))
    {
        SVCRT_LOGE("INSTALL", "%s is %u B, not an image that fits (%u..%u)",
                   path, (unsigned)size,
                   (unsigned)SVCRT_APP_HEADER_SIZE,
                   (unsigned)IMAGE_POOL_USABLE_SIZE);
        svcrt_fault_record(SVCRT_FAULT_INSTALLFAIL, svcrt_current_task_id);
        return SVCRT_LOADER_ERR_SIZE;
    }

    if(svcrt_fs_open_read(path) != 0)
    {
        SVCRT_LOGE("INSTALL", "cannot open %s (fs err %d)",
                   path, (int)svcrt_fs_last_error());
        svcrt_fault_record(SVCRT_FAULT_INSTALLFAIL, svcrt_current_task_id);
        return SVCRT_LOADER_ERR_PARAM;
    }

    if(svcrt_installer_file_hdr() != 0)
    {
        SVCRT_LOGE("INSTALL", "short read on %s (needs %u B of header)",
                   path, (unsigned)SVCRT_APP_HEADER_SIZE);
        (void)svcrt_fs_close_read();
        svcrt_fault_record(SVCRT_FAULT_INSTALLFAIL, svcrt_current_task_id);
        return SVCRT_LOADER_ERR_SIZE;
    }

    if(svcrt_installer_hdr.magic != SVCRT_APP_MAGIC)
    {
        /* Say it here: an .svcapp does not identify itself by name, and a file
         * that merely looks like one would otherwise be reported deeper as a
         * corrupt header - a cause the operator cannot act on. */
        SVCRT_LOGE("INSTALL", "%s: magic %08X, want %08X (not an .svcapp)",
                   path, (unsigned)svcrt_installer_hdr.magic,
                   (unsigned)SVCRT_APP_MAGIC);
        (void)svcrt_fs_close_read();
        svcrt_fault_record(SVCRT_FAULT_INSTALLFAIL, svcrt_current_task_id);
        return SVCRT_LOADER_ERR_MAGIC;
    }

    dev = svcrt_installer_file_dev();

    if(dev < 0)
    {
        SVCRT_LOGE("INSTALL", "file source device unavailable");
        (void)svcrt_fs_close_read();
        svcrt_fault_record(SVCRT_FAULT_INSTALLFAIL, svcrt_current_task_id);
        return SVCRT_LOADER_ERR_PARAM;
    }

    /* image_len = the file size: the loader must refuse a frame claiming more
     * than the file actually holds, and must stop at its end. */
    r = svcrt_installer_load(dev, size);

    (void)svcrt_dev_close_internal(dev);
    (void)svcrt_fs_close_read();

    return r;
}

#else   /* INSTALLER_ENABLE == 0 */

int32 svcrt_installer_init(void)
{
    return 0;
}

int32 svcrt_installer_run_once(int32 dev, uint32 timeout_ms)
{
    (void)dev;
    (void)timeout_ms;
    return SVCRT_LOADER_ERR_PARAM;
}

int32 svcrt_installer_from_file(const char *path)
{
    (void)path;
    return SVCRT_LOADER_ERR_PARAM;
}

#endif
