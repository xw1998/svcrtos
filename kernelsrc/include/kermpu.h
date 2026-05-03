/**
* @brief SVCrtOS 内存保护单元
* @author chenjl
* @date 2019.07.04
*/

#ifndef __KERMPU_H__
#define __KERMPU_H__

#include "kerOs.h"
#include "kernel.h"
#include "svcrt_config.h"

#if (SVCRT_USE_MPU == 1)

extern void kerMpuMdlInit(void);
extern void kerMpuSet(uint32 romAddr,uint32 romSize,uint32 ramAddr,uint32 ramSize);
void kerSetAppMpu(TASK_CONTEXT *pApp);
void kerMpuResetSetting(void);

#else

#define kerMpuMdlInit()
#define kerMpuSet(romAddr,romSize,ramAddr,ramSize)
#define kerSetAppMpu(pApp)
#define kerMpuResetSetting()

#endif

#endif
