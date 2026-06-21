/**
* @brief SVCrtOS 内核集中配置文件
* @details 借鉴 RT-Thread 的 rtconfig.h 思路，把内核所有可裁剪、可调参数集中在此管理。
*          板级配置可通过 SVCRT_BOARD_CONFIG 宏指向的头文件覆盖这里的默认值。
* @note 本文件不包含任何芯片相关头文件，也不直接操作任何硬件寄存器。
*/

#ifndef __SVCRT_CONFIG_H__
#define __SVCRT_CONFIG_H__

/* ============================================================
 * CPU 架构选择
 * @brief 选择目标 CPU 架构，决定上下文切换等移植层的实现。
 *        实际实现由对应 port 目录提供，这里只做编译期选择。
 * ============================================================ */
#ifndef SVCRT_CPU_ARCH
#define SVCRT_CPU_ARCH            1
#endif

#define SVCRT_ARCH_CORTEX_M3      0
#define SVCRT_ARCH_CORTEX_M4      1
#define SVCRT_ARCH_CORTEX_M7      2

/* ============================================================
 * FPU 浮点单元使能
 * @brief 是否启用浮点单元，影响上下文切换时是否保存浮点寄存器。
 *        需与目标 CPU 实际硬件一致；M4F/M7 有 FPU，M3 无 FPU。
 * ============================================================ */
#ifndef SVCRT_USE_FPU
#define SVCRT_USE_FPU             1
#endif

/* ============================================================
 * MPU 内存保护单元使能
 * @brief 是否启用 MPU，实现任务间 ROM/RAM 空间隔离。
 *        需与目标 CPU 实际硬件一致；启用后每次切换会重配 MPU 区域。
 * ============================================================ */
#ifndef SVCRT_USE_MPU
#define SVCRT_USE_MPU             1
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
 * @brief 通过编译选项 -DSVCRT_BOARD_CONFIG=<chip_header.h> 指定，
 *        用于让板级配置覆盖本文件的默认参数。
 * ============================================================ */
#ifdef SVCRT_BOARD_CONFIG
#include SVCRT_BOARD_CONFIG
#endif

#endif
