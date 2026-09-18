/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    stm32f4xx_it.c
  * @brief   Interrupt Service Routines.（STM32F401 板）
  ******************************************************************************
  * @note  本文件只保留 CubeMX 生成的外壳。
  *        内核接管的中断/异常一律在本文件里用 #if 0 屏蔽，实体实现分两处：
  *          board/stm32f401/svcrt_board.c : SysTick_Handler / CPU 故障向量
  *          board/stm32f401/drvuart.c     : USART2_IRQHandler
  *        屏蔽 SysTick_Handler 很重要：HAL 的毫秒时基由板级 SysTick_Handler
  *        代为补 tick（见 svcrt_board.c），这里若再放一份会双重计数。
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "stm32f4xx_it.h"
/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
/* USER CODE END Includes */

/* USER CODE BEGIN EV */

/* USER CODE END EV */

/******************************************************************************/
/*           Cortex-M4 Processor Interruption and Exception Handlers          */
/******************************************************************************/
/**
  * @brief This function handles Non maskable interrupt.
  */
void NMI_Handler(void)
{
  /* USER CODE BEGIN NonMaskableInt_IRQn 0 */

  /* USER CODE END NonMaskableInt_IRQn 0 */
  /* USER CODE BEGIN NonMaskableInt_IRQn 1 */
   while (1)
  {
  }
  /* USER CODE END NonMaskableInt_IRQn 1 */
}

/**
  * @brief This function handles Hard fault interrupt.
  * @note  由 SVCrtOS 接管，实现见 board/stm32f401/svcrt_board.c。
  */
#if 0
void HardFault_Handler(void)
{
  while (1)
  {
  }
}
#endif

/**
  * @brief This function handles Memory management fault.
  * @note  由 SVCrtOS 接管，实现见 board/stm32f401/svcrt_board.c。
  */
#if 0
void MemManage_Handler(void)
{
  while (1)
  {
  }
}
#endif

/**
  * @brief This function handles Pre-fetch fault, memory access fault.
  * @note  由 SVCrtOS 接管，实现见 board/stm32f401/svcrt_board.c。
  */
#if 0
void BusFault_Handler(void)
{
  while (1)
  {
  }
}
#endif

/**
  * @brief This function handles Undefined instruction or illegal state.
  * @note  由 SVCrtOS 接管，实现见 board/stm32f401/svcrt_board.c。
  */
#if 0
void UsageFault_Handler(void)
{
  while (1)
  {
  }
}
#endif

/**
  * @brief This function handles System service call via SWI instruction.
  * @note  由 SVCrtOS 内核接管（端口层实现 SVC_Handler）。
  */
#if 0
void SVC_Handler(void)
{
}
#endif

/**
  * @brief This function handles Debug monitor.
  */
void DebugMon_Handler(void)
{
  /* USER CODE BEGIN DebugMonitor_IRQn 0 */

  /* USER CODE END DebugMonitor_IRQn 0 */
  /* USER CODE BEGIN DebugMonitor_IRQn 1 */

  /* USER CODE END DebugMonitor_IRQn 1 */
}

/**
  * @brief This function handles Pendable request for system service.
  * @note  由 SVCrtOS 内核接管（端口层实现 PendSV_Handler）。
  */
#if 0
void PendSV_Handler(void)
{
}
#endif

/**
  * @brief This function handles System tick timer.
  * @note  由 SVCrtOS 内核接管，实现见 board/stm32f401/svcrt_board.c。
  */
#if 0
void SysTick_Handler(void)
{
  HAL_IncTick();
}
#endif

/******************************************************************************/
/* STM32F4xx Peripheral Interrupt Handlers                                    */
/* Add here the Interrupt Handlers for the used peripherals.                  */
/* For the available peripheral interrupt handler names,                      */
/* please refer to the startup file (startup_stm32f401xe.s).                  */
/******************************************************************************/

/* USART2_IRQHandler 在 board/stm32f401/drvuart.c 里实现（控制台中断收发）。 */

/* USER CODE BEGIN 1 */

/* USER CODE END 1 */
