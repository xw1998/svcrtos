/**
* @file svcrt_installer.c
* @brief SVCrtOS 内核内安装任务实现（方案A）
* @details 安装流程：
*          1) 打开镜像接收设备（默认 COM1），**只打开一次并常驻持有句柄**；
*          2) 在字节流中搜索镜像头魔数 "SVCA" 完成逐字节同步；
*          3) 收满 256 字节镜像头；
*          4) 交给 svcrt_loader_load_dev_hdr() 流式写入空闲槽位
*             （长度/兼容签名校验 → 擦除 → 分块写入 → 回读复算 CRC）；
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
#include "svcrt_fault.h"
#include "svcrt_partition.h"

#if (INSTALLER_ENABLE == 1)

/* 空转上限：达到后先让出 CPU，下一轮继续在同一位置收（不丢已收字节） */
#define SVCRT_INSTALLER_IDLE_LIMIT   (20000u)

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
                int32 r;

                /* 按镜像头里的 type 自动分流：驱动镜像进驱动区，其余进 App 槽位 */
                if(svcrt_installer_hdr.type == SVCRT_APP_TYPE_DRIVER)
                {
                    r = svcrt_loader_load_driver_dev(dev, &svcrt_installer_hdr, 0u);
                    svcrt_installer_got = 0u;

                    if(r >= 0)
                    {
                        #if (DRIVER_AUTO_START == 1)
                        svcrt_loader_start_driver();
                        #endif
                    }
                    else
                    {
                        /* 安装失败不能静默：记录故障，便于用故障读数接口事后定位 */
                        svcrt_fault_record(SVCRT_FAULT_INSTALLFAIL, svcrt_current_task_id);
                    }
                }
                else
                {
                    r = svcrt_loader_load_dev_hdr(dev, &svcrt_installer_hdr, 0u);
                    svcrt_installer_got = 0u;

                    if(r >= 0)
                    {
                        #if (INSTALLER_AUTO_START == 1)
                        svcrt_loader_start((uint32)r);
                        #endif
                    }
                    else
                    {
                        /* 同上：App 安装失败（长度/兼容/CRC/Flash 任一环节）记故障 */
                        svcrt_fault_record(SVCRT_FAULT_INSTALLFAIL, svcrt_current_task_id);
                    }
                }
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

#else   /* INSTALLER_ENABLE == 0 */

int32 svcrt_installer_init(void)
{
    return 0;
}

#endif
