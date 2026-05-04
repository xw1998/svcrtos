/**
* @brief SVCrtOS ??›C???????? - STM32F427
* @details ??MPU???????¨²????????
*          ?????????‰???????????ARM MPU?????
* @author xw
* @date 2026.05.03
*/

#include "stm32f4xx.h"
#include "svcrt_mpu.h"

#if (SVCRT_USE_MPU == 1)

void svcrt_mpu_module_init(void)
{
    int32 i;
    if(MPU->TYPE == 0)
    {
        return;
    }
    SVCRT_DMB();
    MPU->CTRL = 0;

    for(i = 0; i < 8; i++)
    {
        MPU->RNR = i;
        MPU->RBAR = 0;
        MPU->RASR = 0;
    }
    MPU->CTRL = MPU_CTRL_ENABLE_Msk | MPU_CTRL_PRIVDEFENA_Msk;
    SVCRT_DSB();
    SVCRT_ISB();
}

void svcrt_mpu_set_app(svcrt_task_t *p_app)
{
    int32 rnr = 0;

    SVCRT_DMB();
    MPU->CTRL = 0;

    for(rnr = 0; rnr < 4; rnr++)
    {
        MPU->RNR  = rnr;
        MPU->RBAR = p_app->mpu_bar[rnr];
        MPU->RASR = p_app->mpu_asr[rnr];
    }

    MPU->CTRL = MPU_CTRL_ENABLE_Msk | MPU_CTRL_PRIVDEFENA_Msk;
    SVCRT_DSB();
    SVCRT_ISB();
}

void svcrt_mpu_reset(void)
{
    int32 rnr = 0;
    SVCRT_DMB();
    MPU->CTRL = 0;

    for(rnr = 0; rnr < 8; rnr++)
    {
        MPU->RNR  = rnr;
        MPU->RASR = 0;
    }

    MPU->CTRL = MPU_CTRL_ENABLE_Msk | MPU_CTRL_PRIVDEFENA_Msk;
    SVCRT_DSB();
    SVCRT_ISB();
}

void svcrt_mpu_set(uint32 rom_addr, uint32 rom_size, uint32 ram_addr, uint32 ram_size)
{
    uint32 ram_asr = 0x13060001;
    uint32 rom_asr = 0x06020001;
    int32  reg_idx;
    uint8  rom_region = 31;
    uint8  ram_region = 31;

    if(MPU->TYPE == 0)
    {
        return;
    }

    for(reg_idx = 4; reg_idx < 32; reg_idx++)
    {
        if((1 << (reg_idx + 1)) >= rom_size)
        {
            rom_region = reg_idx;
            break;
        }
    }

    for(reg_idx = 4; reg_idx < 32; reg_idx++)
    {
        if((1 << (reg_idx + 1)) >= ram_size)
        {
            ram_region = reg_idx;
            break;
        }
    }

    rom_addr = rom_addr & ~((1 << (1 + rom_region)) - 1);
    ram_addr = ram_addr & ~((1 << (1 + ram_region)) - 1);

    SVCRT_DMB();
    MPU->CTRL = 0;
    MPU->RNR  = 0;
    MPU->RBAR = rom_addr;
    MPU->RASR = rom_asr | (rom_region << 1);

    MPU->RNR  = 1;
    MPU->RBAR = ram_addr;
    MPU->RASR = ram_asr | (ram_region << 1);

    MPU->CTRL = MPU_CTRL_ENABLE_Msk | MPU_CTRL_PRIVDEFENA_Msk;
    SVCRT_DSB();
    SVCRT_ISB();
}

#endif
