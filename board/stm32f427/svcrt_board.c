/**
* @brief SVCrtOS ?弶?????? - STM32F427
* @details ??? svcrt_port.h ?????????????????????
*          ?弶?ж?????????豸???
*          ?????/???????/??????????/???ü???λ??
*          kernelsrc/src/svcrt_cfg.c,???????????????塣
* @author xw
* @date 2026.05.03
*/

#include "stm32f4xx.h"
#include "svcrt_port.h"
#include "svcrt_dev.h"
#include "svcrt_task.h"
#include "svcrt_config.h"

#if (SVCRT_USE_MPU == 1)
#include "svcrt_mpu.h"
#endif

/* ============================================================
 * ?ж????
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
 * ?????л?????:???? PendSV,?? PendSV_Handler ??????????л?
 * ============================================================ */
void svcrt_port_switch_task(void)
{
    SCB->ICSR = SCB_ICSR_PENDSVSET_Msk;
}

/* ============================================================
 * CPU ?????
 * ============================================================ */
void svcrt_port_wfi(void) { __WFI(); }
void svcrt_port_wfe(void) { __WFE(); }
void svcrt_port_nop(void) { __NOP(); }
void svcrt_port_isb(void) { __ISB(); }
void svcrt_port_dsb(void) { __DSB(); }
void svcrt_port_dmb(void) { __DMB(); }

/* ============================================================
 * ?????? CONTROL ?????
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
 * SysTick ?????(?? svcrt_task_delay ??????????)
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
 * ?弶?????:??? FPU Э?????????????
 * ============================================================ */
void svcrt_port_board_init(void)
{
    /* ??? CP10/CP11 ???????? */
    SCB->CPACR |= (3UL << 20) | (3UL << 22);
}

/* ============================================================
 * ?ж???????????
 * @note PendSV ?????????????????,??????????????????ж?;
 *       SVCall ???? SysTick ???????????????????
 * ============================================================ */
void svcrt_port_irq_init(void)
{
    NVIC_SetPriorityGrouping(0);
    NVIC_SetPriority(PendSV_IRQn,  0xFF);
    NVIC_SetPriority(SysTick_IRQn, 0x00);
    NVIC_SetPriority(SVCall_IRQn,  0x01);
}

/* ============================================================
 * 启动系统节拍定时器
 * @param tick_period_us 节拍周期(us)
 * @note CMSIS 的 SysTick_Config() 内部设置 CLKSOURCE=1,
 *       即 SysTick 使用 HCLK 作为时钟源(不分频),
 *       因此 ticks = SystemCoreClock * tick_period_us / 1000000
 * ============================================================ */
void svcrt_port_start_timer(uint32 tick_period_us)
{
    /* 先乘后除避免精度损失(使用 unsigned long long 防溢出) */
    uint32 ticks = (uint32)(((unsigned long long)SystemCoreClock * tick_period_us) / 1000000u);
    SysTick_Config(ticks);
}

/* ============================================================
 * FPU ???:???????????????????
 * ============================================================ */
void svcrt_port_enable_fpu(void)
{
    FPU->FPCCR = FPU_FPCCR_ASPEN_Msk | FPU_FPCCR_LSPEN_Msk;
}

/* ============================================================
 * ???/???????? MPU ????????
 * ============================================================ */
void svcrt_port_set_idle_mpu(uint32 task_func, uint32 stack_addr, uint32 stack_size)
{
    #if (SVCRT_USE_MPU == 1)
    svcrt_mpu_set(task_func, 0x1000, stack_addr, stack_size);
    #else
    (void)task_func;
    (void)stack_addr;
    (void)stack_size;
    #endif
}

/* ============================================================
 * ?????豸???
 * @brief ????????豸????????????????;
 *        ???????????λ?? drvuart.c / drvled.c ???????
 * ============================================================ */
extern svcrt_dev_drv_t usart_drv;
extern svcrt_dev_drv_t led_drv;

void svcrt_dev_board_init(void)
{
    svcrt_dev_register("COM1", &usart_drv, 0);
    svcrt_dev_register("LED",  &led_drv,  0);
}

/* ============================================================
 * ?弶?ж????
 * @brief Cortex-M ????ж???????,?????????????????
 *        ???? board ???????????????ж????????
 * ============================================================ */
void SysTick_Handler(void)
{
    svcrt_kernel_tick_handler();
}

/**
 * @brief HardFault 中断入口
 * @note  Debug 时若 PC 卡在此处的 while(1),说明发生了硬件错误。
 *        可在 Keil → View → System Viewer → Core Peripherals → SCB
 *        查看 HFSR/CFSR/MMFAR/BFAR 寄存器定位故障原因。
 *        典型原因:
 *          - INVSTATE: EPSR.T 位被清(跳到了 ARM 状态)
 *          - PRECISERR/IMPRECISERR: 总线错误(野指针访问)
 *          - STKERR/UNSTKERR: 异常入栈/出栈时栈指针非法
 *          - NOCP: FPU 协处理器访问未使能
 */
void HardFault_Handler(void)
{
    /* 死循环便于调试器暂停查看故障寄存器
     * 注释下面的 while(1) 即可调用内核默认处理(挂掉当前任务并切换) */
    volatile uint32 hfsr  = SCB->HFSR;
    volatile uint32 cfsr  = SCB->CFSR;
    volatile uint32 mmfar = SCB->MMFAR;
    volatile uint32 bfar  = SCB->BFAR;
    (void)hfsr; (void)cfsr; (void)mmfar; (void)bfar;
    while(1) { }

    /* 内核默认处理(当前不可达,需要时移除上面的 while(1)) */
    /* svcrt_hardfault_handler(); */
}
