/**
* @brief SVCrtOS Driver SDK ??????? - ???????????
* @details ???????????????????? svcrt_dev_drv_t ???? + DrvMain ???
*          ?????????????? .bin??????? ROM ??????0x08040000????
*          ??????????¨Ή?????? svcrt_dev_open("TEMP", 0) ?????υτ??
*          ??????????? SVC 0x14 ?????????? svcrt_driver_bridge.c ????????
*/

#include "svcrt_driver_sdk.h"

/* ?????????? */
#define TEMP_CTRL_SET_UNIT   (0x0100)   /* 0=?????, 1=????? */

typedef struct {
    svcrt_dev_hdr_t hdr;
    uint8   unit;           /* ????¦Λ */
    int16   last_temp;      /* ?????????? */
    uint8   opened;
} temp_dev_obj_t;

static temp_dev_obj_t temp_obj;

static svcrt_dev_hdr_t *temp_drv_open(uint32 dev_id, uint32 param)
{
    (void)dev_id; (void)param;
    temp_obj.unit           = 0;
    temp_obj.last_temp      = 250;       /* 25.0 ??????0.1 ???¦Λ?? */
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
        v = (int16)(v * 9 / 5 + 320);   /* ???????0.1 ???¦Λ?? */

    pdata[0] = (uint8)(v & 0xFF);
    pdata[1] = (uint8)((v >> 8) & 0xFF);
    return 2;
}

static int32 temp_drv_write(svcrt_dev_hdr_t *obj, uint8 *pdata, int32 len)
{
    (void)obj; (void)pdata; (void)len;
    return SVCRT_DRV_ERROR;   /* ????????? */
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

void DrvMain(void)
{
    svcrt_drv_register("TEMP", &temp_drv, 0);

    while(1)
    {
        /* ??????????????????????????????????????? */
        svcrt_task_wait(1000);
    }
}
