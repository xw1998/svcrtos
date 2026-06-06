/**
* @brief SVCrtOS 内核启动与初始化（默认模板）
* @details 提供一个弱定义的 main() 作为最小启动示例。
*          当应用程序定义了自己的 main() 时，此弱定义会被覆盖。
* @author xw
* @date 2026.05.03
*/

#include "svcrt_cfg.h"
#include "svcrt_task.h"
#include "svcrt_event.h"
#include "svcrt_hal.h"
#include "svcrt_dev.h"
#include "svcrt_config.h"

static void svcrt_start_idle_default(void);
static void svcrt_kernel_init_default(void);
static uint32 svcrt_idle_stack_default[100];

__weak int main(void)
{
    svcrt_port_board_init();

    svcrt_cfg_load();

    svcrt_port_irq_init();

    svcrt_kernel_init_default();

    svcrt_port_start_timer(SVCRT_TICK_PERIOD_US);

    svcrt_start_idle_default();
    return 0;
}

static void svcrt_start_idle_default(void)
{
    uint32 psp = (uint32)(svcrt_idle_stack_default + 99);
    psp &= ~0x7u;

    #if (SVCRT_USE_MPU == 1)
    svcrt_port_set_idle_mpu((uint32)svcrt_start_idle_default, (uint32)svcrt_idle_stack_default, sizeof(svcrt_idle_stack_default));
    #endif

    svcrt_port_enter_idle(psp, SVCRT_USE_PRIV);

    while(1)
    {
        SVCRT_WFI();
    }
}

static void svcrt_kernel_init_default(void)
{
    svcrt_event_module_init();
    svcrt_dev_module_init();
    svcrt_dev_board_init();

    #if (SVCRT_USE_MPU == 1)
    svcrt_port_mpu_init();
    #endif
}
