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
    uint32 sp;

    (void)hfsr; (void)cfsr; (void)mmfar; (void)bfar;

    /* 必须交给内核故障处理：任务级恢复（重建栈帧重启该任务），
     * 连续故障达到 APP_CRASH_RESTART_MAX 时由 svcrt_loader_on_fault() 禁用该 App。
     * 此前这里直接 while(1)，导致上述围栏在真实硬件上永远不会执行。 */
    sp = svcrt_hardfault_handler();

    if(sp != 0u)
    {
        /* 内核已选出可运行任务并返回其栈指针：直接恢复并异常返回（本函数不返回） */
        svcrt_port_resume_task(sp);
    }

    while(1) { }
}

/* ============================================================
 * 其余 CPU 异常向量
 * @brief 统一交给内核故障处理（见 svcrt_cpu_fault_handler）
 * @details stm32f4xx_it.c 中 CubeMX 生成的同名函数已在源码内用 #if 0 屏蔽。
 *          这些向量的处理逻辑与本文件 HardFault_Handler 一致：
 *          内核返回可恢复的任务栈指针时，直接恢复该任务上下文并异常返回；
 *          返回 0（内核/中断上下文故障）时落回 while(1)，停机等调试器接管。
 * ============================================================ */
void MemManage_Handler(void)
{
    uint32 sp = svcrt_cpu_fault_handler(SVCRT_FAULT_MEMFAULT);

    if(sp != 0u)
    {
        /* 同上：故障恢复路径不依赖 PendSV，直接恢复目标任务上下文 */
        svcrt_port_resume_task(sp);
    }

    while(1) { }
}

void BusFault_Handler(void)
{
    uint32 sp = svcrt_cpu_fault_handler(SVCRT_FAULT_BUSFAULT);

    if(sp != 0u)
    {
        /* 同上：故障恢复路径不依赖 PendSV，直接恢复目标任务上下文 */
        svcrt_port_resume_task(sp);
    }

    while(1) { }
}

void UsageFault_Handler(void)
{
    uint32 sp = svcrt_cpu_fault_handler(SVCRT_FAULT_USGFAULT);

    if(sp != 0u)
    {
        /* 同上：故障恢复路径不依赖 PendSV，直接恢复目标任务上下文 */
        svcrt_port_resume_task(sp);
    }

    while(1) { }
}
