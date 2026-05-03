/**
* @brief 操作系统内核功能接口文件
* @details 供应用程序(分区)使用的OS接口封装，通过SVC指令陷入内核态
* @author chenjl
* @date 2019.07.01
*/

#include "anOs.h"

int32  __svc(0x10)  CallDevIo(uint32 *p);
void   __svc(0x11)  CallTimeFcn(uint32 fn,uint32 p);
uint32 __svc(0x12)  CallGetSysInfo(uint32 fn);
int32 __svc(0x13)   CallEventFcn(uint32 *p);

int32 dev_open(char* name,uint32 param)
{
    uint32 parameters[4];

    parameters[0] = 1;
    parameters[1] = (uint32)name;
    parameters[2] = param;

    return CallDevIo(parameters);
}

int32 dev_read(int32 handle,void *pdata,int32 len)
{
    uint32 parameters[4];

    parameters[0] = 2;
    parameters[1] = handle;
    parameters[2] = (uint32)pdata;
    parameters[3] = len;

    return CallDevIo(parameters);
}

int32 dev_write(int32 handle,void *pdata,int32 len)
{
    uint32 parameters[4];

    parameters[0] = 3;
    parameters[1] = handle;
    parameters[2] = (uint32)pdata;
    parameters[3] = len;

    return CallDevIo(parameters);
}

int32 dev_ctrl(int32 handle,uint32 code,uint32 value)
{
    uint32 parameters[4];

    parameters[0] = 4;
    parameters[1] = handle;
    parameters[2] = code;
    parameters[3] = value;

    return CallDevIo(parameters);
}

__weak void AppMain(void)
{
}

void TaskWait(uint32 ms)
{
    CallTimeFcn(1,ms);
}

void TaskWaitNxtPeriod(void)
{
    CallTimeFcn(2,0);
}

void TaskDelay(uint32 us)
{
    CallTimeFcn(3,us);
}

void TaskKill(void)
{
    CallTimeFcn(4,0);
}

uint32 GetSystemTimeMs(void)
{
    return CallGetSysInfo(1);
}

uint32 GetCpuPayload(void)
{
    return CallGetSysInfo(2);
}

int32 CreateEvent(char* name)
{
    uint32 p[3];
    p[0] = 1;
    p[1] = (uint32)name;
    return CallEventFcn(p);
}

void WaitEvent(int32 handle,int32 timeout)
{
    uint32 p[3];
    p[0] = 2;
    p[1] = handle;
    p[2] = timeout;
    CallEventFcn(p);
}

void SetEvent(int32 handle)
{
    uint32 p[3];
    p[0] = 3;
    p[1] = handle;
    CallEventFcn(p);
}
