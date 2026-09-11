/**
* @brief SVCrtOS ?潩????? - STM32F427
* @details ????port???????潩???????潩?????????
*          - FPU???????CPACR + FPCCR??
*          - NVIC?????????
*          - ?潩???COM1?????LED??????
*          - ?潩???????
*          ?????????CPU??????????潩??????
*          port/arm/cortex-m4/ ??????board?????????
* @author xw
* @date 2026.05.03
*/

#include "stm32f4xx.h"
#include "svcrt_hal.h"
#include "svcrt_dev.h"
#include "svcrt_task.h"
#include "svcrt_config.h"
#include "drvled.h"
#include "drvuart.h"

/* ============================================================
 * ?潩????? - ????port????????
 * ============================================================ */
void svcrt_port_board_init(void)
{
    SCB->CPACR |= (3UL << 20) | (3UL << 22);
    FPU->FPCCR = FPU_FPCCR_ASPEN_Msk;
}

/* ============================================================
 * ?潩??????????? - ????port????????
 * ============================================================ */
void svcrt_port_irq_init(void)
{
    NVIC_SetPriorityGrouping(0);
    NVIC_SetPriority(PendSV_IRQn,  0xFF);
    NVIC_SetPriority(SysTick_IRQn, 0x00);
    NVIC_SetPriority(SVCall_IRQn,  0x01);
}

/* ============================================================
 * FPU???? - ????port????????
 * @note ?????? svcrt_port_board_init()???????????????
 * ============================================================ */
void svcrt_port_enable_fpu(void)
{
    FPU->FPCCR = FPU_FPCCR_ASPEN_Msk | FPU_FPCCR_LSPEN_Msk;
}

/* ============================================================
 * ????????MPU???? - ????port????????
 * ============================================================ */
void svcrt_port_set_idle_mpu(uint32 task_func, uint32 stack_addr, uint32 stack_size)
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
 * ?潩???
 * ============================================================ */
extern svcrt_dev_drv_t usart_drv;
extern svcrt_dev_drv_t led_drv;

void svcrt_dev_board_init(void)
{
    svcrt_dev_register("LED",  &led_drv, LED_ID_RED);
    svcrt_dev_register("LED2", &led_drv, LED_ID_GREEN);
    svcrt_dev_register("COM1", &usart_drv, 0);
}

/* ============================================================
 * ?潩???????
 * @brief Cortex-M ?潩?????????????????潩??潩board??
*         ???潩??????????????????
 * ============================================================ */
void SysTick_Handler(void)
{
    svcrt_kernel_tick_handler();
}

void HardFault_Handler(void)
{
    volatile uint32 hfsr  = SCB->HFSR;
    volatile uint32 cfsr  = SCB->CFSR;
    volatile uint32 mmfar = SCB->MMFAR;
    volatile uint32 bfar  = SCB->BFAR;
    (void)hfsr; (void)cfsr; (void)mmfar; (void)bfar;
    while(1) { }
}
