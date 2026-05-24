/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body + SVCRTOS ???????
  ******************************************************************************
  * @attention
  *
  * ?? CubeMX ????,USER CODE ????????? SVCRTOS ??????????????????
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "rtc.h"
#include "sdio.h"
#include "spi.h"
#include "usart.h"
#include "usb_otg.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "svcrt.h"
#include "svcrt_port.h"
#include "svcrt_cfg.h"
#include "svcrt_event.h"
#include "svcrt_dev.h"
#include "svcrt_mpu.h"
#include "svcrt_task.h"
#include "svcrt_config.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
/* SVCRTOS 任务栈(8 字节对齐,放在 BSS 区由启动代码清零) */
#define SVCRT_IDLE_STACK_WORDS    256
#define SVCRT_LED_STACK_WORDS     256

static uint32 svcrt_idle_stack[SVCRT_IDLE_STACK_WORDS];
static uint32 led_task_stack[SVCRT_LED_STACK_WORDS];
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */
static void svcrt_register_tasks(void);
static void svcrt_kernel_init(void);
static void svcrt_start_idle(void);
static void led_blink_task(void);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
//  MX_SDIO_SD_Init();
//  MX_SPI2_Init();
  MX_USART1_UART_Init();
//  MX_USB_OTG_FS_PCD_Init();
  MX_RTC_Init();
  /* USER CODE BEGIN 2 */

  /* ============================================================
   * SVCRTOS ????????
   * ============================================================ */

  /* 1) ?弶????????(??? FPU Э??????????) */
  svcrt_port_board_init();

  /* 2) ???ü???(????????) */
  svcrt_cfg_load();

  /* 3) ?????????? */
  svcrt_register_tasks();

  /* 4) ?ж??????????(PendSV ???, SysTick ???, SVCall ????) */
  svcrt_port_irq_init();

  /* 5) ????????????(??????豸???????????????) */
  svcrt_kernel_init();

  /* 6) 进入空闲任务循环(内部会先初始化 PSP/CONTROL,
   *    再启动 SysTick,确保第一次中断进来时 PSP 已就绪,
   *    防止 PendSV_Handler 操作野指针导致 HardFault)
   *    永不返回 */
  svcrt_start_idle();
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  *
  * @note 直接使用 HSI (内部 16MHz RC) 配置 SYSCLK = 96MHz,
  *       完全绕开 HSE 外部晶振,排除硬件起振问题。
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /* ===== 使用 HSI (16MHz 内部 RC) =====
   * VCO_IN  = HSI / PLLM = 16MHz / 16 = 1MHz
   * VCO_OUT = 1MHz * 192 = 192MHz
   * SYSCLK  = 192 / 2 = 96MHz
   * USB/SDIO/RNG_CLK = 192 / 4 = 48MHz (PLLQ=4)
   *
   * @note 不再启动 HSE,LSI 保留供 IWDG/RTC 使用。
   */
  RCC_OscInitStruct.OscillatorType      = RCC_OSCILLATORTYPE_HSI | RCC_OSCILLATORTYPE_LSI;
  RCC_OscInitStruct.HSEState            = RCC_HSE_OFF;
  RCC_OscInitStruct.HSIState            = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.LSIState            = RCC_LSI_ON;
  RCC_OscInitStruct.PLL.PLLState        = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource       = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM            = 16;
  RCC_OscInitStruct.PLL.PLLN            = 192;
  RCC_OscInitStruct.PLL.PLLP            = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ            = 4;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
   *
   * @note SYSCLK=96MHz, FLASH 等待周期 = 3 WS (2.7V~3.6V 范围)
   *       APB1 最大 45MHz → DIV4 = 24MHz (安全)
   *       APB2 最大 90MHz → DIV2 = 48MHz (安全)
   */
  RCC_ClkInitStruct.ClockType      = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK
                                   | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider  = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;   /* APB1 = 24MHz */
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;   /* APB2 = 48MHz */

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_3) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

/* ============================================================
 * LED ???????
 * @brief ????豸???? "LED" ?豸,????????????
 * ============================================================ */
