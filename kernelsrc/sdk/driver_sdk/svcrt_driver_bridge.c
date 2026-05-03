/**
* @brief SVCrtOS Driver SDK - Çı¶¯×¢²áÇÅ½Ó²ã
*/

#include "svcrt_driver_sdk.h"
#include "svcrt_dev.h"

static svcrt_dev_drv_t _drv_adapter;

static svcrt_dev_hdr_t *_drv_adapter_open(uint32 dev_id, uint32 param)
{
    svcrt_dev_drv_t *real_drv = (svcrt_dev_drv_t *)_drv_adapter.drv_open;
    if(real_drv && real_drv->drv_open)
    {
        return real_drv->drv_open(dev_id, param);
    }
    return 0;
}

static int32 _drv_adapter_read(svcrt_dev_hdr_t *obj, uint8 *pdata, int32 len)
{
    svcrt_dev_drv_t *real_drv = (svcrt_dev_drv_t *)_drv_adapter.drv_open;
    if(real_drv && real_drv->drv_read)
    {
        return real_drv->drv_read(obj, pdata, len);
    }
    return SVCRT_DRV_ERROR;
}

static int32 _drv_adapter_write(svcrt_dev_hdr_t *obj, uint8 *pdata, int32 len)
{
    svcrt_dev_drv_t *real_drv = (svcrt_dev_drv_t *)_drv_adapter.drv_open;
    if(real_drv && real_drv->drv_write)
    {
        return real_drv->drv_write(obj, pdata, len);
    }
    return SVCRT_DRV_ERROR;
}

static int32 _drv_adapter_ctrl(svcrt_dev_hdr_t *obj, uint32 code, uint32 value)
{
    svcrt_dev_drv_t *real_drv = (svcrt_dev_drv_t *)_drv_adapter.drv_open;
    if(real_drv && real_drv->drv_ctrl)
    {
        return real_drv->drv_ctrl(obj, code, value);
    }
    return SVCRT_DRV_ERROR;
}

int32 svcrt_drv_register(const char *name, svcrt_dev_drv_t *drv, uint32 dev_num)
{
    if(drv == 0 || name == 0)
        return -2;

    _drv_adapter.drv_open  = (svcrt_drv_open_func)drv;
    _drv_adapter.drv_read  = _drv_adapter_read;
    _drv_adapter.drv_write = _drv_adapter_write;
    _drv_adapter.drv_ctrl  = _drv_adapter_ctrl;

    return svcrt_dev_register(name, &_drv_adapter, dev_num);
}

int32 svcrt_drv_unregister(const char *name)
{
    return svcrt_dev_unregister(name);
}

int32 svcrt_drv_get_count(void)
{
    return svcrt_dev_get_count();
}
