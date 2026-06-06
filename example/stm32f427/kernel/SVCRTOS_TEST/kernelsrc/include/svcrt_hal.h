/**
* @brief SVCrtOS 硬件抽象层接口
* @details 定义内核与硬件的完整解耦接口，分为五个层次：
*          1) CPU指令层 - CPU核心指令封装（WFI/ISB/DSB等）
*          2) 上下文层 - 任务上下文结构与栈帧初始化
*          3) 中断控制层 - 中断开关、优先级、任务切换触发
*          4) 定时器层 - 系统节拍定时器与微秒延时
*          5) 板级层 - 板卡初始化与设备注册
*
*          所有架构相关实现均在 port/ 目录下，
*          内核代码不包含任何MCU头文件，不直接操作任何寄存器。
* @author xw
* @date 2026.05.03
*/

#ifndef __SVCRT_HAL_H__
#define __SVCRT_HAL_H__

#include "svcrt_types.h"
#include "svcrt_config.h"

/* ============================================================
 * CPU指令层
 * ============================================================ */
void svcrt_port_wfi(void);
void svcrt_port_wfe(void);
void svcrt_port_nop(void);
void svcrt_port_isb(void);
void svcrt_port_dsb(void);
void svcrt_port_dmb(void);

#define SVCRT_WFI()            svcrt_port_wfi()
#define SVCRT_WFE()            svcrt_port_wfe()
#define SVCRT_NOP()            svcrt_port_nop()
#define SVCRT_ISB()            svcrt_port_isb()
#define SVCRT_DSB()            svcrt_port_dsb()
#define SVCRT_DMB()            svcrt_port_dmb()

/* ============================================================
 * 中断控制层
 * ============================================================ */
void svcrt_port_disable_irq(void);
void svcrt_port_enable_irq(void);
void svcrt_port_switch_task(void);

#define SVCRT_DISABLE_IRQ()    svcrt_port_disable_irq()
#define SVCRT_ENABLE_IRQ()     svcrt_port_enable_irq()
#define SVCRT_SWITCH_TASK()    svcrt_port_switch_task()

/* ============================================================
 * 上下文层
 * ============================================================ */
void svcrt_port_set_psp(uint32 val);
uint32 svcrt_port_get_control(void);
void svcrt_port_set_control(uint32 val);
uint32 svcrt_port_stack_init(uint32 stack_top, void (*entry)(void));
void svcrt_port_enter_idle(uint32 psp, uint32 use_priv);

/* ============================================================
 * 定时器层
 * ============================================================ */
uint32 svcrt_port_get_system_clock(void);
uint32 svcrt_port_get_systick_val(void);
uint32 svcrt_port_get_systick_load(void);
void svcrt_port_start_timer(uint32 tick_period_us);
void svcrt_port_delay_us(uint32 us);

/* ============================================================
 * 板级层（弱定义，由board层覆盖）
 * ============================================================ */
void svcrt_port_board_init(void);
void svcrt_port_irq_init(void);
void svcrt_port_enable_fpu(void);
void svcrt_port_set_idle_mpu(uint32 task_func, uint32 stack_addr, uint32 stack_size);

/* ============================================================
 * MPU接口层
 * ============================================================ */
#if (SVCRT_USE_MPU == 1)

void svcrt_port_mpu_init(void);
void svcrt_port_mpu_set_region(uint32 rom_addr, uint32 rom_size, uint32 ram_addr, uint32 ram_size);
void svcrt_port_mpu_set_app(uint32 *mpu_bar, uint32 *mpu_asr);
void svcrt_port_mpu_reset(void);

#else

#define svcrt_port_mpu_init()
#define svcrt_port_mpu_set_region(rom_addr, rom_size, ram_addr, ram_size)
#define svcrt_port_mpu_set_app(mpu_bar, mpu_asr)
#define svcrt_port_mpu_reset()

#endif

#endif
