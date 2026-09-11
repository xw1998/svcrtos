/**
* @brief SVCrtOS Cortex-M4 移植层实现
* @details 实现 svcrt_hal.h 中声明的所有硬件抽象接口，包括：
*          CPU 控制、中断开关、寄存器访问、任务栈帧初始化、
*          系统时钟与微秒延时、MPU 配置等。
*          仅依赖 CMSIS 内核头文件，不依赖任何芯片厂商的 HAL 库。
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
 * CPU 控制
 * ============================================================ */
void svcrt_port_wfi(void)  { __WFI(); }
void svcrt_port_wfe(void)  { __WFE(); }
void svcrt_port_nop(void)  { __NOP(); }
void svcrt_port_isb(void)  { __ISB(); }
void svcrt_port_dsb(void)  { __DSB(); }
void svcrt_port_dmb(void)  { __DMB(); }

/* ============================================================
 * 原子操作与多核支持（自旋锁底层原语）
 * @brief Cortex-M3/M4 使用 LDREX/STREX 独占访问实现原子 CAS。
 *        单核系统 CPU ID 恒为 0；自旋提示用 NOP 降低总线压力。
 *        移植到 RISC-V / LoongArch 时，请用 AMO / LR-SC / LL-SC
 *        指令重写本组接口，内核与自旋锁实现无需任何改动。
 * ============================================================ */

/**
* @brief 比较并交换（原子操作）
* @param p_addr    目标地址（4 字节对齐）
* @param expect    期望值
* @param new_value 期望成立时写入的新值
* @return 1=成功，0=当前值不是 expect（未修改）
*/
uint32 svcrt_port_atomic_cas(volatile uint32 *p_addr, uint32 expect, uint32 new_value)
{
    uint32 old;

    do
    {
        old = __LDREXW((volatile uint32 *)p_addr);
        if(old != expect)
        {
            __CLREX();
            return 0u;
        }
    } while(__STREXW(new_value, (volatile uint32 *)p_addr) != 0u);

    return 1u;
}

/**
* @brief 获取当前 CPU 编号
* @return 单核 MCU 固定返回 0，多核移植时返回硬件核号
*/
uint32 svcrt_port_cpu_id(void)
{
    return 0u;
}

/**
* @brief 自旋等待提示
* @note 单核无总线争用，用 NOP 即可；多核可改为 __WFE() 降低功耗与争用。
*/
void svcrt_port_spin_hint(void)
{
    __NOP();
}


/* ============================================================
 * 中断开关
 * ============================================================ */
uint8 svcrt_port_in_isr(void)
{
    return (__get_IPSR() != 0) ? 1 : 0;
}

uint32 svcrt_port_syscall_num(void *p_exc_ctx)
{
    /* Cortex-M: SVC 指令带 8 位立即数作为系统调用号，
     * 位于硬件压栈的栈帧返回地址 PC 之前 1 字节。
     * 对应 SVCRT_ARCH_SVC_NUM_BITS = 8。 */
    uint32 *p_frame = (uint32 *)p_exc_ctx;
    /* 栈帧中 PC 位于索引 6（R0,R1,R2,R3,R12,LR,PC,xPSR），
     * SVC 指令的低 8 位立即数位于 PC 之前 1 字节。 */
    return (uint32)((char *)p_frame[6])[-2];
}

void svcrt_port_disable_irq(void)
{
    __disable_irq();
}

void svcrt_port_enable_irq(void)
{
    __enable_irq();
}

/**
* @brief 进入临界区，保存 PRIMASK 并关中断
* @return 进入前的中断状态
*/
uint32 svcrt_port_enter_critical(void)
{
    uint32 primask = __get_PRIMASK();
    __disable_irq();
    return primask;
}

/**
* @brief 退出临界区，恢复进入前的中断状态
*/
void svcrt_port_exit_critical(uint32 state)
{
    __set_PRIMASK(state);
}

/**
* @brief 读取系统调用参数（0~3，对应 R0~R3）
* @details Cortex-M 硬件压栈顺序为 R0,R1,R2,R3,R12,LR,PC,xPSR，
*          使用栈帧指针直接索引即可。
*/
uint32 svcrt_port_svc_get_arg(void *p_exc_ctx, uint32 idx)
{
    return ((uint32 *)p_exc_ctx)[idx];
}

/**
* @brief 写回系统调用返回值（R0）
*/
void svcrt_port_svc_set_ret(void *p_exc_ctx, uint32 value)
{
    ((uint32 *)p_exc_ctx)[0] = value;
}

void svcrt_port_switch_task(void)
{
    SCB->ICSR = SCB_ICSR_PENDSVSET_Msk;
}

/* ============================================================
 * 寄存器访问
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

    *(--p_sp) = 0x01000000;        /* xPSR: Thumb λ */
    *(--p_sp) = (uint32)entry;     /* PC */
    *(--p_sp) = 0;                 /* LR */
    *(--p_sp) = 0;                 /* R12 */
    *(--p_sp) = 0;                 /* R3 */
    *(--p_sp) = 0;                 /* R2 */
    *(--p_sp) = 0;                 /* R1 */
    *(--p_sp) = 0;                 /* R0 */

    /* 第二部分：上下文切换时手动保存/恢复的 R4-R11 + EXC_RETURN(LR)
     * EXC_RETURN = 0xFFFFFFFD: 异常返回到 Thread 模式、使用 PSP、且无 FPU 扩展帧。
     * PendSV 切换时通过 LR 的 bit4 判断是否需要保存/恢复 S16-S31。 */
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

void svcrt_port_enter_idle(uint32 stack_ptr, uint32 use_priv)
{
    svcrt_port_set_psp(stack_ptr);

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
 * 系统时钟与延时
 * ============================================================ */
uint32 svcrt_port_get_system_clock(void)
{
    return (uint32)SystemCoreClock;
}

uint32 svcrt_port_get_timer_counter(void)
{
    return SysTick->VAL;
}

uint32 svcrt_port_get_timer_reload(void)
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
    int32 tm_start = (int32)svcrt_port_get_timer_counter();
    int32 wait_clk = (int32)(us * (SystemCoreClock / 1000000));
    int32 tm_end;
    int32 tm_diff;

    while(wait_clk > 0)
    {
        tm_end = (int32)svcrt_port_get_timer_counter();
        tm_diff = tm_start - tm_end;
        if(tm_diff < 0)
            tm_diff += (int32)svcrt_port_get_timer_reload();
        tm_start = tm_end;

        wait_clk -= tm_diff;
    }
}

/* ============================================================
 * 板级初始化默认实现，可被 board 层覆盖
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
 * MPU 配置
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

void svcrt_port_mpu_set_app(const svcrt_arch_mpu_t *p_mpu)
{
    int32 rnr = 0;

    SVCRT_DMB();
    MPU->CTRL = 0;

    for(rnr = 0; rnr < 4; rnr++)
    {
        MPU->RNR  = rnr;
        MPU->RBAR = p_mpu->region_base[rnr];
        MPU->RASR = p_mpu->region_attr[rnr];
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
