/**
* @brief SVCrtOS 对外API头文件
* @details 供应用程序(分区)使用的操作系统接口定义
*          此文件属于App SDK，不依赖任何内核内部头文件
* @author chenjl
* @date 2019.07.01
*/

#ifndef __SVCRT_H__
#define __SVCRT_H__

#include "svcrt_types.h"

int32 dev_open(char* name,uint32 param);
int32 dev_read(int32 handle,void *pdata,int32 len);
int32 dev_write(int32 handle,void *pdata,int32 len);
int32 dev_ctrl(int32 handle,uint32 code,uint32 value);

void TaskWait(uint32 ms);
void TaskWaitNxtPeriod(void);
void TaskDelay(uint32 us);
void TaskKill(void);

uint32 GetSystemTimeMs(void);
uint32 GetCpuPayload(void);
int32 CreateEvent(char* name);
void WaitEvent(int32 handle,int32 timeout);
void SetEvent(int32 handle);

#endif
