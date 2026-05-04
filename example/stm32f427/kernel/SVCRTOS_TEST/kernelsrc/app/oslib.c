/**
* @brief SVCrtOS 操作系统内核功能接口文件
* @details 供应用程序(分区)使用的OS接口封装，通过SVC指令陷入内核态
* @author xw
* @date 2026.05.03
*/

#include "svcrt.h"

int32  __svc(0x10)  svcrt_call_dev_io(uint32 *p);
void   __svc(0x11)  svcrt_call_task_ctrl(uint32 fn, uint32 p);
uint32 __svc(0x12)  svcrt_call_sys_info(uint32 fn);
int32  __svc(0x13)  svcrt_call_event_ctrl(uint32 *p);

int32 svcrt_dev_open(char *name, uint32 param)
{
    uint32 parameters[4];

    parameters[0] = 1;
    parameters[1] = (uint32)name;
    parameters[2] = param;

    return svcrt_call_dev_io(parameters);
}

int32 svcrt_dev_read(int32 handle, void *pdata, int32 len)
{
    uint32 parameters[4];

    parameters[0] = 2;
    parameters[1] = handle;
    parameters[2] = (uint32)pdata;
    parameters[3] = len;

    return svcrt_call_dev_io(parameters);
}

int32 svcrt_dev_write(int32 handle, void *pdata, int32 len)
{
    uint32 parameters[4];

    parameters[0] = 3;
    parameters[1] = handle;
    parameters[2] = (uint32)pdata;
    parameters[3] = len;

    return svcrt_call_dev_io(parameters);
}

int32 svcrt_dev_close(int32 handle)
{
    uint32 parameters[4];

    parameters[0] = 5;
    parameters[1] = handle;

    return svcrt_call_dev_io(parameters);
}

int32 svcrt_dev_ctrl(int32 handle, uint32 code, uint32 value)
{
    uint32 parameters[4];

    parameters[0] = 4;
    parameters[1] = handle;
    parameters[2] = code;
    parameters[3] = value;

    return svcrt_call_dev_io(parameters);
}

__weak void AppMain(void)
{
}

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

void svcrt_task_kill(void)
{
    svcrt_call_task_ctrl(4, 0);
}

uint32 svcrt_get_time_ms(void)
{
    return svcrt_call_sys_info(1);
}

uint32 svcrt_get_cpu_usage(void)
{
    return svcrt_call_sys_info(2);
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
