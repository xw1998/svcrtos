/**
* @brief SVCrtOS Driver SDK - Çý¶¯×¢²áÇÅ½Ó²ã
*/

#include "svcrt_driver_sdk.h"
#include "devsio.h"

static DRV_INTERFACE _drv_adapter;

static DEV_HDR* _drv_adapter_open(uint32 devid, uint32 param)
{
    SVCRT_DRV_INTERFACE *real_drv = (SVCRT_DRV_INTERFACE*)_drv_adapter.DrvOpen;
    if(real_drv && real_drv->DrvOpen)
    {
        return real_drv->DrvOpen(devid, param);
    }
    return 0;
}

static int32 _drv_adapter_read(DEV_HDR *obj, uint8 *pdata, int32 len)
{
    SVCRT_DRV_INTERFACE *real_drv = (SVCRT_DRV_INTERFACE*)_drv_adapter.DrvOpen;
    if(real_drv && real_drv->DrvRead)
    {
        return real_drv->DrvRead(obj, pdata, len);
    }
    return SVCRT_DRV_ERROR;
}

static int32 _drv_adapter_write(DEV_HDR *obj, uint8 *pdata, int32 len)
{
    SVCRT_DRV_INTERFACE *real_drv = (SVCRT_DRV_INTERFACE*)_drv_adapter.DrvOpen;
    if(real_drv && real_drv->DrvWrite)
    {
        return real_drv->DrvWrite(obj, pdata, len);
    }
    return SVCRT_DRV_ERROR;
}

static int32 _drv_adapter_ctrl(DEV_HDR *obj, uint32 code, uint32 value)
{
    SVCRT_DRV_INTERFACE *real_drv = (SVCRT_DRV_INTERFACE*)_drv_adapter.DrvOpen;
    if(real_drv && real_drv->DrvCtrl)
    {
        return real_drv->DrvCtrl(obj, code, value);
    }
    return SVCRT_DRV_ERROR;
}

int32 svcrtDrvRegister(const char *name, SVCRT_DRV_INTERFACE *drv, uint32 dev_num)
{
    if(drv == 0 || name == 0)
        return -2;

    _drv_adapter.DrvOpen  = (DrvOpenFunc)drv;
    _drv_adapter.DrvRead  = _drv_adapter_read;
    _drv_adapter.DrvWrite = _drv_adapter_write;
    _drv_adapter.DrvCtrl  = _drv_adapter_ctrl;

    return kerDevRegister(name, (DRV_INTERFACE*)&_drv_adapter, dev_num);
}

int32 svcrtDrvUnregister(const char *name)
{
    return kerDevUnregister(name);
}

int32 svcrtDrvGetCount(void)
{
    return kerDevGetCount();
}
