/**
* @brief SVCrtOS 内核启动与初始化（单一权威实现）
* @details 本文件只提供 svcrt_kernel_module_init()：内核各模块的初始化顺序在此唯一确定。
*          内核【不提供 main() 入口】，入口一律由工程侧 main() 提供——
*          同一个映像里存在两个 main 时，AC6 会因辅助符号 __ARM_use_no_argv
*          重复定义而链接失败（L6200E: by svcrt_init.o and main.o），
*          即“弱 main 可被工程 main 覆盖”在 AC6 下并不成立。
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
#include "svcrt_mpu.h"
#include "svcrt_config.h"
#include "svcrt_init.h"

/* 内核模块初始化：唯一权威顺序实现（任何工程的 main 都只调用它） */
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
    svcrt_mpu_module_init();
    #endif
}
