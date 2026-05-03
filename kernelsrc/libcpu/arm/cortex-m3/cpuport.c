/**
* @brief SVCrtOS Cortex-M3 “∆÷≤≤„ µœ÷
*/

#include "svcrt_port.h"

void svcrtPortBoardInit(void)
{
}

void svcrtPortIrqInit(void)
{
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_4);
    NVIC_SetPriority(PendSV_IRQn, 0xFF);
    NVIC_SetPriority(SysTick_IRQn, 0x00);
}

void svcrtPortStartTimer(uint32 tick_period_us)
{
    uint32 ticks = SystemCoreClock / 1000000 * tick_period_us / 8;
    SysTick_Config(ticks);
}

void svcrtPortEnableFpu(void)
{
}

void svcrtPortSetIdleMpu(uint32 task_func, uint32 stack_addr, uint32 stack_size)
{
}
