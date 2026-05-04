/**
* @brief SVCrtOS 内核启动与初始化
* @details 操作系统启动流程：板卡初始化 → 配置加载 → 内核初始化 → 启动调度
* @author xw
* @date 2026.05.03
*/

#include "svcrt_cfg.h"
#include "svcrt_task.h"
#include "svcrt_event.h"
#include "svcrt_mpu.h"
#include "svcrt_dev.h"
#include "svcrt_config.h"

static void svcrt_start_idle(void);
static void svcrt_kernel_init(void);
static uint32 svcrt_idle_stack[100];

int main(void)
{
    svcrt_port_board_init();

    svcrt_cfg_load();

    svcrt_port_irq_init();

    svcrt_kernel_init();

    svcrt_port_start_timer(SVCRT_TICK_PERIOD_US);

    svcrt_start_idle();
    return 0;
}

static void svcrt_start_idle(void)
{
    SVCRT_SET_PSP((uint32)(svcrt_idle_stack + 99));

    #if (SVCRT_USE_MPU == 1)
    svcrt_port_set_idle_mpu((uint32)svcrt_start_idle, (uint32)svcrt_idle_stack, sizeof(svcrt_idle_stack));
    #endif

    #if (SVCRT_USE_PRIV == 1)
    SVCRT_SET_CONTROL(0x3 | SVCRT_GET_CONTROL());
    #else
    SVCRT_SET_CONTROL(0x2 | SVCRT_GET_CONTROL());
    #endif
    SVCRT_ISB();
    SVCRT_WFI();

    while(1)
    {
    }
}

static void svcrt_kernel_init(void)
{
    svcrt_event_module_init();
    svcrt_dev_module_init();
    svcrt_dev_board_init();

    #if (SVCRT_USE_MPU == 1)
    svcrt_mpu_module_init();
    #endif
}
