/**
* @brief SVCrtOS Driver SDK - Ê¾ÀýLEDÇý¶¯
*/

#include "svcrt_driver_sdk.h"

typedef struct {
    svcrt_dev_hdr_t hdr;
    uint32  led_id;
    uint8   state;
} led_dev_obj_t;

static led_dev_obj_t led_dev_obj = {{0}, 0, 0};

static svcrt_dev_hdr_t *led_drv_open(uint32 dev_id, uint32 param)
{
    led_dev_obj.led_id = dev_id;
    led_dev_obj.state = 0;
    led_dev_obj.hdr.block_size = 1;
    return (svcrt_dev_hdr_t *)&led_dev_obj;
}

static int32 led_drv_close(svcrt_dev_hdr_t *obj)
{
    return SVCRT_DRV_OK;
}

static int32 led_drv_read(svcrt_dev_hdr_t *obj, uint8 *pdata, int32 len)
{
    led_dev_obj_t *p = (led_dev_obj_t *)obj;
    if(pdata != 0 && len >= 1)
    {
        pdata[0] = p->state;
        return 1;
    }
    return SVCRT_DRV_ERROR;
}

static int32 led_drv_write(svcrt_dev_hdr_t *obj, uint8 *pdata, int32 len)
{
    led_dev_obj_t *p = (led_dev_obj_t *)obj;
    if(pdata != 0 && len >= 1)
    {
        p->state = pdata[0];
    }
    return len;
}

static int32 led_drv_ctrl(svcrt_dev_hdr_t *obj, uint32 code, uint32 value)
{
    led_dev_obj_t *p = (led_dev_obj_t *)obj;
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

static svcrt_dev_drv_t led_drv = {
    led_drv_open,
    led_drv_close,
    led_drv_read,
    led_drv_write,
    led_drv_ctrl
};

int32 led_drv_install(void)
{
    return svcrt_drv_register("LED", &led_drv, 0);
}
