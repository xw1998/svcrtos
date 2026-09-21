/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : SVCrtOS 内核示例（STM32F401 板）主程序
  ******************************************************************************
  * @note 本工程是 SVCrtOS 在 STM32F401 上的最小可信闭环：
  *         节拍 / 调度 / 任务栈用量 / UART2 控制台 shell。
  *       地址布局只在 config/stm32f401/svcrt_partition.h 定义一次，
  *       本文件与板级代码一律从该头文件派生，不再出现裸地址。
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* 注意：本文件的板级 LED 任务运行在内核态，直接调用 _internal 接口。
 * 面向 SVC 用户态的封装（svcrt_dev_open 等）留给 App/驱动 SDK，
 * 见 kernelsrc/app/oslib.c 对外提供的函数族。 */
/* USER CODE BEGIN Includes */
#include "svcrt.h"
#include "svcrt_hal.h"
#include "svcrt_cfg.h"
#include "svcrt_event.h"
#include "svcrt_dev.h"
#include "svcrt_sync.h"
#include "svcrt_task.h"
#include "svcrt_config.h"
#include "svcrt_ptable.h"
#include "svcrt_loader.h"
#include "svcrt_installer.h"
#include "svcrt_shell.h"
#include "svcrt_log.h"
#include "svcrt_mq.h"
#include "svcrt_timer.h"
#include "svcrt_fault.h"
#include "svcrt_init.h"
#include "svcrt_partition.h"
#include "svcrt_layout.h"
#include "svcrt_audit.h"
#include "svcrt_guard.h"
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
#define SVCRT_CNT_STACK_WORDS     1024

/* 注意：外部镜像（驱动 / App）由各自镜像静态注册任务时，
 * 它们的固定地址与栈都在内核寄存器表里，从 config/stm32f401/svcrt_partition.h 派生：
 *   svcrt_loader_scan_driver()/start_driver()、svcrt_loader_scan()/start()
 * 本次 bring-up 不涉及镜像池，只做内核自带任务。 */

static uint32 svcrt_idle_stack[SVCRT_IDLE_STACK_WORDS];
static uint32 led_task_stack[SVCRT_LED_STACK_WORDS];
static uint32 cnt_task_stack[SVCRT_CNT_STACK_WORDS];

/* 内核里给 LED 任务配的一把互斥锁：多个任务写同一个 LED 设备 */
static int32 g_led_mutex = -1;

/* 供 trace / 调试观察用（故意不加 static，符号表里按名字可读）：
 *   g_led_phase        LED 当前亮灭状态（0=灭 1=亮）
 *   g_led_blink_count  LED 任务完成的闪灯次数
 *   g_cnt_task_ticks   计数任务的心跳计数
 * 变量时间线（trace_scope）与 read_variable 都直接看这三个。 */
volatile uint32 g_led_phase       = 0;
volatile uint32 g_led_blink_count = 0;
volatile uint32 g_cnt_task_ticks  = 0;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */
static void svcrt_register_tasks(void);
static void svcrt_kernel_init(void);
static void svcrt_start_idle(void);
static void led_blink_task(void);
static void counter_task(void);
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
  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* Configure the system clock: HSI 16MHz -> PLL -> 84MHz（F401 最高主频） */
  SystemClock_Config();

  /* USER CODE BEGIN 2 */

  /* 板级：FPU 使能（CPACR/FPCCR） */
  svcrt_port_board_init();

  /* 分区表：从配置派生内核可见的地址视图 */
  svcrt_cfg_load();

  /* 注册内核自带任务（栈 + 任务表项） */
  svcrt_register_tasks();

  /* 中断优先级 + 故障异常使能 */
  svcrt_port_irq_init();

  /* 内核模块 / 设备注册 / 日志 / 分区扫描 / shell 初始化 */
  svcrt_kernel_init();

  /* 进入 idle：起 SysTick、开调度、WFI 等节拍 */
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
  * @note  HSI(16MHz) / PLLM=16 -> 1MHz；PLLN=336 -> 336MHz VCO；
  *        PLLP=4 -> SYSCLK 84MHz；PLLQ=7 -> 48MHz（USB/SDIO 时钟域）。
  *        VOS = Scale2 才允许 84MHz，Flash 取 2 个等待周期。
  *        AHBCLK=DIV1(84MHz) / APB1=DIV2(42MHz) / APB2=DIV1(84MHz)。
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE2);

  RCC_OscInitStruct.OscillatorType      = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSEState            = RCC_HSE_OFF;
  RCC_OscInitStruct.HSIState            = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState        = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource       = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM            = 16;
  RCC_OscInitStruct.PLL.PLLN            = 336;
  RCC_OscInitStruct.PLL.PLLP            = RCC_PLLP_DIV4;
  RCC_OscInitStruct.PLL.PLLQ            = 7;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  RCC_ClkInitStruct.ClockType      = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK
                                   | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider  = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

