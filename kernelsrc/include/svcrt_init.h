/**
* @file svcrt_init.h
* @brief SVCrtOS 内核启动初始化接口
* @details 把"内核需要初始化哪些模块、以什么顺序"收敛到唯一一处实现
*          （svcrt_init.c 的 svcrt_kernel_module_init），避免出现两份启动流程
*          （内核自带的弱 main 与工程里的 main）各自演化、
*          最终导致某个模块在某个工程下漏初始化的问题。
*
*          工程侧 main() 的推荐启动顺序：
*          1) HAL / 时钟 / 外设初始化（CubeMX 生成部分）
*          2) svcrt_port_board_init()
*          3) svcrt_cfg_load()
*          4) svcrt_register_tasks()（注册本工程的板级任务，可选）
*          5) svcrt_port_irq_init()
*          6) svcrt_kernel_module_init()   <-- 本文件提供
*          7) 分区表与外部映像：svcrt_ptable_init() → svcrt_loader_scan*()/start*()
*             → svcrt_installer_init()
*          8) svcrt_start_idle()（进入空闲任务，启动调度）
*/

#ifndef __SVCRT_INIT_H__
#define __SVCRT_INIT_H__

#if (defined(__cplusplus))
extern "C" {
#endif

/**
* @brief 初始化全部内核模块，并注册内置服务任务
* @details 顺序：事件 → 同步 → 消息队列 → 软定时器 → 故障记录 → 设备 → 板级设备，
*          然后注册软定时器服务任务，最后（需要时）初始化 MPU。
*          必须在 svcrt_cfg_load() 之后、调度启动之前调用。
* @note 任何启动路径都应调用本函数，不要在工程侧重复逐个调用各模块 init。
*/
void svcrt_kernel_module_init(void);

#if (defined(__cplusplus))
}
#endif

#endif /* __SVCRT_INIT_H__ */
