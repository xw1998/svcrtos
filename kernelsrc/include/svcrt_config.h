/**
* @brief SVCrtOS 内核集中配置文件
* @details 借鉴 RT-Thread 的 rtconfig.h 思路，把内核所有可裁剪、可调参数集中在此管理。
*          板级配置可通过 SVCRT_BOARD_CONFIG 宏指向的头文件覆盖这里的默认值。
* @note 本文件不包含任何芯片相关头文件，也不直接操作任何硬件寄存器。
*/

#ifndef __SVCRT_CONFIG_H__
#define __SVCRT_CONFIG_H__

/* ============================================================
 * 板级配置优先加载
 * @brief 板级配置可覆盖 SVCRT_CPU_ARCH / SVCRT_ARCH_CORE、主频、
 *        内存地址等参数，因此必须在架构派生与默认值之前加载。
 *        通过编译选项 -DSVCRT_BOARD_CONFIG=<chip_header.h> 指定。
 * ============================================================ */
#ifdef SVCRT_BOARD_CONFIG
#include SVCRT_BOARD_CONFIG
#endif

/* ============================================================
 * 自旋锁 / 临界区
 * @brief 内核与驱动侧自旋锁（svcrt_spin.h）
 * ============================================================ */
#ifndef SVCRT_USE_SPINLOCK
#define SVCRT_USE_SPINLOCK        1
#endif

/* ============================================================
 * 调度器锁（用户态临界区）
 * @brief 用户任务通过 SVC 调用 svcrt_sched_lock/unlock，
 *        禁止任务切换（不关中断），相当于 RT-Thread 的 rt_enter_critical
 * ============================================================ */
#ifndef SVCRT_USE_SCHED_LOCK
#define SVCRT_USE_SCHED_LOCK      1
#endif

/* ============================================================
 * 任务栈使用量分析
 * @brief 运行时统计每个任务的峰值栈用量（高水位法）：
 *        1) 创建时用 SVCRT_STACK_FILL_PATTERN 填充整段栈；
 *        2) 查询时扫描未被覆盖的图案区得到已用峰值；
 *        3) 上下文切换时记录最低栈指针，二者取大值。
 * ============================================================ */
#ifndef SVCRT_USE_STACK_USAGE
#define SVCRT_USE_STACK_USAGE     1
#endif

#ifndef SVCRT_STACK_FILL_PATTERN
#define SVCRT_STACK_FILL_PATTERN  (0xcdcdcdcd)
#endif

#include "svcrt_arch.h"

/* ============================================================
 * CPU 架构选择（架构抽象层）
 * @brief 架构族 / 具体核心 / 能力宏统一由 svcrt_arch.h 提供，
 *        本文件只负责据此派生内核功能开关的默认值。
 *        选择方式（二选一，板级配置或编译选项均可）：
 *          1) SVCRT_ARCH_CORE = SVCRT_CPU_CORE_xxx（推荐，支持 ARM/RISC-V/LoongArch）
 *          2) SVCRT_CPU_ARCH  = 0/1/2（兼容旧工程，仅 Cortex-M3/M4/M7）
 *        新架构实现放在 kernelsrc/port/<族>/<核心>/，详见 port/README.md
 * ============================================================ */

/* ============================================================
 * FPU 浮点单元使能
 * @brief 是否启用浮点单元，影响上下文切换时是否保存浮点寄存器。
 *        需与目标 CPU 实际硬件一致；M4F/M7 有 FPU，M3 无 FPU。
 * ============================================================ */
#ifndef SVCRT_USE_FPU
#define SVCRT_USE_FPU             SVCRT_ARCH_HAS_FPU
#endif

/* ============================================================
 * MPU 内存保护单元使能
 * @brief 是否启用 MPU，实现任务间 ROM/RAM 空间隔离。
 *        需与目标 CPU 实际硬件一致；启用后每次切换会重配 MPU 区域。
 * ============================================================ */
#ifndef SVCRT_USE_MPU
#define SVCRT_USE_MPU             SVCRT_ARCH_HAS_MPU
#endif

/* ============================================================
 * 特权级分离使能
 * @note 依赖 MPU；未启用 MPU 时特权分离无意义，自动关闭。
 * ============================================================ */
#ifndef SVCRT_USE_PRIV
#if (SVCRT_USE_MPU == 1)
#define SVCRT_USE_PRIV            1
#else
#define SVCRT_USE_PRIV            0
#endif
#endif

/* ============================================================
 * 任务与调度相关参数
 * ============================================================ */
#define SVCRT_TASK_MAX_NUM        (7)
#define SVCRT_TICK_PERIOD_US      (500)
#define SVCRT_EVENT_NUM           (10)
#define SVCRT_MAX_EVENT_WAITERS   (4)

/* 信号量、互斥锁及其等待者数量 */
#define SVCRT_SEM_NUM             (8)
#define SVCRT_MTX_NUM             (8)
#define SVCRT_MAX_SYNC_WAITERS    (4)

#define SVCRT_MS_TO_TICK(ms)      ((ms) * 1000 / SVCRT_TICK_PERIOD_US)

/* ============================================================
 * 设备框架最大设备数
 * ============================================================ */
#define SVCRT_DEV_MAX_NUM         (8)

/* ============================================================
 * 消息队列配置
 * ============================================================ */
#define SVCRT_USE_MQ              1
#define SVCRT_MQ_NUM              (8)
#define SVCRT_MQ_DEPTH            (8)
#define SVCRT_MQ_MSG_WORDS        (4)

/* ============================================================
 * 软定时器配置
 * ============================================================ */
#define SVCRT_USE_TIMER           1
#define SVCRT_TIMER_NUM           (8)
#define SVCRT_TIMER_TASK_PRI      (200)
#define SVCRT_TIMER_TASK_STACK_WORDS  (96)

/* ============================================================
 * 故障记录与任务恢复配置
 * ============================================================ */
#define SVCRT_USE_FAULT_RECOVER   1
#define SVCRT_FAULT_RECORD_NUM    (8)

/* ============================================================
 * CPU 负载统计开关
 * ============================================================ */
#define SVCRT_USE_CPU_LOAD        1

/* ============================================================
 * 栈溢出检测开关与栈底标志值
 * ============================================================ */
#define SVCRT_USE_STACK_CHECK     1
#define SVCRT_STACK_END_FLAG      (0xed01)

/* ============================================================
 * 共享内存配置
 * @brief 默认值，通常由板级配置文件 board/svcrt_board_config.h
 *        根据具体芯片的内存布局覆盖。
 * ============================================================ */
#ifndef SVCRT_SHARE_MEM_ADDR
#define SVCRT_SHARE_MEM_ADDR      (0x20028000)
#endif

#ifndef SVCRT_SHARE_MEM_SIZE
#define SVCRT_SHARE_MEM_SIZE      (0x8000)
#endif

/* ============================================================
 * 系统主频配置
 * @brief 默认值，通常由板级配置文件覆盖。
 *        svcrt_port_get_system_clock() 返回该频率，
 *        移植时一般与 CMSIS 的 SystemCoreClock 一致。
 * ============================================================ */
#ifndef SVCRT_SYSTEM_CLOCK_HZ
#define SVCRT_SYSTEM_CLOCK_HZ     (168000000)
#endif

/* ============================================================
 * 板级配置文件挂载点
 * @brief 已移至本文件开头（架构选择之前）加载，
 *        以便板级配置能覆盖 SVCRT_ARCH_CORE 并参与能力派生。
 * ============================================================ */

#endif
