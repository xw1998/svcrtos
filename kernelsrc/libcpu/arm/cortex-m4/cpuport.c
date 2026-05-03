/**
* @brief SVCrtOS Cortex-M4 ÒÆÖ²²ãÊµÏÖ
*/

#include "svcrt_port.h"

void svcrtPortBoardInit(void)
{
    SCB->CPACR |= (3 << 20) | (3 << 22);
}

void svcrtPortIrqInit(void)
{
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_4);
    NVIC_SetPriority(PendSV_IRQn, 0xFF);
    NVIC_SetPriority(SysTick_IRQn, 0x00);
    NVIC_SetPriority(SVCall_IRQn, 0x01);
}

void svcrtPortStartTimer(uint32 tick_period_us)
{
    uint32 ticks = SystemCoreClock / 1000000 * tick_period_us / 8;
    SysTick_Config(ticks);
}

void svcrtPortEnableFpu(void)
{
    FPU->FPCCR = FPU_FPCCR_ASPEN_Msk | FPU_FPCCR_LSPEN_Msk;
}

void svcrtPortSetIdleMpu(uint32 task_func, uint32 stack_addr, uint32 stack_size)
{
    kerMpuSet(task_func, 0x1000, stack_addr, stack_size);
}
