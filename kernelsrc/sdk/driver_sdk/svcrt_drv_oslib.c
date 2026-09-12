/**
* @brief SVCrtOS Driver SDK - 用户态驱动OS接口封装
* @details 用户态驱动通过SVC指令调用内核服务
*          提供任务等待、事件操作等API
*          仅在 SVCRT_DRV_USER_MODE 定义时使用
*/

#include "svcrt_driver_sdk.h"
#include "svcrt_svc_call.h"

SVCRT_SVC_DECL_1(int32, 0x10, svcrt_call_dev_io, uint32 *);
SVCRT_SVC_DECL_V2(0x11, svcrt_call_task_ctrl, uint32, uint32);
SVCRT_SVC_DECL_1(uint32, 0x12, svcrt_call_sys_info, uint32);
SVCRT_SVC_DECL_1(int32, 0x13, svcrt_call_event_ctrl, uint32 *);

void svcrt_task_wait(uint32 ms)
{
    svcrt_call_task_ctrl(1, ms);
}

void svcrt_task_wait_period(void)
{
    svcrt_call_task_ctrl(2, 0);
}

void svcrt_task_delay(uint32 us)
{
    svcrt_call_task_ctrl(3, us);
}

uint32 svcrt_get_time_ms(void)
{
    return svcrt_call_sys_info(1);
}

int32 svcrt_event_create(char *name)
{
    uint32 p[3];
    p[0] = 1;
    p[1] = (uint32)name;
    return svcrt_call_event_ctrl(p);
}

void svcrt_event_wait(int32 handle, int32 timeout)
{
    uint32 p[3];
    p[0] = 2;
    p[1] = handle;
    p[2] = timeout;
    svcrt_call_event_ctrl(p);
}

void svcrt_event_set(int32 handle)
{
    uint32 p[3];
    p[0] = 3;
    p[1] = handle;
    svcrt_call_event_ctrl(p);
}
