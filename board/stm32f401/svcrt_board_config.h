/**
* @brief SVCrtOS 板级配置 - STM32F401
* @details 覆盖内核默认配置，适配 STM32F401 硬件特性
*          F401：Cortex-M4F / 最高 84MHz / 96KB SRAM / 无 CCM。
*          本板控制台 = USART2（PA2/PA3），LED = PC13。
*          通过编译选项 -DSVCRT_BOARD_CONFIG=\"svcrt_board_config.h\" 包含
*          此文件同时包含CMSIS设备头文件，供port层使用寄存器定义
*/

#ifndef __SVCRT_BOARD_CONFIG_H__
#define __SVCRT_BOARD_CONFIG_H__

#include "stm32f4xx.h"

#undef  SVCRT_CPU_ARCH
#define SVCRT_CPU_ARCH            SVCRT_ARCH_CORTEX_M4

#define SVCRT_SYSTEM_CLOCK_HZ     (84000000)

#define SVCRT_SHARE_MEM_SIZE      (0x8000)

#undef  SVCRT_USE_FPU
#define SVCRT_USE_FPU             1

#undef  SVCRT_USE_MPU
#define SVCRT_USE_MPU             0

#undef  SVCRT_USE_PRIV
#define SVCRT_USE_PRIV            0

/* Development bypass: let the kernel treat a raw image burned directly
 * at the slot base as runnable, so the fixed-address flash + MDK
 * breakpoint workflow keeps working. Release firmware must reset it to 0
 * and accept only CRC-checked .svcapp packages. */
#undef  APP_ALLOW_RAW_IMAGE
#define APP_ALLOW_RAW_IMAGE       1

#endif
