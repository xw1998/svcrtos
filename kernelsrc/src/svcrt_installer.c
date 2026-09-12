/**
* @file svcrt_installer.c
* @brief SVCrtOS 内核内安装任务实现（方案A）
* @details 安装流程：
*          1) 打开镜像接收设备（默认 COM1）；
*          2) 在字节流中搜索镜像头魔数 "SVCA" 完成逐字节同步；
*          3) 读出完整 256 字节镜像头；
*          4) 交给 svcrt_loader_load_dev_hdr() 流式写入空闲槽位
*             （内部完成：长度/兼容签名/CRC 校验 → 擦除 → 分块写入 → 回读复算 CRC）；
*          5) 按策略自动启动该 App。
*
*          掉电/中断安全：写入期间槽位为 INSTALLING，只有全部写完且 CRC 复核通过
*          才会置 LOADED；因此中断安装不会留下一个「看似可用」的坏槽位。
*
* @note 本模块只能由内核特权态调用。
*/

#include "svcrt_installer.h"
#include "svcrt_loader.h"
#include "svcrt_dev.h"
#include "svcrt_task.h"
#include "svcrt_cfg.h"
#include "svcrt_partition.h"

#if (INSTALLER_ENABLE == 1)

/* 空转上限：达到后交还 CPU，避免在无数据时长期独占 */
#define SVCRT_INSTALLER_IDLE_LIMIT   (200000u)

/* 安装任务的静态栈（内核任务，取自内核 RAM；与 App 无关） */
static uint32 svcrt_installer_stack[INSTALLER_TASK_STACK_SIZE / 4u];

/**
* @brief 在设备字节流中同步到镜像头
* @param dev   设备句柄
* @param p_hdr 输出：读满 256 字节的镜像头
* @return 0=已读到完整镜像头，-1=长时间无数据（需让出 CPU）
* @details 先用 4 字节窗口匹配魔数，不匹配则逐字节左移重新同步，
*          因此串口上混杂的杂散字节会被自动跳过。
*/
static int32 svcrt_installer_sync(int32 dev, svcrt_app_header_t *p_hdr)
{
    uint8 *p = (uint8 *)p_hdr;
    uint32 got = 0u;
    uint32 idle = 0u;

    while(got < SVCRT_APP_HEADER_SIZE)
    {
        uint8 b;
        int32 r = svcrt_dev_read_internal(dev, &b, 1);

        if(r == 1)
        {
            idle = 0u;

            if(got < 4u)
            {
                p[got++] = b;
                if((got == 4u) && (p_hdr->magic != SVCRT_APP_MAGIC))
                {
                    p[0] = p[1];
                    p[1] = p[2];
                    p[2] = p[3];
                    got = 3u;       /* 丢弃最早一字节，继续向后匹配 */
                }
            }
            else
            {
                p[got++] = b;
            }
        }
        else
        {
            idle++;
            if(idle > SVCRT_INSTALLER_IDLE_LIMIT)
            {
                return -1;
            }
        }
    }

    return 0;
}

static void svcrt_installer_task(void)
{
    char dev_name[] = INSTALLER_DEV_NAME;
    svcrt_app_header_t hdr;

    for(;;)
    {
        int32 dev = svcrt_dev_open_internal(dev_name, (uint32)INSTALLER_DEV_ARG);

        if(dev >= 0)
        {
            if(svcrt_installer_sync(dev, &hdr) == 0)
            {
                int32 slot = svcrt_loader_load_dev_hdr(dev, &hdr, 0u);

                if(slot >= 0)
                {
                    #if (INSTALLER_AUTO_START == 1)
                    svcrt_loader_start((uint32)slot);
                    #endif
                }
            }
        }

        /* 单轮结束（成功/失败/无数据）都让出 CPU，避免抢占 App 运行 */
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

#else   /* INSTALLER_ENABLE == 0 */

int32 svcrt_installer_init(void)
{
    return 0;
}

#endif
