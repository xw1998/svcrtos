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

uint8  svcrt_port_in_isr(void);
uint32 svcrt_port_syscall_num(void *p_exc_ctx);

/**
* @brief 读取系统调用上下文中的第 idx 个参数（0~3）
* @param p_exc_ctx 系统调用入口传给内核的上下文指针
* @param idx       参数序号（0 对应 a0/r0）
* @return 参数值
* @details 各架构的栈帧/trap 帧布局不同，由 port 层负责解析，
*          内核只通过本接口与上面的宏访问参数，不感知任何寄存器布局。
*/
uint32 svcrt_port_svc_get_arg(void *p_exc_ctx, uint32 idx);

/**
* @brief 写回系统调用返回值（第 0 个参数寄存器）
* @param p_exc_ctx 系统调用上下文指针
* @param value     返回值
*/
void   svcrt_port_svc_set_ret(void *p_exc_ctx, uint32 value);

/* 系统调用上下文统一访问宏：内核代码只使用这三个宏 */
#define SVCRT_SVC_ARG(p_ctx, idx)    svcrt_port_svc_get_arg((void *)(p_ctx), (uint32)(idx))
#define SVCRT_SVC_RET(p_ctx, val)    svcrt_port_svc_set_ret((void *)(p_ctx), (uint32)(val))
#define SVCRT_SVC_NUM(p_ctx)         svcrt_port_syscall_num((void *)(p_ctx))

#define SVCRT_DISABLE_IRQ()    svcrt_port_disable_irq()
#define SVCRT_ENABLE_IRQ()     svcrt_port_enable_irq()
#define SVCRT_SWITCH_TASK()    svcrt_port_switch_task()

/**
* @brief 进入临界区（保存中断使能状态并关中断）
* @return 进入前的中断状态，必须原样传给 svcrt_port_exit_critical
* @details 与 SVCRT_DISABLE_IRQ 的区别：本接口保存并恢复状态，
*          支持嵌套，且适配 RISC-V/LoongArch 等需要保存状态寄存器的架构。
*          新代码建议优先使用本接口。
*/
uint32 svcrt_port_enter_critical(void);

/**
* @brief 退出临界区（恢复此前保存的中断状态）
* @param state svcrt_port_enter_critical 的返回值
*/
void   svcrt_port_exit_critical(uint32 state);

#define SVCRT_ENTER_CRITICAL()       svcrt_port_enter_critical()
#define SVCRT_EXIT_CRITICAL(state)   svcrt_port_exit_critical(state)

/* ============================================================
 * 原子操作与多核支持
 * @brief 自旋锁（svcrt_spin.h）依赖的架构相关原语，由 port 层实现：
 *        - ARM Cortex-M：LDREX/STREX 独占访问
 *        - RISC-V：AMO 原子指令或 LR/SC 指令对
 *        - LoongArch：LL/SC 指令对
 *        单核移植可仅返回成功，但为将来 SMP 扩展建议直接实现原子语义。
 * ============================================================ */

/**
* @brief 比较并交换（原子操作）
* @param p_addr    目标地址（必须 4 字节对齐）
* @param expect    期望的当前值
* @param new_value 期望成立时写入的新值
* @return 1=交换成功，0=当前值与 expect 不符（未修改）
*/
uint32 svcrt_port_atomic_cas(volatile uint32 *p_addr, uint32 expect, uint32 new_value);

/**
* @brief 获取当前 CPU 编号（单核恒返回 0，多核返回硬件核号）
* @return CPU 编号
*/
uint32 svcrt_port_cpu_id(void);

/**
* @brief 自旋等待提示（降低自旋总线压力，可插入 NOP/WFE/PAUSE）
*/
void   svcrt_port_spin_hint(void);


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
void svcrt_port_enter_idle(uint32 stack_ptr, uint32 use_priv);

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
* @brief 获取系统节拍定时器当前计数值（自由递减计数器）
* @return 当前计数值
* @details ARM 为 SysTick->VAL，RISC-V 可为 mtime 低 32 位，
*          LoongArch 可为恒定频率定时器计数，由 port 层映射。
*/
uint32 svcrt_port_get_timer_counter(void);

/**
* @brief 获取系统节拍定时器重载值（计数上限）
* @return 重载值
* @details ARM 为 SysTick->LOAD，其他架构为等效周期值。
*/
uint32 svcrt_port_get_timer_reload(void);

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
void svcrt_port_mpu_set_app(const svcrt_arch_mpu_t *p_mpu);
void svcrt_port_mpu_reset(void);

#else

#define svcrt_port_mpu_init()
#define svcrt_port_mpu_set_region(rom_addr, rom_size, ram_addr, ram_size)
#define svcrt_port_mpu_set_app(p_mpu)
#define svcrt_port_mpu_reset()

#endif


/* ============================================================
 * 自旋锁 API（内核/驱动侧）
 * @brief svcrt_spin.h 基于本文件的原子接口实现，此处统一引入，
 *        便于内核与板级驱动代码只包含 svcrt_hal.h 即可使用。
 * ============================================================ */
/* ============================================================
 * Flash 接口层
 * @brief 片内 Flash 擦除/编程（供 Loader 写入 App 镜像）
 *        由 board 层实现（如 board/stm32f427/drvflash.c）。
 *        Flash 已映射到地址空间，读取不需要本组接口。
 * ============================================================ */

/**
* @brief 擦除 Flash 区间（按扇区擦除，自动对齐到扇区边界）
* @param addr 起始地址
* @param size 字节长度
* @return 0=成功，-1=失败
*/
int32  svcrt_port_flash_erase(uint32 addr, uint32 size);

/**
* @brief 写入 Flash（按字节编程，写入前该区间必须已擦除）
* @param addr 起始地址
* @param data 数据指针
* @param len  字节长度
* @return 0=成功，-1=失败
*/
int32  svcrt_port_flash_write(uint32 addr, const uint8 *data, uint32 len);

/**
* @brief 获取指定地址所在扇区的大小（字节）
* @param addr Flash 地址
* @return 扇区大小；地址非法返回 0
*/
uint32 svcrt_port_flash_sector_size(uint32 addr);

#include "svcrt_spin.h"

#endif