static void led_blink_task(void)
{
    int32 led = svcrt_dev_open_internal("LED", 0);
    uint8 on  = 1;

    while(1)
    {
        /* 驱动约定：写设备时 len>0 点亮，len==0 熄灭（见板级 drvled） */
        svcrt_mtx_lock_internal(g_led_mutex, -1);   /* -1 = 永久等待 */
        svcrt_dev_write_internal(led, &on, 1);      /* 点亮 */
        svcrt_mtx_unlock_internal(g_led_mutex);
        g_led_phase = 1;
        svcrt_task_wait_internal(1000);

        svcrt_mtx_lock_internal(g_led_mutex, -1);   /* -1 = 永久等待 */
        svcrt_dev_write_internal(led, &on, 0);      /* 熄灭 */
        svcrt_mtx_unlock_internal(g_led_mutex);
        g_led_phase = 0;
        svcrt_task_wait_internal(1000);

        g_led_blink_count++;
    }
}

/* 第二个任务：没有第二颗 LED，改成一个纯软件的计数心跳。
 * 它同时是"调度真的在切"的活证据（计数任务的心跳在走、栈用量非 0），
 * 也是 trace_scope / trace_pcsample 的观测对象。 */
static void counter_task(void)
{
    while(1)
    {
        g_cnt_task_ticks++;
        svcrt_task_wait_internal(500);
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
    p_task->base_priority = 10;         /* 继承撤销的恢复依据：缺了会被提到优先级 0 */
    p_task->shm_attri   = 0;
    p_task->status      = SVCRT_TASK_READY;
    p_task->period_time = p_task->period;
    p_task->wait_time   = 0;
    p_task->tim_tick    = 0;
    p_task->touch_tick  = 0;

    #if (SVCRT_USE_MPU == 1)
    {
        int32 i;
        for(i = 0; i < SVCRT_MPU_REGION_MAX; i++)
        {
            p_task->mpu.region_base[i] = 0;
            p_task->mpu.region_attr[i] = 0;
        }
    }
    #endif

    svcrt_task_stack_init(p_task, led_blink_task,
                          led_task_stack, sizeof(led_task_stack));

    svcrt_task_count++;
    svcrt_ready_add(svcrt_task_count - 1);   /* 手工登记 TCB 也必须挂就绪链 */

    cnt_task_stack[0] = SVCRT_STACK_END_FLAG_VAL;

    p_task = &svcrt_task_table[svcrt_task_count];
    p_task->ram_start   = (uint32)cnt_task_stack;
    p_task->ram_size    = sizeof(cnt_task_stack);
    p_task->stack_size  = sizeof(cnt_task_stack);
    p_task->rom_start   = 0;
    p_task->rom_size    = 0;
    p_task->period      = SVCRT_MS_TO_TICK(500);
    p_task->priority    = 10;
    p_task->base_priority = 10;         /* 继承撤销的恢复依据：缺了会被提到优先级 0 */
    p_task->shm_attri   = 0;
    p_task->status      = SVCRT_TASK_READY;
    p_task->period_time = p_task->period;
    p_task->wait_time   = 0;
    p_task->tim_tick    = 0;
    p_task->touch_tick  = 0;

    #if (SVCRT_USE_MPU == 1)
    {
        int32 i;
        for(i = 0; i < SVCRT_MPU_REGION_MAX; i++)
        {
            p_task->mpu.region_base[i] = 0;
            p_task->mpu.region_attr[i] = 0;
        }
    }
    #endif

    svcrt_task_stack_init(p_task, counter_task,
                          cnt_task_stack, sizeof(cnt_task_stack));

    svcrt_task_count++;
    svcrt_ready_add(svcrt_task_count - 1);   /* 手工登记 TCB 也必须挂就绪链 */
}

static void svcrt_kernel_init(void)
{
    /* 顺序与内核默认流程 kernelsrc/src/svcrt_init.c 一致：
     * 先初始化内核模块、再挂载/扫描外部镜像并注册安装器，
     * 顺序反了会拿不到外设。 */
    svcrt_kernel_module_init();

    /* Log first: everything after this point can leave a trace on the
     * console, which is the only cheap way to see where a boot stops. */
    svcrt_log_init();
    SVCRT_LOGI("BOOT", "SVCrtOS kernel starting on STM32F401, build %s %s",
               __DATE__, __TIME__);

    /* 内核里给 LED 任务用的互斥锁 */
    g_led_mutex = svcrt_mtx_create_internal("ledmtx");

    /* ---- 分区与外部镜像 ---- */
    svcrt_ptable_init();

    /* Device-side layout config (install mode + fixed slot table) must be
     * resolved before the pool is scanned: both the scan and the installer
     * ask svcrt_layout_*() where an image is allowed to live. An empty or
     * invalid CONFIG region falls back to the compile-time default layout,
     * so a bad configuration never stops the kernel from booting. */
    svcrt_layout_init();

    /* Power-on recovery: erase what an interrupted install or an aborted
     * compaction left behind, and push any hole towards the top of the
     * pool. Runs before any task is started so images can be moved
     * without touching running state.
     *
     * It MUST run after the pool scan, not before: reclaim() decides what
     * is garbage from the slot table, and right after svcrt_ptable_init()
     * that table is empty - every remaining image in the pool would look
     * like garbage and be erased, so a reboot would uninstall everything.
     * The scan below registers the live images first; it is idempotent
     * (svcrt_loader_scan_pool() runs once), so the calls further down
     * reuse its result and cost nothing. */
    (void)svcrt_loader_scan_driver();
    (void)svcrt_loader_reclaim();

    /* Drivers are identified and started before Apps (drivers have the higher
     * priority, and App services depend on driver services being ready). */
    if(svcrt_loader_scan_driver() > 0u)
    {
        svcrt_loader_start_autostart_driver();
    }

    /* App slots: raw images (development) or headed images (install / flash)
     * are identified first, then started according to the per-slot flag. */
    if(svcrt_loader_scan() > 0u)
    {
        svcrt_loader_start_autostart();
    }

    #if (SHELL_ENABLE == 1)
    /* Console enabled: the shell task owns the serial port, so the resident
     * installer task is not registered here. Image installation is triggered
     * on demand by the shell's "install" command. */
    svcrt_shell_init();
    #else
    /* No console: the resident installer task receives images by itself. */
    svcrt_installer_init();
    #endif

    /* Structure self-audit: the table is populated and the images are
     * registered, so this is the first moment the invariants mean anything,
     * and the last one before any image code runs. */
    (void)svcrt_audit_boot();

    /* Guard last: the watchdog is armed here (when the board asks for
     * it) and from this point on the kernel decides whether it gets fed. */
    svcrt_guard_init();

    /* One line that answers the two questions asked most often on the
     * console: how much room is left for images, and how many tasks the
     * application set has already consumed. */
    {
        uint32 pool_largest = 0u;
        uint32 pool_free    = svcrt_loader_pool_free(&pool_largest);

        SVCRT_LOGI("POOL", "free %u B (largest run %u B), tasks %d/%u",
                   pool_free, pool_largest,
                   (int)svcrt_task_count, (uint32)SVCRT_TASK_MAX_NUM);
    }
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

    /* The idle PSP context now exists, so the scheduler may switch: this
     * is what starts the first registered task. */
    svcrt_port_switch_enable();

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
  (void)file;
  (void)line;
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