static void led_blink_task(void)
{
    int32 led = svcrt_dev_open("LED", 0);
    uint8 on  = 1;

    while(1)
    {
        svcrt_dev_write(led, &on, 1);   /* len>0 ??????? */
        svcrt_task_wait(500);
        svcrt_dev_write(led, &on, 0);   /* len==0 ?????? */
        svcrt_task_wait(500);
    }
}

/* ============================================================
 * ??????????
 * @brief ????????????????? svcrt_task_table,
 *        ?????? svcrt_task_stack_init ????α????
 * ============================================================ */
static void svcrt_register_tasks(void)
{
    svcrt_task_t *p_task;

    /* ????????????,?? stack-check ?????? */
    led_task_stack[0] = SVCRT_STACK_END_FLAG_VAL;

    p_task = &svcrt_task_table[svcrt_task_count];
    p_task->ram_start   = (uint32)led_task_stack;
    p_task->ram_size    = sizeof(led_task_stack);
    p_task->stack_size  = sizeof(led_task_stack);
    p_task->rom_start   = 0;
    p_task->rom_size    = 0;
    p_task->period      = SVCRT_MS_TO_TICK(1000);
    p_task->priority    = 10;
    p_task->shm_attri   = 0;
    p_task->status      = SVCRT_TASK_READY;
    p_task->period_time = p_task->period;
    p_task->wait_time   = 0;
    p_task->tim_tick    = 0;
    p_task->touch_tick  = 0;

    #if (SVCRT_USE_MPU == 1)
    {
        int32 i;
        for(i = 0; i < 8; i++)
        {
            p_task->mpu_bar[i] = 0;
            p_task->mpu_asr[i] = 0;
        }
    }
    #endif

    svcrt_task_stack_init(p_task, led_blink_task,
                          led_task_stack, sizeof(led_task_stack));

    svcrt_task_count++;
}

/* ============================================================
 * ????????????
 * ============================================================ */
static void svcrt_kernel_init(void)
{
    svcrt_event_module_init();
    svcrt_dev_module_init();
    svcrt_dev_board_init();   /* ??? COM1 / LED ??????豸 */

    #if (SVCRT_USE_MPU == 1)
    svcrt_mpu_module_init();
    #endif
}

/* ============================================================
 * 空闲任务
 * @brief 启动调度的方式:
 *        1) 初始化 PSP 为 idle 栈顶 (尚未切换到 PSP)
 *        2) 通过 SVC 触发首次任务切换? 此处采用更稳的方式:
 *           直接设 PSP → 设 CONTROL.SPSEL=1 → 立即 ISB
 *        3) 启动 SysTick 节拍
 *        4) WFI 进入低功耗等待第一次 SysTick → PendSV → 切到任务
 *
 * @note  本函数从 main() 被调用,进入时 SP=MSP。切到 PSP 后,
 *        当前函数所有局部变量仍可访问 (它们要么在寄存器,要么
 *        在 MSP 已经入栈,只读 PSP 不会冲突)。
 * ============================================================ */
static void svcrt_start_idle(void)
{
    /* PSP 必须指向栈尾(高地址),并 8 字节对齐 */
    uint32 psp = (uint32)(&svcrt_idle_stack[SVCRT_IDLE_STACK_WORDS]);
    psp &= ~0x7u;

    /* 1. 先设 PSP (此时 CPU 仍用 MSP) */
    SVCRT_SET_PSP(psp);

    #if (SVCRT_USE_MPU == 1)
    svcrt_port_set_idle_mpu((uint32)svcrt_start_idle,
                            (uint32)svcrt_idle_stack,
                            sizeof(svcrt_idle_stack));
    #endif

    /* 2. 切到 PSP 栈 (CONTROL.SPSEL=1)
     *    SVCRT_USE_PRIV=0 时保持特权模式 */
    #if (SVCRT_USE_PRIV == 1)
    SVCRT_SET_CONTROL(0x3);          /* SPSEL=1, nPRIV=1 */
    #else
    SVCRT_SET_CONTROL(0x2);          /* SPSEL=1, nPRIV=0 */
    #endif
    SVCRT_ISB();                     /* 同步流水线 */

    /* 3. 现在 SP=PSP, 启动 SysTick 节拍 */
    svcrt_port_start_timer(SVCRT_TICK_PERIOD_US);

    /* 4. 进入空闲循环 */
    while(1)
    {
        SVCRT_WFI();
    }
}

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
