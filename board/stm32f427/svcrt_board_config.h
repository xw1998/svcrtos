/**
* @brief SVCrtOS 板级配置 - STM32F427
* @details 覆盖内核默认配置，适配 STM32F427 硬件特性
*          通过编译选项 -DSVCRT_BOARD_CONFIG=\"svcrt_board_config.h\" 包含
*          此文件同时包含CMSIS设备头文件，供port层使用寄存器定义
*/

#ifndef __SVCRT_BOARD_CONFIG_H__
#define __SVCRT_BOARD_CONFIG_H__

#include "stm32f4xx.h"

#undef  SVCRT_CPU_ARCH
#define SVCRT_CPU_ARCH            SVCRT_ARCH_CORTEX_M4

#define SVCRT_SYSTEM_CLOCK_HZ     (168000000)

#define SVCRT_SHARE_MEM_SIZE      (0x8000)

#undef  SVCRT_USE_FPU
#define SVCRT_USE_FPU             1

#undef  SVCRT_USE_MPU
#define SVCRT_USE_MPU             0

#undef  SVCRT_USE_PRIV
#define SVCRT_USE_PRIV            0

#endif
