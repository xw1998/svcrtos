/**
* @brief SVCrtOS 硬件移植接口
* @details 此文件是SVCrtOS与具体MCU之间的唯一耦合点。
*          用户必须根据目标平台实现此文件中的所有宏、函数和变量。
*          内核代码不直接包含任何MCU头文件，所有硬件访问均通过此文件抽象。
*
* 使用方法：将此文件复制到用户工程目录，修改为适配目标MCU的实现
*/

#ifndef __SVCRT_PORT_H__
#define __SVCRT_PORT_H__

#include "svcrt_config.h"

/* ============================================================
 * [必须实现] 包含目标MCU的头文件
 * ============================================================ */
#include "stm32f4xx.h"

/* ============================================================
 * [必须实现] 基础类型定义
 * ============================================================ */
typedef unsigned long  uint32;
typedef signed   long  int32;
typedef unsigned short uint16;
typedef signed   short int16;
typedef unsigned char  uint8;
typedef signed   char  int8;

/* ============================================================
 * [必须实现] 中断控制
 * ============================================================ */
#define SVCRT_DISABLE_IRQ()    __disable_irq()
#define SVCRT_ENABLE_IRQ()     __enable_irq()

/* ============================================================
 * [必须实现] 任务切换触发
 * @brief 设置PendSV挂起位，触发上下文切换
 * ============================================================ */
#define SVCRT_SWITCH_TASK()    (SCB->ICSR = SCB_ICSR_PENDSVSET_Msk)

/* ============================================================
 * [必须实现] CPU指令封装
 * ============================================================ */
#define SVCRT_WFI()            __WFI()
#define SVCRT_WFE()            __WFE()
#define SVCRT_ISB()            __ISB()
#define SVCRT_DSB()            __DSB()
#define SVCRT_DMB()            __DMB()
#define SVCRT_NOP()            __NOP()

/* ============================================================
 * [必须实现] 栈指针与控制寄存器操作
 * ============================================================ */
#define SVCRT_SET_PSP(val)     __set_PSP(val)
#define SVCRT_GET_CONTROL()    __get_CONTROL()
#define SVCRT_SET_CONTROL(val) __set_CONTROL(val)

/* ============================================================
 * [必须实现] 系统主频
 * @brief 必须在移植层定义此变量，单位Hz
 * ============================================================ */
extern uint32_t SystemCoreClock;

/* ============================================================
 * [必须实现] 移植层回调函数
 * @brief 用户需实现这些函数，由内核在启动时调用
 * ============================================================ */

/**
* @brief 硬件板卡初始化
* @details 在C运行时初始化之前调用，用于使能FPU等必须在早期配置的硬件
*          如果使用FPU，必须在此函数中使能CP10/CP11
*/
void svcrtPortBoardInit(void);

/**
* @brief 中断优先级配置
* @details 配置NVIC优先级分组，设置PendSV为最低优先级，SysTick优先级
*/
void svcrtPortIrqInit(void);

/**
* @brief 启动系统定时器
* @param tick_period_us 滴答周期(微秒)
* @details 配置SysTick定时器，使其按指定周期产生中断
*/
void svcrtPortStartTimer(uint32 tick_period_us);

/**
* @brief FPU使能
* @details 使能浮点运算单元，仅在SVCRT_USE_FPU=1时被调用
*/
void svcrtPortEnableFpu(void);

/**
* @brief 后台任务MPU保护设置
* @details 设置后台安全任务的MPU保护区域，仅在SVCRT_USE_MPU=1时被调用
* @param task_func 后台任务函数地址
* @param stack_addr 后台栈地址
* @param stack_size 栈大小
*/
void svcrtPortSetIdleMpu(uint32 task_func, uint32 stack_addr, uint32 stack_size);

/* ============================================================
 * [必须实现] SysTick相关（用于kerTaskDelay微秒延时）
 * ============================================================ */

static __inline uint32 svcrtPortGetSysTickVal(void)
{
    return SysTick->VAL;
}

static __inline uint32 svcrtPortGetSysTickLoad(void)
{
    return SysTick->LOAD;
}

#endif
