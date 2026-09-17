/**
* @brief SVCrtOS Driver SDK 示例驱动 - 温度传感器（模拟）
* @details 演示独立驱动的最小实现：svcrt_dev_drv_t 五个接口 + DrvMain 注册。
*          可编译为独立 .bin 固件，烧录到驱动池分区（起始 0x08040000）。
*          应用侧用 svcrt_dev_open("TEMP", 0) 即可按设备名访问。
*          注册经 SVC 0x14，由 svcrt_driver_bridge.c 转发到内核。
*/

#include "svcrt_driver_sdk.h"

/* 控制命令码 */
#define TEMP_CTRL_SET_UNIT   (0x0100)   /* 0=摄氏度，1=华氏度 */

typedef struct {
    svcrt_dev_hdr_t hdr;
    uint8   unit;           /* 单位（0=摄氏度，1=华氏度） */
    int16   last_temp;      /* 最近一次读数，以 0.1 度为单位 */
    uint8   opened;
} temp_dev_obj_t;

static temp_dev_obj_t temp_obj;

static svcrt_dev_hdr_t *temp_drv_open(uint32 dev_id, uint32 param)
{
    (void)dev_id; (void)param;
    temp_obj.unit           = 0;
    temp_obj.last_temp      = 250;       /* 25.0 度（以 0.1 度为单位） */
    temp_obj.hdr.block_size = 2;
    temp_obj.opened         = 1;
    return (svcrt_dev_hdr_t *)&temp_obj;
}

static int32 temp_drv_close(svcrt_dev_hdr_t *obj)
{
    temp_dev_obj_t *p = (temp_dev_obj_t *)obj;
    p->opened = 0;
    return SVCRT_DRV_OK;
}

static int32 temp_drv_read(svcrt_dev_hdr_t *obj, uint8 *pdata, int32 len)
{
    temp_dev_obj_t *p = (temp_dev_obj_t *)obj;
    int16 v;

    if(pdata == 0 || len < 2)
        return SVCRT_DRV_INVALID_PARAM;
    if(!p->opened)
        return SVCRT_DRV_ERROR;

    v = p->last_temp;
    if(p->unit == 1)
        v = (int16)(v * 9 / 5 + 320);   /* 摄氏度转华氏度，同样按 0.1 度的定点表示 */

    pdata[0] = (uint8)(v & 0xFF);
    pdata[1] = (uint8)((v >> 8) & 0xFF);
    return 2;
}

static int32 temp_drv_write(svcrt_dev_hdr_t *obj, uint8 *pdata, int32 len)
{
    (void)obj; (void)pdata; (void)len;
    return SVCRT_DRV_ERROR;   /* 温度只读，不支持写入 */
}

static int32 temp_drv_ctrl(svcrt_dev_hdr_t *obj, uint32 code, uint32 value)
{
    temp_dev_obj_t *p = (temp_dev_obj_t *)obj;
    switch(code)
    {
    case TEMP_CTRL_SET_UNIT:
        p->unit = (uint8)(value & 0x1);
        return SVCRT_DRV_OK;
    case SVCRT_DEV_CTRL_GET_STATUS:
        return (int32)p->last_temp;
    default:
        return SVCRT_DRV_ERROR;
    }
}

static svcrt_dev_drv_t temp_drv = {
    temp_drv_open,
    temp_drv_close,
    temp_drv_read,
    temp_drv_write,
    temp_drv_ctrl
};

/* ---- console services: user log (SVC 0x19) and user command (SVC 0x1A) - */

static int drv_cmd_temp(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    (void)svcrt_shell_printf("TEMP raw=%d (0.1 deg), unit=%d\r\n",
                             (int)temp_obj.last_temp, (int)temp_obj.unit);
    return 0;
}

static svcrt_ushell_cmd_t g_drv_cmd =
{
    "temp",
    drv_cmd_temp,
    "Simulated temperature driver, registered by the driver via SVC 0x1A",
    1
};


void DrvMain(void)
{
    uint8 cmd_done = 0u;

    svcrt_drv_register("TEMP", &temp_drv, 0);

    (void)svcrt_log_printf(SVCRT_LOG_INFO, "TEMPDRV", "temperature driver registered\r\n");

    while(1)
    {
        svcrt_task_wait(1000);

        /* The console command table is built by the shell task once the
         * scheduler is running, and drivers are started before it, so the
         * first console registration waits one period to be sure. */
        if(cmd_done == 0u)
        {
            int32 gr = svcrt_shell_cmd_register(&g_drv_cmd);

            (void)svcrt_log_printf(SVCRT_LOG_INFO, "TEMPDRV",
                                   "console command 'temp' register rc=%d\r\n", (int)gr);
            cmd_done = 1u;
        }
    }
}
