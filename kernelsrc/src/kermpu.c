/**
* @brief SVCrtOS 内存保护单元
* @author chenjl
* @date 2019.07.04
*/

#include "kermpu.h"

#if (SVCRT_USE_MPU == 1)

void kerMpuMdlInit(void)
{
    int32 i;
    if(MPU->TYPE == 0)
    {
        return ;
    }
    SVCRT_DMB();
    MPU->CTRL = 0;

    for(i=0;i<8;i++)
    {
        MPU->RNR = i;
        MPU->RBAR = 0;
        MPU->RASR = 0;
    }
    MPU->CTRL = MPU_CTRL_ENABLE_Msk | MPU_CTRL_PRIVDEFENA_Msk;
    SVCRT_DSB();
    SVCRT_ISB();
}

void kerSetAppMpu(TASK_CONTEXT *pApp)
{
    int32 rnr = 0;

    SVCRT_DMB();
    MPU->CTRL = 0;

    for(rnr = 0;rnr<4;rnr++)
    {
        MPU->RNR  = rnr;
        MPU->RBAR = pApp->mpu_bar[rnr];
        MPU->RASR = pApp->mpu_asr[rnr];
    }

    MPU->CTRL = MPU_CTRL_ENABLE_Msk | MPU_CTRL_PRIVDEFENA_Msk;
    SVCRT_DSB();
    SVCRT_ISB();
}

void kerMpuResetSetting(void)
{
    int32 rnr = 0;
    SVCRT_DMB();
    MPU->CTRL = 0;

    for(rnr=0;rnr<8;rnr++)
    {
        MPU->RNR  = rnr;
        MPU->RASR = 0;
    }

    MPU->CTRL = MPU_CTRL_ENABLE_Msk | MPU_CTRL_PRIVDEFENA_Msk;
    SVCRT_DSB();
    SVCRT_ISB();
}

void kerMpuSet(uint32 romAddr,uint32 romSize,uint32 ramAddr,uint32 ramSize)
{
    uint32 ramASR = 0x13060001;
    uint32 romASR = 0x06020001;
    int32  regidx;
    uint8  romRegion = 31;
    uint8  ramRegion = 31;
    if(MPU->TYPE == 0)
    {
        return ;
    }
    for(regidx=4;regidx<32;regidx++)
    {
        if((1<<(regidx+1)) >= romSize)
        {
            romRegion = regidx;
            break;
        }
    }

    for(regidx=4;regidx<32;regidx++)
    {
        if((1<<(regidx+1)) >= ramSize)
        {
            ramRegion = regidx;
            break;
        }
    }

    romAddr = romAddr & ~((1<<(1+romRegion))-1);
    ramAddr = ramAddr & ~((1<<(1+ramRegion))-1);

    SVCRT_DMB();
    MPU->CTRL = 0;
    MPU->RNR  = 0;
    MPU->RBAR = romAddr;
    MPU->RASR = romASR | (romRegion<<1);

    MPU->RNR  = 1;
    MPU->RBAR = ramAddr;
    MPU->RASR = ramASR | (ramRegion<<1);

    MPU->CTRL = MPU_CTRL_ENABLE_Msk | MPU_CTRL_PRIVDEFENA_Msk;
    SVCRT_DSB();
    SVCRT_ISB();
}

#endif
