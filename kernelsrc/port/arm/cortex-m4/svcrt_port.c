/**
* @brief SVCrtOS Cortex-M4 ????????
* @details ??? svcrt_hal.h ???????????§Þ??????????
*          ??????CPU????????§Ø????????????????
*          ????????????????MPU???????
*          ???????????CMSIS?????????????????¦Ê¦Ã???HAL??
* @author xw
* @date 2026.05.03
*/

#include "svcrt_hal.h"
#include "svcrt_task.h"
#include "svcrt_config.h"

#ifdef SVCRT_BOARD_CONFIG
#include SVCRT_BOARD_CONFIG
#endif

/* ============================================================
 * CPU????
 * ============================================================ */
void svcrt_port_wfi(void)  { __WFI(); }
void svcrt_port_wfe(void)  { __WFE(); }
void svcrt_port_nop(void)  { __NOP(); }
void svcrt_port_isb(void)  { __ISB(); }
void svcrt_port_dsb(void)  { __DSB(); }
void svcrt_port_dmb(void)  { __DMB(); }

/* ============================================================
 * ?§Ø?????
 * ============================================================ */
void svcrt_port_disable_irq(void)
{
    __disable_irq();
}

void svcrt_port_enable_irq(void)
{
    __enable_irq();
}

void svcrt_port_switch_task(void)
{
    SCB->ICSR = SCB_ICSR_PENDSVSET_Msk;
}

/* ============================================================
 * ???????
 * ============================================================ */
void svcrt_port_set_psp(uint32 val)
{
    __set_PSP(val);
}

uint32 svcrt_port_get_control(void)
{
    return __get_CONTROL();
}

void svcrt_port_set_control(uint32 val)
{
    __set_CONTROL(val);
}

uint32 svcrt_port_stack_init(uint32 stack_top, void (*entry)(void))
{
    uint32 *p_sp;

    p_sp = (uint32 *)stack_top;

    *(--p_sp) = 0x01000000;        /* xPSR: Thumb ¦Ë */
    *(--p_sp) = (uint32)entry;     /* PC */
    *(--p_sp) = 0;                 /* LR */
    *(--p_sp) = 0;                 /* R12 */
    *(--p_sp) = 0;                 /* R3 */
    *(--p_sp) = 0;                 /* R2 */
    *(--p_sp) = 0;                 /* R1 */
    *(--p_sp) = 0;                 /* R0 */

    /* ????????????R4-R11 + EXC_RETURN(LR)
     * EXC_RETURN = 0xFFFFFFFD: ???? Thread ??????? PSP?????? FPU ??????
     * PendSV ?§Ý????? LR ?? bit4 ?§Ø?????? S16-S31?? */
    *(--p_sp) = 0xFFFFFFFD;        /* LR (EXC_RETURN) */
    *(--p_sp) = 0;                 /* R11 */
    *(--p_sp) = 0;                 /* R10 */
    *(--p_sp) = 0;                 /* R9 */
    *(--p_sp) = 0;                 /* R8 */
    *(--p_sp) = 0;                 /* R7 */
    *(--p_sp) = 0;                 /* R6 */
    *(--p_sp) = 0;                 /* R5 */
    *(--p_sp) = 0;                 /* R4 */

    return (uint32)p_sp;
}

void svcrt_port_enter_idle(uint32 psp, uint32 use_priv)
{
    svcrt_port_set_psp(psp);

    if(use_priv)
    {
        svcrt_port_set_control(0x3 | svcrt_port_get_control());
    }
    else
    {
        svcrt_port_set_control(0x2 | svcrt_port_get_control());
    }
    svcrt_port_isb();
}

/* ============================================================
 * ???????
 * ============================================================ */
uint32 svcrt_port_get_system_clock(void)
{
    return (uint32)SystemCoreClock;
}

uint32 svcrt_port_get_systick_val(void)
{
    return SysTick->VAL;
}

uint32 svcrt_port_get_systick_load(void)
{
    return SysTick->LOAD;
}

void svcrt_port_start_timer(uint32 tick_period_us)
{
    uint32 ticks = (uint32)(((unsigned long long)SystemCoreClock * tick_period_us) / 1000000u);
    SysTick_Config(ticks);
}

void svcrt_port_delay_us(uint32 us)
{
    int32 tm_start = (int32)svcrt_port_get_systick_val();
    int32 wait_clk = (int32)(us * (SystemCoreClock / 1000000));
    int32 tm_end;
    int32 tm_diff;

    while(wait_clk > 0)
    {
        tm_end = (int32)svcrt_port_get_systick_val();
        tm_diff = tm_start - tm_end;
        if(tm_diff < 0)
            tm_diff += (int32)svcrt_port_get_systick_load();
        tm_start = tm_end;

        wait_clk -= tm_diff;
    }
}

/* ============================================================
 * ?‰???????ÈÉ??board?????
 * ============================================================ */
__weak void svcrt_port_board_init(void)
{
    SCB->CPACR |= (3UL << 20) | (3UL << 22);
    FPU->FPCCR = FPU_FPCCR_ASPEN_Msk;
}

__weak void svcrt_port_irq_init(void)
{
    NVIC_SetPriorityGrouping(0);
    NVIC_SetPriority(PendSV_IRQn,  0xFF);
    NVIC_SetPriority(SysTick_IRQn, 0x00);
    NVIC_SetPriority(SVCall_IRQn,  0x01);
}

__weak void svcrt_port_enable_fpu(void)
{
    FPU->FPCCR = FPU_FPCCR_ASPEN_Msk | FPU_FPCCR_LSPEN_Msk;
}

__weak void svcrt_port_set_idle_mpu(uint32 task_func, uint32 stack_addr, uint32 stack_size)
{
    #if (SVCRT_USE_MPU == 1)
    svcrt_port_mpu_set_region(task_func, 0x1000, stack_addr, stack_size);
    #else
    (void)task_func;
    (void)stack_addr;
    (void)stack_size;
    #endif
}

/* ============================================================
 * MPU????
 * ============================================================ */
#if (SVCRT_USE_MPU == 1)

void svcrt_port_mpu_init(void)
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

void svcrt_port_mpu_set_region(uint32 rom_addr, uint32 rom_size, uint32 ram_addr, uint32 ram_size)
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

void svcrt_port_mpu_set_app(uint32 *mpu_bar, uint32 *mpu_asr)
{
    int32 rnr = 0;

    SVCRT_DMB();
    MPU->CTRL = 0;

    for(rnr = 0; rnr < 4; rnr++)
    {
        MPU->RNR  = rnr;
        MPU->RBAR = mpu_bar[rnr];
        MPU->RASR = mpu_asr[rnr];
    }

    MPU->CTRL = MPU_CTRL_ENABLE_Msk | MPU_CTRL_PRIVDEFENA_Msk;
    SVCRT_DSB();
    SVCRT_ISB();
}

void svcrt_port_mpu_reset(void)
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

#endif
