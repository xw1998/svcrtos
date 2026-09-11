/**
* @brief SVCrtOS Driver SDK - 驱动注册桥接层
* @details 将SDK驱动的注册请求转发到内核设备框架
*          SDK驱动和内核驱动共用相同的 svcrt_dev_drv_t 结构
*          支持两种编译模式：
*          - 内核态(默认): 直接调用内核函数 svcrt_dev_register/unregister
*          - 用户态(SVCRT_DRV_USER_MODE): 通过SVC 0x14陷入内核完成注册
*          仅依赖 svcrt_driver_sdk.h，无需包含内核内部头文件，可独立编译
* @author xw
* @date 2026.05.03
*/

#include "svcrt_driver_sdk.h"

#ifdef SVCRT_DRV_USER_MODE

int32 svcrt_drv_register(const char *name, svcrt_dev_drv_t *drv, uint32 dev_num)
{
    uint32 p[4];
    p[0] = 1;
    p[1] = (uint32)name;
    p[2] = (uint32)drv;
    p[3] = dev_num;
    return svcrt_call_drv_mgr(p);
}

int32 svcrt_drv_unregister(const char *name)
{
    uint32 p[4];
    p[0] = 2;
    p[1] = (uint32)name;
    return svcrt_call_drv_mgr(p);
}

int32 svcrt_drv_get_count(void)
{
    uint32 p[4];
    p[0] = 3;
    return svcrt_call_drv_mgr(p);
}

#else

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

#endif
