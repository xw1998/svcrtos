/**
* @brief SVCrtOS 硬件抽象层接口
* @details 定义内核与硬件的完整解耦接口，分为四个层次：
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
 * @brief CPU核心指令封装，由port层实现
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
 * @brief 中断开关与任务切换触发，由port层实现
 * ============================================================ */
void svcrt_port_disable_irq(void);
void svcrt_port_enable_irq(void);
void svcrt_port_switch_task(void);

#define SVCRT_DISABLE_IRQ()    svcrt_port_disable_irq()
#define SVCRT_ENABLE_IRQ()     svcrt_port_enable_irq()
#define SVCRT_SWITCH_TASK()    svcrt_port_switch_task()

/* ============================================================
 * 上下文层
 * @brief 任务上下文操作，由port层实现
 *        不同CPU架构的寄存器布局、栈帧格式、
 *        上下文保存/恢复方式完全不同，
 *        全部由port层定义和实现。
 * ============================================================ */

/**
* @brief 设置进程栈指针PSP
* @param val PSP值
*/
void svcrt_port_set_psp(uint32 val);

/**
* @brief 获取CONTROL寄存器值
* @return CONTROL寄存器当前值
*/
uint32 svcrt_port_get_control(void);

/**
* @brief 设置CONTROL寄存器值
* @param val 要写入的值
*/
void svcrt_port_set_control(uint32 val);

/**
* @brief 初始化任务栈帧
* @param stack_top  栈顶地址（高地址，已8字节对齐）
* @param entry      任务入口函数
* @return           初始化后的栈指针（将存入task->stack_ptr）
* @details 不同架构的异常栈帧格式不同：
*          - ARM Cortex-M: 压入xPSR/PC/LR/R12/R3-R0(硬件) + R4-R11(软件)
*          - RISC-V: 压入mepc/mstatus/x1-x31
*          此函数由port层实现，内核无需关心具体布局
*/
uint32 svcrt_port_stack_init(uint32 stack_top, void (*entry)(void));

/**
* @brief 切换到空闲任务上下文
* @details 设置PSP/CONTROL寄存器，切换到PSP栈模式
*          不同架构的特权级切换机制不同，由port层实现
* @param psp        空闲任务栈顶地址
* @param use_priv   是否使用特权分离模式
*/
void svcrt_port_enter_idle(uint32 psp, uint32 use_priv);

/* ============================================================
 * 定时器层
 * @brief 系统节拍定时器与微秒延时，由port层实现
 * ============================================================ */

/**
* @brief 获取系统主频(Hz)
* @return 系统主频，单位Hz
* @details 不同平台获取方式不同：
*          - STM32: SystemCoreClock变量
*          - 其他MCU: 可能是宏或函数
*          内核通过此接口统一获取，不直接依赖SystemCoreClock
*/
uint32 svcrt_port_get_system_clock(void);

/**
* @brief 获取SysTick当前计数值
* @return SysTick->VAL 或等效值
*/
uint32 svcrt_port_get_systick_val(void);

/**
* @brief 获取SysTick重载值
* @return SysTick->LOAD 或等效值
*/
uint32 svcrt_port_get_systick_load(void);

/**
* @brief 启动系统节拍定时器
* @param tick_period_us 节拍周期（微秒）
*/
void svcrt_port_start_timer(uint32 tick_period_us);

/**
* @brief 微秒级忙等延时
* @param us 延时微秒数
* @details 使用硬件定时器实现精确延时，
*          由port层根据具体定时器实现
*/
void svcrt_port_delay_us(uint32 us);

/* ============================================================
 * 板级层
 * @brief 板卡初始化与设备注册，由board层实现
 * ============================================================ */

/**
* @brief 板卡硬件初始化
* @details 初始化CPU协处理器（如FPU）、时钟等
*/
void svcrt_port_board_init(void);

/**
* @brief 中断控制器初始化
* @details 配置NVIC优先级分组，设置PendSV/SysTick/SVC优先级
*/
void svcrt_port_irq_init(void);

/**
* @brief FPU使能
* @details 当 SVCRT_USE_FPU=1 时由port层实现
*/
void svcrt_port_enable_fpu(void);

/**
* @brief 设置空闲任务MPU区域
* @details 当 SVCRT_USE_MPU=1 时由port层实现
* @param task_func 任务函数地址
* @param stack_addr 栈地址
* @param stack_size 栈大小
*/
void svcrt_port_set_idle_mpu(uint32 task_func, uint32 stack_addr, uint32 stack_size);

/* ============================================================
 * MPU接口层
 * @brief MPU区域配置，由port层实现
*        当SVCRT_USE_MPU=0时，这些函数被宏替换为空
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
