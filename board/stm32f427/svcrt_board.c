/**
* @brief SVCrtOS ?èâ?????? - STM32F427
* @details ??? svcrt_port.h ?????????????????????
*          ????èâ?ıÙ??? svcrt_dev_board_init()
*
*          ???????ß‡??????€x??????????????????
*          ??????kernelsrc/???????¶ ¶»????
*/

#include "svcrt_port.h"
#include "svcrt_dev.h"
#include "svcrt_task.h"
#include "stm32f4xx.h"

#if (SVCRT_USE_MPU == 1)
#include "svcrt_mpu.h"
#endif

/* ============================================================
 * ?ßÿ????
 * ============================================================ */
void svcrt_port_disable_irq(void)
{
    __disable_irq();
}

void svcrt_port_enable_irq(void)
{
    __enable_irq();
}

/* ============================================================
 * ?????ß›?????
 * ============================================================ */
void svcrt_port_switch_task(void)
{
    SCB->ICSR = SCB_ICSR_PENDSVSET_Msk;
}

/* ============================================================
 * CPU ?????
 * ============================================================ */
void svcrt_port_wfi(void)  { __WFI(); }
void svcrt_port_wfe(void)  { __WFE(); }
void svcrt_port_nop(void)  { __NOP(); }
void svcrt_port_isb(void)  { __ISB(); }
void svcrt_port_dsb(void)  { __DSB(); }
void svcrt_port_dmb(void)  { __DMB(); }

/* ============================================================
 * ?????????????????
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

/* ============================================================
 * SysTick ???
 * ============================================================ */
uint32 svcrt_port_get_systick_val(void)
{
    return SysTick->VAL;
}

uint32 svcrt_port_get_systick_load(void)
{
    return SysTick->LOAD;
}

/* ============================================================
 * ???????????
 * ============================================================ */
void svcrt_port_board_init(void)
{
    SCB->CPACR |= (3 << 20) | (3 << 22);
}

void svcrt_port_irq_init(void)
{
    NVIC_SetPriorityGrouping(0);
    NVIC_SetPriority(PendSV_IRQn, 0xFF);
    NVIC_SetPriority(SysTick_IRQn, 0x00);
    NVIC_SetPriority(SVCall_IRQn, 0x01);
}

void svcrt_port_start_timer(uint32 tick_period_us)
{
    uint32 ticks = SystemCoreClock / 1000000 * tick_period_us / 8;
    SysTick_Config(ticks);
}

void svcrt_port_enable_fpu(void)
{
    FPU->FPCCR = FPU_FPCCR_ASPEN_Msk | FPU_FPCCR_LSPEN_Msk;
}

void svcrt_port_set_idle_mpu(uint32 task_func, uint32 stack_addr, uint32 stack_size)
{
    #if (SVCRT_USE_MPU == 1)
    svcrt_mpu_set(task_func, 0x1000, stack_addr, stack_size);
    #endif
}

/* ============================================================
 * ?èâ?ıÙ???
 * @brief ???????????????????????????
 *        ??????????drvuart.c, drvled.c ???????? board/ ??
 * ============================================================ */
extern svcrt_dev_drv_t usart_drv;
extern svcrt_dev_drv_t led_drv;

void svcrt_dev_board_init(void)
{
    svcrt_dev_register("COM1", &usart_drv, 0);
    svcrt_dev_register("LED",  &led_drv,  0);
}

/* ============================================================
 * ?ßÿ???????
 * @brief ARM Cortex-M ?ßÿ???????????????????
 *        ???????ß‡????????ß‡????ßÿ??????????????????
 * ============================================================ */
void SysTick_Handler(void)
{
    svcrt_kernel_tick_handler();
}

void HardFault_Handler(void)
{
    svcrt_hardfault_handler();
}
