/**
* @brief SVCrtOS Driver SDK - 驱动注册桥接层
* @details 将SDK驱动的注册请求转发到内核设备框架
*          SDK驱动和内核驱动共用相同的 svcrt_dev_drv_t 结构
* @author xw
* @date 2026.05.03
*/

#include "svcrt_driver_sdk.h"
#include "svcrt_dev.h"

int32 svcrt_drv_register(const char *name, svcrt_dev_drv_t *drv, uint32 dev_num)
{
    return svcrt_dev_register(name, drv, dev_num);
}

int32 svcrt_drv_unregister(const char *name)
{
    return svcrt_dev_unregister(name);
}

int32 svcrt_drv_get_count(void)
{
    return svcrt_dev_get_count();
}
