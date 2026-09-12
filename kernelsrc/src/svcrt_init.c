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
*          4) svcrt_port_irq_init()
*          5) svcrt_kernel_module_init()   各内核模块 + 板级设备 + 内置服务任务
*                                          （唯一权威顺序实现，见本文件下方）
*          6) 注册应用/驱动任务（svcrt_task_register 等，必须在调度启动前完成）
*          7) svcrt_start_idle()（进入空闲任务，启动调度）
*
*          需要“设备自安装”能力的工程，在第 5 步之后追加：
*          svcrt_ptable_init() → svcrt_loader_scan_driver()/svcrt_loader_start_driver()
*          → svcrt_loader_scan()/svcrt_loader_start() → svcrt_installer_init()
*          （参考实现：example/stm32f427/kernel/SVCRTOS_TEST/Core/Src/main.c 的 svcrt_kernel_init）
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
#include "svcrt_mq.h"
#include "svcrt_timer.h"
#include "svcrt_fault.h"
#include "svcrt_config.h"
#include "svcrt_init.h"

static void svcrt_start_idle_default(void);
static uint32 svcrt_idle_stack_default[100];

__weak int main(void)
{
    svcrt_port_board_init();

    svcrt_cfg_load();

    svcrt_port_irq_init();

    svcrt_kernel_module_init();

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

/* 内核模块初始化：唯一权威顺序实现（弱 main 与工程 main 都调用它） */
void svcrt_kernel_module_init(void)
{
    svcrt_event_module_init();
    svcrt_sync_module_init();
    svcrt_mq_module_init();
    svcrt_timer_module_init();
    svcrt_fault_module_init();
    svcrt_dev_module_init();
    svcrt_dev_board_init();

    /* 内置定时器服务任务：须在 cfg_load 之后、调度启动之前注册 */
    svcrt_timer_task_install();

    #if (SVCRT_USE_MPU == 1)
    svcrt_port_mpu_init();
    #endif
}
