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
#include "svcrt_fault.h"

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

    /* 使能 MemManage/BusFault/UsageFault：
     * Cortex-M 复位后这三个异常默认关闭，不使能的话 MPU 越权、非法访问、
     * 未定义指令一律升级成 HardFault，故障类型无法区分，
     * stm32f4xx_it.c 里的对应处理函数也永远不会被调用。 */
    SCB->SHCSR |= SCB_SHCSR_MEMFAULTENA_Msk |
                  SCB_SHCSR_BUSFAULTENA_Msk |
                  SCB_SHCSR_USGFAULTENA_Msk;
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

    /* 必须交给内核故障处理：任务级恢复（重建栈帧重启该任务），
     * 连续故障达到 APP_CRASH_RESTART_MAX 时由 svcrt_loader_on_fault() 禁用该 App。
     * 此前这里直接 while(1)，导致上述围栏在真实硬件上永远不会执行。 */
    svcrt_hardfault_handler();

    /* svcrt_hardfault_handler() 正常路径会经 SVCRT_SWITCH_TASK 切走，
     * 只有“内核上下文故障”会自行 while(1)；此处再兜底一次。 */
    while(1) { }
}

/* ============================================================
 * 其余 CPU 异常向量
 * @brief 统一交给内核故障处理（见 svcrt_cpu_fault_handler）
 * @details stm32f4xx_it.c 中 CubeMX 生成的同名函数已在源码内用 #if 0 屏蔽。
 *          这些向量的处理逻辑与本文件 HardFault_Handler 一致：
 *          正常路径经 SVCRT_SWITCH_TASK 切走，末尾 while(1) 仅作兜底。
 * ============================================================ */
void MemManage_Handler(void)
{
    svcrt_cpu_fault_handler(SVCRT_FAULT_MEMFAULT);
    while(1) { }
}

void BusFault_Handler(void)
{
    svcrt_cpu_fault_handler(SVCRT_FAULT_BUSFAULT);
    while(1) { }
}

void UsageFault_Handler(void)
{
    svcrt_cpu_fault_handler(SVCRT_FAULT_USGFAULT);
    while(1) { }
}
