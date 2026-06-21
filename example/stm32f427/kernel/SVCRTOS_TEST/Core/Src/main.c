/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
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
#include "svcrt_hal.h"
#include "svcrt_cfg.h"
#include "svcrt_event.h"
#include "svcrt_dev.h"
#include "svcrt_sync.h"
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
#define SVCRT_IDLE_STACK_WORDS    512
#define SVCRT_LED_STACK_WORDS     1024
#define SVCRT_LED2_STACK_WORDS    1024

static uint32 svcrt_idle_stack[SVCRT_IDLE_STACK_WORDS];
static uint32 led_task_stack[SVCRT_LED_STACK_WORDS];
static uint32 led2_task_stack[SVCRT_LED2_STACK_WORDS];

/* ??????????????????? LED ??????υτ?????§Υ?????????????? */
static int32 g_led_mutex = -1;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */
static void svcrt_register_tasks(void);
static void svcrt_kernel_init(void);
static void svcrt_start_idle(void);
static void led_blink_task(void);
static void led2_blink_task(void);
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
  MX_USART1_UART_Init();
  MX_RTC_Init();
  /* USER CODE BEGIN 2 */

  svcrt_port_board_init();

  svcrt_cfg_load();

  svcrt_register_tasks();

  svcrt_port_irq_init();

  svcrt_kernel_init();

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
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

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

  RCC_ClkInitStruct.ClockType      = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK
                                   | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider  = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_3) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

static void led_blink_task(void)
{
    int32 led = svcrt_dev_open("LED", 0);
    uint8 on  = 1;

    while(1)
    {
        /* ???????????υτ§Υ?????????????? */
        svcrt_mutex_lock(g_led_mutex, 0);
        svcrt_dev_write(led, &on, 1);      /* ???? */
        svcrt_mutex_unlock(g_led_mutex);
        svcrt_task_wait(1000);

        svcrt_mutex_lock(g_led_mutex, 0);
        svcrt_dev_write(led, &on, 0);      /* ???? */
        svcrt_mutex_unlock(g_led_mutex);
        svcrt_task_wait(1000);
    }
}

static void led2_blink_task(void)
{
    int32 led = svcrt_dev_open("LED2", 0);
    uint8 on  = 1;

    while(1)
    {
        /* ??????¦Λ???????????????? */
        svcrt_mutex_lock(g_led_mutex, 0);
        svcrt_dev_write(led, &on, 0);      /* ???? */
        svcrt_mutex_unlock(g_led_mutex);
        svcrt_task_wait(1000);

        svcrt_mutex_lock(g_led_mutex, 0);
        svcrt_dev_write(led, &on, 1);      /* ???? */
        svcrt_mutex_unlock(g_led_mutex);
        svcrt_task_wait(1000);
    }
}

static void svcrt_register_tasks(void)
{
    svcrt_task_t *p_task;

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

    led2_task_stack[0] = SVCRT_STACK_END_FLAG_VAL;

    p_task = &svcrt_task_table[svcrt_task_count];
    p_task->ram_start   = (uint32)led2_task_stack;
    p_task->ram_size    = sizeof(led2_task_stack);
    p_task->stack_size  = sizeof(led2_task_stack);
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

    svcrt_task_stack_init(p_task, led2_blink_task,
                          led2_task_stack, sizeof(led2_task_stack));

    svcrt_task_count++;
}

static void svcrt_kernel_init(void)
{
    svcrt_event_module_init();
    svcrt_sync_module_init();
    svcrt_dev_module_init();
    svcrt_dev_board_init();

    /* ??????????? LED ?υτ§Υ????????????????????????????????? */
    g_led_mutex = svcrt_mtx_create_internal("ledmtx");

    #if (SVCRT_USE_MPU == 1)
    svcrt_port_mpu_init();
    #endif
}

static void svcrt_start_idle(void)
{
    uint32 psp = (uint32)(&svcrt_idle_stack[SVCRT_IDLE_STACK_WORDS]);
    psp &= ~0x7u;

    #if (SVCRT_USE_MPU == 1)
    svcrt_port_set_idle_mpu((uint32)svcrt_start_idle,
                            (uint32)svcrt_idle_stack,
                            sizeof(svcrt_idle_stack));
    #endif

    svcrt_port_enter_idle(psp, SVCRT_USE_PRIV);

    svcrt_port_start_timer(SVCRT_TICK_PERIOD_US);

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
  * @param  line: assert_param error line number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
