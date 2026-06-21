/**
* @brief SVCrtOS 内核启动与初始化（提供默认弱实现）
* @details 本文件提供默认的内核启动 main() 入口及初始化流程。
*          若应用使用 CubeMX 生成的 main.c，可自行实现强符号 main() 覆盖此处的弱实现。
*          覆盖时需按相同顺序调用各初始化步骤，否则内核无法正常启动。
*
*          应用自定义 main() 时建议遵循以下启动顺序：
*          1) HAL 初始化 + 时钟配置 + 外设初始化（CubeMX 生成部分）
*          2) svcrt_port_board_init()
*          3) svcrt_cfg_load()
*          4) svcrt_register_tasks()（注册应用任务）
*          5) svcrt_port_irq_init()
*          6) svcrt_kernel_init()
*          7) svcrt_start_idle()（进入空闲任务，启动调度）
*
* @author xw
* @date 2026.05.03
*/

#include "svcrt_cfg.h"
#include "svcrt_task.h"
#include "svcrt_event.h"
#include "svcrt_hal.h"
#include "svcrt_dev.h"
#include "svcrt_sync.h"
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
    svcrt_sync_module_init();
    svcrt_dev_module_init();
    svcrt_dev_board_init();

    #if (SVCRT_USE_MPU == 1)
    svcrt_port_mpu_init();
    #endif
}
