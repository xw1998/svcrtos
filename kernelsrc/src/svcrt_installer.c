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
* @param dev 设备句柄（位置正好在镜像头之后）
* @return 成功返回槽位号（>=0），失败返回 SVCRT_LOADER_ERR_x
* @details 「开机是否自启」只由镜像头 flags 决定（tools/pack_app.py --autostart /
*          --no-autostart 写入，裸镜像走 svcrt_loader_identify() 的全局默认）。
*          安装路径与开机扫描路径用同一套判据，避免同一个镜像「烧录自启、
*          安装不自启」这种不一致。
*/
static int32 svcrt_installer_commit(int32 dev)
{
    uint32 autostart = ((svcrt_installer_hdr.flags & SVCRT_APP_FLAG_AUTOSTART) != 0u) ? 1u : 0u;
    svcrt_partition_table_t *pt = svcrt_ptable_get();
    int32 r;

    /* App and driver share one install path; the landing slot is picked by the */
    /* pool allocator at install time (highest-density append), it is not */
    /* declared by the image header any more. A driver entry only additionally */
    /* requires type = DRIVER, which keeps the by-type routing explicit. */
    if(svcrt_installer_hdr.type == SVCRT_APP_TYPE_DRIVER)
    {
        r = svcrt_loader_load_driver_dev(dev, &svcrt_installer_hdr, 0u);
    }
    else
    {
        r = svcrt_loader_load_dev_hdr(dev, &svcrt_installer_hdr, 0u);
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

#endif
