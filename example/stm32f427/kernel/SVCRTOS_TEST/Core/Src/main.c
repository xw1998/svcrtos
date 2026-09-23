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
/* ×¢Òâ£º±¾ÎÄ¼þµÄ°å¼¶ LED ÈÎÎñÊÇÄÚºË²àÈÎÎñ£¬Ö±½Óµ÷ÓÃ _internal ½Ó¿Ú£¬
 * ²»×ß SVC ÓÃ»§Ì¬·â×°£¨svcrt_dev_open µÈ£©¡£ÓÃ»§Ì¬·â×°Ö»ÊôÓÚ App/Çý¶¯ SDK£¬
 * ÔçÆÚ¿¿ kernelsrc/app/oslib.c ÖØ¸´¸±±¾Ìá¹©µÄ×ö·¨ÒÑ·ÏÆú¡£ */
/* USER CODE BEGIN Includes */
#include "svcrt.h"
#include "svcrt_hal.h"
#include "svcrt_cfg.h"
#include "svcrt_event.h"
#include "svcrt_dev.h"
#include "svcrt_sync.h"
#include "svcrt_task.h"
#include "svcrt_mpu.h"
#include "svcrt_config.h"
#include "svcrt_ptable.h"
#include "svcrt_loader.h"
#include "svcrt_installer.h"
#include "svcrt_shell.h"
#include "svcrt_log.h"
#include "svcrt_vfs.h"
#include "svcrt_mq.h"
#include "svcrt_timer.h"
#include "svcrt_fault.h"
#include "svcrt_init.h"
#include "svcrt_partition.h"
#include "svcrt_layout.h"
#include "svcrt_audit.h"
#include "svcrt_guard.h"
#include "svcrt_crash.h"
#if (SVCRT_USE_LWIP == 1)
#include "lwip_port.h"
#endif
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

/* ×¢Òâ£ºÍâ²¿·ÖÇø¹Ì¼þ£¨Çý¶¯ / App£©²»ÔÙÔÚÕâÀï¾²Ì¬×¢²áÈÎÎñ¡£
 * ËüÃÇµÄÈë¿ÚµØÖ·ÓëÕ»ÓÉÄÚºË¼ÓÔØÆ÷°´ config/svcrt_partition.h ÍÆµ¼£º
 *   svcrt_loader_scan_driver()/start_driver()¡¢svcrt_loader_scan()/start()
 * ÕâÑù¡°¹Ì¶¨µØÖ·ÉÕÂ¼µ÷ÊÔ¡±Óë¡°´®¿Ú°²×°¡±Á½ÌõÂ·¾¶¹²ÓÃÍ¬Ò»Ì×Æô¶¯Âß¼­¡£ */

static uint32 svcrt_idle_stack[SVCRT_IDLE_STACK_WORDS];
static uint32 led_task_stack[SVCRT_LED_STACK_WORDS];
static uint32 led2_task_stack[SVCRT_LED2_STACK_WORDS];

/* Á½¸ö LED ÈÎÎñ¹²ÓÃÒ»°Ñ»¥³âËø£¬±£»¤¶Ô LED Éè±¸µÄÐ´Èë */
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

  /* lwIP ç«¯å£ï¼šåœ¨è°ƒåº¦å™¨èµ·æ¥ä¹‹å‰æŠŠè½®è¯¢ä»»åŠ¡ç™»è®°å¥½ã€‚çœŸæ­£çš„ tcpip_init()
   * ç•™åˆ°è¯¥ä»»åŠ¡çš„ç¬¬ä¸€è½®å†è°ƒâ€”â€”å®ƒä¼šåœ¨ä¿¡å·é‡ä¸Šç­‰å¾…ï¼Œè€Œé‚£æ—¶å€™è¿˜æ²¡æœ‰äºº
   * èƒ½æŠŠå®ƒå”¤é†’ã€‚ */
#if (SVCRT_USE_LWIP == 1)
  svcrt_lwip_start();
#endif

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
    int32 led = svcrt_dev_open_internal("LED", 0);
    uint8 on  = 1;

    while(1)
    {
        /* ¼ÓËøºóÐ´Éè±¸£ºlen>0 µãÁÁ¡¢len==0 Ï¨Ãð£¨Ô¼¶¨¼û°å¼¶ drvled£© */
        svcrt_mtx_lock_internal(g_led_mutex, -1);   /* -1 = ÓÀ¾ÃµÈ´ý */
        svcrt_dev_write_internal(led, &on, 1);      /* µãÁÁ */
        svcrt_mtx_unlock_internal(g_led_mutex);
        svcrt_task_wait_internal(1000);

        svcrt_mtx_lock_internal(g_led_mutex, -1);   /* -1 = ÓÀ¾ÃµÈ´ý */
        svcrt_dev_write_internal(led, &on, 0);      /* Ï¨Ãð */
        svcrt_mtx_unlock_internal(g_led_mutex);
        svcrt_task_wait_internal(1000);
    }
}

