/**
* @file lwip_port.h
* @brief SVCrtOS 侧使用 lwIP 的入口（内核初始化时调一次）。
* @author xw
* @date 2026.09.23
*/
#ifndef SVCRT_LWIP_PORT_H
#define SVCRT_LWIP_PORT_H

#include "svcrt_types.h"

/** @brief lwIP 服务任务的优先级（低于 tcpip 线程的 12，见 lwipopts.h）。 */
#define SVCRT_LWIP_TASK_PRIO      (20u)
/** @brief 服务任务的周期（ms）。它现在没有周期工作——只是不能退出，
 *         所以按秒级躺着，将来接真实网卡驱动才有活干。 */
#define SVCRT_LWIP_TASK_PERIOD_MS (1000u)

/**
* @brief 注册 lwIP 的常驻服务任务（真正起 lwIP 的是这个任务的第一轮，见 .c）。
* @return 0=成功，负值=任务表满/参数非法
* @note  不能在 main() 里直接 tcpip_init()：那会阻塞在一个信号量上等 tcpip
*        线程，而 main() 跑在调度器启动之前，等不到任何任务。
*/
int32 svcrt_lwip_start(void);

#endif /* SVCRT_LWIP_PORT_H */
