/**
* @brief SVCrtOS 内核集中配置文件
* @details 类似RT-Thread的rtconfig.h，所有内核可配置参数集中在此。
*          板级配置通过 SVCRT_BOARD_CONFIG 宏包含的文件覆盖默认值。
* @note 此文件不包含任何芯片头文件，不依赖任何架构定义
*/

#ifndef __SVCRT_CONFIG_H__
#define __SVCRT_CONFIG_H__

/* ============================================================
 * CPU架构选择
 * @brief 由板级配置文件定义，内核不预设默认架构
 * ============================================================ */
#ifndef SVCRT_CPU_ARCH
#define SVCRT_CPU_ARCH            1
#endif

#define SVCRT_ARCH_CORTEX_M3      0
#define SVCRT_ARCH_CORTEX_M4      1
#define SVCRT_ARCH_CORTEX_M7      2

/* ============================================================
 * FPU 浮点单元配置
 * @brief 由板级配置文件根据实际硬件定义
 * ============================================================ */
#ifndef SVCRT_USE_FPU
#define SVCRT_USE_FPU             1
#endif

/* ============================================================
 * MPU 内存保护单元配置
 * @brief 由板级配置文件根据实际硬件定义
 * ============================================================ */
#ifndef SVCRT_USE_MPU
#define SVCRT_USE_MPU             1
#endif

/* ============================================================
 * 特权模式分离配置
 * @note 依赖MPU，MPU关闭时特权分离无效
 * ============================================================ */
#ifndef SVCRT_USE_PRIV
#if (SVCRT_USE_MPU == 1)
#define SVCRT_USE_PRIV            1
#else
#define SVCRT_USE_PRIV            0
#endif
#endif

/* ============================================================
 * 内核可裁剪功能配置
 * ============================================================ */
#define SVCRT_TASK_MAX_NUM        (7)
#define SVCRT_TICK_PERIOD_US      (500)
#define SVCRT_EVENT_NUM           (10)
#define SVCRT_MAX_EVENT_WAITERS   (4)

#define SVCRT_MS_TO_TICK(ms)      ((ms) * 1000 / SVCRT_TICK_PERIOD_US)

/* ============================================================
 * 设备驱动最大数量配置
 * ============================================================ */
#define SVCRT_DEV_MAX_NUM         (8)

/* ============================================================
 * CPU负载统计配置
 * ============================================================ */
#define SVCRT_USE_CPU_LOAD        1

/* ============================================================
 * 栈溢出检测配置
 * ============================================================ */
#define SVCRT_USE_STACK_CHECK     1
#define SVCRT_STACK_END_FLAG      (0xed01)

/* ============================================================
 * 共享内存配置
 * ============================================================ */
#ifndef SVCRT_SHARE_MEM_ADDR
#define SVCRT_SHARE_MEM_ADDR      (0x20028000)
#endif

#ifndef SVCRT_SHARE_MEM_SIZE
#define SVCRT_SHARE_MEM_SIZE      (0x8000)
#endif

/* ============================================================
 * 系统主频配置
 * @brief 内核通过 svcrt_port_get_system_clock() 获取实际主频
 * ============================================================ */
#ifndef SVCRT_SYSTEM_CLOCK_HZ
#define SVCRT_SYSTEM_CLOCK_HZ     (168000000)
#endif

/* ============================================================
 * 板级配置包含
 * ============================================================ */
#ifdef SVCRT_BOARD_CONFIG
#include SVCRT_BOARD_CONFIG
#endif

#endif