static void led2_blink_task(void)
{
    int32 led = svcrt_dev_open_internal("LED2", 0);
    uint8 on  = 1;

    while(1)
    {
        /* µÚ¶þ¸ö LED£ºÏÈÃðºóÁÁ£¬Óë led_blink_task ÏàÎ»Ïà·´ */
        svcrt_mtx_lock_internal(g_led_mutex, -1);   /* -1 = ÓÀ¾ÃµÈ´ý */
        svcrt_dev_write_internal(led, &on, 0);      /* Ï¨Ãð */
        svcrt_mtx_unlock_internal(g_led_mutex);
        svcrt_task_wait_internal(1000);

        svcrt_mtx_lock_internal(g_led_mutex, -1);   /* -1 = ÓÀ¾ÃµÈ´ý */
        svcrt_dev_write_internal(led, &on, 1);      /* µãÁÁ */
        svcrt_mtx_unlock_internal(g_led_mutex);
        svcrt_task_wait_internal(1000);
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
    p_task->base_priority = 10;         /* ¼Ì³Ð³·ÏúµÄ»Ö¸´ÒÀ¾Ý£ºÈ±ÁË»á±»Ìáµ½ÓÅÏÈ¼¶ 0 */
    p_task->shm_attri   = 0;
    p_task->status      = SVCRT_TASK_READY;
    p_task->period_time = p_task->period;
    p_task->wait_time   = 0;
    p_task->tim_tick    = 0;
    p_task->touch_tick  = 0;

    /* A hand-built TCB skips svcrt_task_register(), so it also skips the two
     * things that function finishes for us: the MPU windows (kernel task
     * defaults - kernel flash plus the whole chip RAM) and "this task is
     * kernel code, therefore privileged". Leaving both at their .bss values
     * produces an unprivileged task with no MPU region at all, and with
     * PRIVDEFENA that means its very first stack access faults. */
    p_task->is_priv = 1u;
    svcrt_mpu_build_task(p_task);

    svcrt_task_stack_init(p_task, led_blink_task,
                          led_task_stack, sizeof(led_task_stack));

    svcrt_task_count++;
    svcrt_ready_add(svcrt_task_count - 1);   /* ÊÖ¹¤µÇ¼Ç TCB Ò²±ØÐë¹Ò¾ÍÐ÷Á´ */

    led2_task_stack[0] = SVCRT_STACK_END_FLAG_VAL;

    p_task = &svcrt_task_table[svcrt_task_count];
    p_task->ram_start   = (uint32)led2_task_stack;
    p_task->ram_size    = sizeof(led2_task_stack);
    p_task->stack_size  = sizeof(led2_task_stack);
    p_task->rom_start   = 0;
    p_task->rom_size    = 0;
    p_task->period      = SVCRT_MS_TO_TICK(1000);
    p_task->priority    = 10;
    p_task->base_priority = 10;         /* ¼Ì³Ð³·ÏúµÄ»Ö¸´ÒÀ¾Ý£ºÈ±ÁË»á±»Ìáµ½ÓÅÏÈ¼¶ 0 */
    p_task->shm_attri   = 0;
    p_task->status      = SVCRT_TASK_READY;
    p_task->period_time = p_task->period;
    p_task->wait_time   = 0;
    p_task->tim_tick    = 0;
    p_task->touch_tick  = 0;

    /* A hand-built TCB skips svcrt_task_register(), so it also skips the two
     * things that function finishes for us: the MPU windows (kernel task
     * defaults - kernel flash plus the whole chip RAM) and "this task is
     * kernel code, therefore privileged". Leaving both at their .bss values
     * produces an unprivileged task with no MPU region at all, and with
     * PRIVDEFENA that means its very first stack access faults. */
    p_task->is_priv = 1u;
    svcrt_mpu_build_task(p_task);

    svcrt_task_stack_init(p_task, led2_blink_task,
                          led2_task_stack, sizeof(led2_task_stack));

    svcrt_task_count++;
    svcrt_ready_add(svcrt_task_count - 1);   /* ÊÖ¹¤µÇ¼Ç TCB Ò²±ØÐë¹Ò¾ÍÐ÷Á´ */
}

static void svcrt_kernel_init(void)
{
    /* Ë³ÐòÓëÄÚºËÄ¬ÈÏÈë¿Ú kernelsrc/src/svcrt_init.c Ò»ÖÂ£º
     * ÏÈ³õÊ¼»¯¸÷ÄÚºËÄ£¿é£¬ÔÙÈÏ¶¨/Æô¶¯Íâ²¿Ó³Ïñ£¬×îºó×¢²á°²×°ÈÎÎñ¡£
     * £¨È±Ä£¿é³õÊ¼»¯»òË³Ðòµßµ¹»áµ¼ÖÂÄÚºËÎÞ·¨Õý³£Æô¶¯£© */
    /* ¸÷ÄÚºËÄ£¿é + ÄÚÖÃ·þÎñÈÎÎñ£ºÍ³Ò»×ß¹«¹²³õÊ¼»¯£¬±ÜÃâÓëÄÚºËÄ¬ÈÏÈë¿ÚÆ¯ÒÆ */
    svcrt_kernel_module_init();

    /* Log first: everything after this point can leave a trace on the
     * console, which is the only cheap way to see where a boot stops. */
    svcrt_log_init();
    SVCRT_LOGI("BOOT", "SVCrtOS kernel starting, build %s %s",
               __DATE__, __TIME__);

    /* ÄÚºËÄÚÖÃ LED ÈÎÎñ¹²ÓÃµÄ»¥³âÁ¿ */
    g_led_mutex = svcrt_mtx_create_internal("ledmtx");

    /* ---- ·ÖÇø±íÓëÍâ²¿Ó³Ïñ ---- */
    svcrt_ptable_init();

    /* Cross-reset crash bookkeeping. init() only validates or starts the
     * journal; it must run before anything can fault, and it does not
     * need a scanned pool. */
    svcrt_crash_init();

    /* Device-side layout config (install mode + fixed slot table) must be
     * resolved before the pool is scanned: both the scan and the installer
     * ask svcrt_layout_*() where an image is allowed to live. An empty or
     * invalid CONFIG region falls back to the compile-time default layout,
     * so a bad configuration never stops the kernel from booting. */
    svcrt_layout_init();

    /* Debug-attach window: the device-side configuration may ask for a
     * boot delay. It is held here -- after the effective layout is known
     * (the delay itself comes from the CONFIG region) but before anything
     * is scanned or started, so a debugger or a host tool can attach to a
     * device that would otherwise have already begun running images.
     * The wait is split into short chunks so a long delay does not ride on
     * one enormous busy loop. */
    {
        uint32 boot_delay = svcrt_layout_boot_delay_ms();

        if(boot_delay != 0u)
        {
            uint32 left = boot_delay;

            SVCRT_LOGI("BOOT", "boot delay %u ms before autostart (debugger attach window)",
                       boot_delay);
            while(left > 0u)
            {
                uint32 chunk = (left > 10u) ? 10u : left;

                svcrt_port_delay_us(chunk * 1000u);
                left -= chunk;
            }
        }
    }


#if (SVCRT_USE_VFS == 1)
    /* Linux-style path namespace: / is a volatile scratch area, /dev mirrors
     * the device registry, and volumes live under /mnt/<name> where an upper
     * layer mounts them explicitly - which device holds a filesystem is board
     * knowledge, not kernel knowledge.
     *
     * Brought up before any image is started, because that is the whole point
     * of a namespace: an App should be able to open "/dev/uart0" from its own
     * first line, not from the moment someone remembers to initialise it.
     * Failing here is not fatal (the console and the images are still fine),
     * but it is reported rather than swallowed, and the error name says which
     * layer refused. */
    {
        int32 vfs_rc = svcrt_vfs_init();

        if(vfs_rc != 0)
        {
            SVCRT_LOGE("BOOT", "svcrt_vfs_init failed: %d (%s)", (int)vfs_rc,
                       svcrt_vfs_error_name(vfs_rc));
        }
    }
#endif /* SVCRT_USE_VFS */

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

    /* Adopt the crash counts that belong to the images this scan just
     * found, and put any slot that already reached the fault limit back
     * into the held state before the autostart pass below can run it
     * again. Between the scan and the autostart is the only window in
     * which "what is in the pool" is known and nothing has started.
     * reclaim() runs first so the pool addresses the scan left behind
     * are the final ones the journal is re-tagged against. */
    svcrt_crash_scan_apply();

    /* Drivers are identified and started before Apps (drivers have the higher
     * priority, and App services depend on driver services being ready).
     * Whether a slot starts on boot comes from the scan result: an image with
     * a header uses the flags field written by the packer (see --autostart in
     * tools/pack_app.py), while a raw image (development flash-and-run) falls
     * back to the global DRIVER_AUTO_START switch. Slots without the flag are
     * left stopped and must be started explicitly by an upper layer
     * (for example the kernel shell's "driver start"). */
    if(svcrt_loader_scan_driver() > 0u)
    {
        svcrt_loader_start_autostart_driver();
    }

    /* App slots: raw images (development) or headed images (install / flash)
     * are identified first, then started according to the per-slot flag;
     * raw images fall back to APP_AUTO_START. */
    if(svcrt_loader_scan() > 0u)
    {
        svcrt_loader_start_autostart();
    }

    #if (SHELL_ENABLE == 1)
    /* Console enabled: the shell task owns the serial port, so the resident
     * installer task is not registered here. Image installation is triggered
     * on demand by the shell's "install" command, which opens a one-shot
     * receive window on the very same device handle (svcrt_installer_run_once).
     * This keeps a single reader on the UART FIFO. */
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

    /* Start the idle context privileged: the switch path applies each
     * task's own privilege (an App is unprivileged) on the first switch,
     * and the idle task itself is kernel code. */
    svcrt_port_enter_idle(psp, 1u);

    svcrt_port_start_timer(SVCRT_TICK_PERIOD_US);

    /* The idle PSP context now exists, so the scheduler may switch: this
     * is what starts the first registered task (driver/App tasks were
     * kept READY while the loader ran). */
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
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
