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

/* Task switching stays latched off until the board has moved the CPU to
 * the idle PSP context (svcrt_port_enter_idle) and calls
 * svcrt_port_switch_enable(). A switch requested before that runs while
 * the CPU still uses MSP, so the saved PSP is garbage and the PendSV
 * return would go back to the caller instead of starting the new task. */
static uint8 svcrt_port_switch_ready = 0u;

void svcrt_port_switch_task(void)
{
    if(svcrt_port_switch_ready != 0u)
    {
        SCB->ICSR = SCB_ICSR_PENDSVSET_Msk;
    }
}

/* Enable task switching and request the first switch. Call it after
 * svcrt_port_enter_idle() and svcrt_port_start_timer(). */
void svcrt_port_switch_enable(void)
{
    svcrt_port_switch_ready = 1u;
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
    /* The hardware pushes/loads the frame PC without its thumb bit. */
    *(--p_sp) = (uint32)entry & ~1u;  /* PC */
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

/* Move the CPU to the idle PSP context and pick the Thread-mode privilege.
 * CONTROL = 0x2 | CONTROL keeps MSP/PSP selection on the process stack
 * and clears nPRIV (privileged); 0x3 | CONTROL additionally sets nPRIV.
 * The per-task privilege of every later switch comes from
 * svcrt_port_set_thread_priv(), so this is only the privilege of the
 * idle context itself - kernel code, therefore privileged. */
void svcrt_port_enter_idle(uint32 stack_ptr, uint32 use_priv)
{
    svcrt_port_set_psp(stack_ptr);

    if(use_priv)
    {
        svcrt_port_set_control(0x2 | svcrt_port_get_control());
    }
    else
    {
        svcrt_port_set_control(0x3 | svcrt_port_get_control());
    }
    svcrt_port_isb();
}

/* Per-task privilege, applied on every switch: an unprivileged task must
 * become privileged again before the kernel task that follows it resumes.
 * Running in Handler mode is what makes both directions possible - the
 * write itself is always privileged there, so unprivileged code cannot
 * raise its own level. Only bit0 is touched and only when it differs. */
void svcrt_port_set_thread_priv(uint32 is_priv)
{
    uint32 ctrl = svcrt_port_get_control();
    uint32 want = (is_priv != 0u) ? (ctrl & ~1u) : (ctrl | 1u);

    if(want != ctrl)
    {
        svcrt_port_set_control(want);
        svcrt_port_isb();
    }
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

    if(ticks == 0u)
    {
        ticks = 1u;
    }

    /* 不能使用 SysTick_Config()：它会把 SysTick 优先级写成最低值，
     * 覆盖本移植层 svcrt_port_irq_init() 设定的“SysTick 最高、PendSV 最低”
     * 策略。优先级一旦被压低，tick 就可能被其它中断长时间压制，
     * 任务节拍与所有超时全部失真。这里只配置计数与使能，
     * 优先级统一由 svcrt_port_irq_init() 设定。 */
    SysTick->LOAD = ticks - 1u;
    SysTick->VAL  = 0u;
    SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk |
                    SysTick_CTRL_TICKINT_Msk   |
                    SysTick_CTRL_ENABLE_Msk;
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

#if (SVCRT_USE_MPU_APPLY_CACHE == 1)
/* 已生效的区域快照。切换时绝大多数情况区域集没变（同一 App 内的多个任务、
 * 内核任务之间、以及重复切回同一个任务），重复写 24 次寄存器 + 关/开 MPU +
 * 两个屏障纯属浪费。init()/reset() 会把硬件清空，那里必须作废快照。 */
static svcrt_arch_mpu_t svcrt_mpu_applied;
static uint8 svcrt_mpu_applied_ok = 0u;
#endif

/* Idle-task MPU context, captured by svcrt_port_mpu_set_region() at startup.
 * A task switch rewrites every region register, so the kernel re-applies this
 * context on each switch back to the idle task (svcrt_mpu_set_idle()). */
static svcrt_arch_mpu_t svcrt_mpu_idle_ctx;

void svcrt_port_mpu_init(void)
{
    int32 i;

    #if (SVCRT_USE_MPU_APPLY_CACHE == 1)
    svcrt_mpu_applied_ok = 0u;      /* 硬件已被清空，快照作废 */
    #endif

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

/* Smallest MPU SIZE field (RASR bits[5:1]) that covers `size` bytes.
 * An MPU region is a power of two and at least 32 bytes, so SIZE = log2(size) - 1.
 * The previous version searched with
 *     for(reg_idx = 4; reg_idx < 32; reg_idx++)
 *         if((1 << (reg_idx + 1)) >= size)
 * which reaches reg_idx == 31 when size > 2^31 and evaluates `1 << 32`, i.e.
 * undefined behaviour on a 32-bit int. This version clamps the input to 2GB,
 * uses unsigned shifts only, and keeps the shift count <= 31. */
static uint32 svcrt_mpu_size_field(uint32 size)
{
    uint32 field = 4u;                  /* smallest region: 32 bytes */

    if(size > 0x80000000u)
    {
        size = 0x80000000u;
    }

    while((field < 30u) && ((1u << (field + 1u)) < size))
    {
        field++;
    }

    return field;
}

/* Encode one region into the architecture independent context.
 * Returns 0 on success, -1 on a bad index or unknown attribute kind. */
int32 svcrt_port_mpu_encode(svcrt_arch_mpu_t *p_mpu, uint32 idx, uint32 base, uint32 size,
                            svcrt_mpu_mem_t mem)
{
    uint32 attr;
    uint32 region;

    if((p_mpu == 0) || (idx >= SVCRT_MPU_REGION_MAX) || (size == 0u))
    {
        return -1;
    }

    region = svcrt_mpu_size_field(size);

    /* A region base must be aligned to the region size: round down. */
    base = base & ~((1u << (1u + region)) - 1u);

    switch(mem)
    {
        case SVCRT_MPU_MEM_ROM:
            attr = 0x06020001u;     /* AP=0b110: read-only for both, XN=0 */
            break;

        case SVCRT_MPU_MEM_RAM:
            attr = 0x13060001u;     /* AP=0b011: read-write, XN=1 */
            break;

        case SVCRT_MPU_MEM_RAMX:
            /* Same attributes as MEM_RAM but with XN cleared: the MiniApp
             * block is the only place in this kernel where code lives in
             * RAM, so that one region has to be executable.
             * W^X does not hold inside it - accepted, documented tradeoff:
             * splitting it into an RO+X code region and an RW+XN data region
             * would need two power-of-two aligned MPU regions and waste up
             * to half of the block, which is exactly what a load-to-RAM
             * MiniApp cannot afford. The block is private to the MiniApp and
             * released when it exits (see docs/小程序设计.md). */
            attr = 0x03060001u;     /* AP=0b011: read-write, XN=0 */
            break;

        case SVCRT_MPU_MEM_PERIPH_RO:
            attr = 0x12060001u;     /* AP=0b010: unprivileged read-only, XN=1 */
            break;

        case SVCRT_MPU_MEM_PERIPH_RW:
            attr = 0x13060001u;     /* AP=0b011: unprivileged read-write, XN=1 */
            break;

        default:
            return -1;
    }

    p_mpu->region_base[idx] = base;
    p_mpu->region_attr[idx] = attr | (region << 1);
    return 0;
}

/* Startup path, called by the board through svcrt_port_set_idle_mpu():
 * capture the idle task windows (code + stack) and apply them. */
void svcrt_port_mpu_set_region(uint32 rom_addr, uint32 rom_size, uint32 ram_addr, uint32 ram_size)
{
    /* AP encoding on ARMv7-M (see CMSIS ARM_MPU_AP_FULL=3, ARM_MPU_AP_URO=2):
     * 0b011 = privileged and unprivileged read-write,
     * 0b010 = privileged read-write, unprivileged read-only,
     * 0b110 = read-only for both.
     * The data window must be writable by the task, so AP=0b011 with XN=1.
     * An earlier change had flipped it to 0b010 thinking 0b011 was read-only;
     * that would fault on an unprivileged task's first store to its own RAM. */
    if(MPU->TYPE == 0)
    {
        return;
    }

    (void)svcrt_port_mpu_encode(&svcrt_mpu_idle_ctx, 0u, rom_addr, rom_size, SVCRT_MPU_MEM_ROM);
    (void)svcrt_port_mpu_encode(&svcrt_mpu_idle_ctx, 1u, ram_addr, ram_size, SVCRT_MPU_MEM_RAM);
    svcrt_port_mpu_set_app(&svcrt_mpu_idle_ctx);
}

/* Re-apply the captured idle context (called on every switch to idle). */
void svcrt_port_mpu_set_idle(void)
{
    svcrt_port_mpu_set_app(&svcrt_mpu_idle_ctx);
}

void svcrt_port_mpu_set_app(const svcrt_arch_mpu_t *p_mpu)
{
    #if (SVCRT_USE_MPU_APPLY_CACHE == 0)
    /* 旧行为：整个关掉 MPU、重写全部 8 个区域、再开 MPU + 两个屏障。 */
    int32 rnr = 0;

    SVCRT_DMB();
    MPU->CTRL = 0;

    /* svcrt_port_mpu_init()/reset() clear regions 0~7, so writing only the
     * first few would leave stale windows from the previous task in 4~7.
     * Clear them here as well. */
    for(rnr = 0; rnr < 8; rnr++)
    {
        MPU->RNR = rnr;

        if(rnr < (int32)SVCRT_MPU_REGION_MAX)
        {
            MPU->RBAR = p_mpu->region_base[rnr];
            MPU->RASR = p_mpu->region_attr[rnr];
        }
        else
        {
            MPU->RBAR = 0;
            MPU->RASR = 0;
        }
    }

    MPU->CTRL = MPU_CTRL_ENABLE_Msk | MPU_CTRL_PRIVDEFENA_Msk;
    SVCRT_DSB();
    SVCRT_ISB();
    #else
    /* 新行为：只写真正变了的那几个区域，都写完才做一次屏障。
     * 不再整个关掉 MPU：异常处理里跑的是特权态，PRIVDEFENA=1 下特权访问走
     * 默认存储器映射，重配期间没有任何非特权代码在跑，所以就地更新是安全的；
     * 改写一个已使能区域之前，按体系结构要求先写 RASR=0 把它关掉。
     * 上下文与已生效的完全一致时（同一 App 内的多个任务、内核任务之间、
     * 反复切回同一个任务），连一个寄存器都不用碰。 */
    int32 rnr = 0;
    uint8 dirty = 0u;

    if(svcrt_mpu_applied_ok != 0u)
    {
        uint8 same = 1u;

        for(rnr = 0; rnr < (int32)SVCRT_MPU_REGION_MAX; rnr++)
        {
            if((p_mpu->region_base[rnr] != svcrt_mpu_applied.region_base[rnr]) ||
               (p_mpu->region_attr[rnr] != svcrt_mpu_applied.region_attr[rnr]))
            {
                same = 0u;
                break;
            }
        }
        if(same != 0u)
        {
            return;
        }
    }

    for(rnr = 0; rnr < 8; rnr++)
    {
        uint32 base = 0u;
        uint32 attr = 0u;

        if(rnr < (int32)SVCRT_MPU_REGION_MAX)
        {
            base = p_mpu->region_base[rnr];
            attr = p_mpu->region_attr[rnr];
        }

        if((svcrt_mpu_applied_ok != 0u) &&
           (base == svcrt_mpu_applied.region_base[rnr]) &&
           (attr == svcrt_mpu_applied.region_attr[rnr]))
        {
            continue;
        }
        if((svcrt_mpu_applied_ok != 0u) &&
           ((svcrt_mpu_applied.region_attr[rnr] & 1u) != 0u))
        {
            MPU->RNR  = (uint32)rnr;
            MPU->RASR = 0u;
        }

        MPU->RNR  = (uint32)rnr;
        MPU->RBAR = base;
        MPU->RASR = attr;

        svcrt_mpu_applied.region_base[rnr] = base;
        svcrt_mpu_applied.region_attr[rnr] = attr;
        dirty = 1u;
    }

    svcrt_mpu_applied_ok = 1u;

    if(dirty != 0u)
    {
        SVCRT_DSB();
        SVCRT_ISB();
    }
    #endif
}


void svcrt_port_mpu_reset(void)
{
    int32 rnr = 0;

    #if (SVCRT_USE_MPU_APPLY_CACHE == 1)
    svcrt_mpu_applied_ok = 0u;      /* 硬件已被清空，快照作废 */
    #endif

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
