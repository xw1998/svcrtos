/**
* @brief SVCrtOS Cortex-M3 “∆÷≤≤„ µœ÷
*/

#include "svcrt_port.h"

void svcrt_port_board_init(void)
{
}

void svcrt_port_irq_init(void)
{
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_4);
    NVIC_SetPriority(PendSV_IRQn, 0xFF);
    NVIC_SetPriority(SysTick_IRQn, 0x00);
}

void svcrt_port_start_timer(uint32 tick_period_us)
{
    uint32 ticks = SystemCoreClock / 1000000 * tick_period_us / 8;
    SysTick_Config(ticks);
}

void svcrt_port_enable_fpu(void)
{
}

void svcrt_port_set_idle_mpu(uint32 task_func, uint32 stack_addr, uint32 stack_size)
{
}
