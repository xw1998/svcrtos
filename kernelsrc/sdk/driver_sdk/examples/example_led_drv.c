/**
* @brief SVCrtOS Driver SDK - Ê¾ÀýLEDÇý¶¯
*/

#include "svcrt_driver_sdk.h"

typedef struct {
    DEV_HDR hdr;
    uint32  led_id;
    uint8   state;
} LED_DEV_OBJ;

static LED_DEV_OBJ led_dev_obj = {0, 0, 0};

static DEV_HDR* led_drv_open(uint32 devid, uint32 param)
{
    led_dev_obj.led_id = devid;
    led_dev_obj.state = 0;
    led_dev_obj.hdr.blocksize = 1;
    return (DEV_HDR*)&led_dev_obj;
}

static int32 led_drv_close(DEV_HDR *obj)
{
    return SVCRT_DRV_OK;
}

static int32 led_drv_read(DEV_HDR *obj, uint8 *pdata, int32 len)
{
    LED_DEV_OBJ *p = (LED_DEV_OBJ*)obj;
    if(pdata != 0 && len >= 1)
    {
        pdata[0] = p->state;
        return 1;
    }
    return SVCRT_DRV_ERROR;
}

static int32 led_drv_write(DEV_HDR *obj, uint8 *pdata, int32 len)
{
    LED_DEV_OBJ *p = (LED_DEV_OBJ*)obj;
    if(pdata != 0 && len >= 1)
    {
        p->state = pdata[0];
    }
    return len;
}

static int32 led_drv_ctrl(DEV_HDR *obj, uint32 code, uint32 value)
{
    LED_DEV_OBJ *p = (LED_DEV_OBJ*)obj;
    switch(code)
    {
    case SVCRT_DEV_CTRL_GET_STATUS:
        return p->state;
    case SVCRT_DEV_CTRL_RESET:
        p->state = 0;
        return SVCRT_DRV_OK;
    default:
        return SVCRT_DRV_ERROR;
    }
}

static SVCRT_DRV_INTERFACE led_drv = {
    led_drv_open,
    led_drv_close,
    led_drv_read,
    led_drv_write,
    led_drv_ctrl
};

int32 led_drv_install(void)
{
    return svcrtDrvRegister("LED", &led_drv, 0);
}
