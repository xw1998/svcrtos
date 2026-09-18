/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : main.c 的公共定义（STM32F401 板）
  ******************************************************************************
  */
/* USER CODE END Header */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32f4xx_hal.h"

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* Private defines -----------------------------------------------------------*/
/* 本板外设引脚由板级驱动自己初始化：
 *   控制台 USART2  -> PA2(TX) / PA3(RX)  见 board/stm32f401/drvuart.c
 *   板载指示灯      -> PC13              见 board/stm32f401/drvled.c
 * 因此这里不再声明 CubeMX 生成的 *_Pin 宏。 */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
