/**
* @file svcrt_net.h
* @brief 内核侧网络服务（SVC 0x1E）的入口。
* @details 为什么不是"SVC 里直接调 lwIP"：lwIP 的 socket API 会把请求投给
*          tcpip 线程并等它的完成信号量，也就是要阻塞；而 SVC 处理程序里
*          不能阻塞（PendSV 不会抢占 SVC，见 svcrt_hal.h 的
*          svcrt_sync_in_handler 说明）。所以拆成两边：
*            - SVC 处理程序只登记请求、立刻返回 SVCRT_NET_EPENDING；
*            - 一个内核服务任务用 thread mode 执行真正会阻塞的 lwIP 调用，
*              把结果放进请求槽；调用方（App SDK）轮询同一个请求拿结果。
*          真正的等待发生在 App 侧，和信号量/互斥锁的"登记 + WOULDBLOCK"
*          是同一套思路，只是这里多了一个执行者。
*
*          服务任务只调**非阻塞**的 lwIP 调用（socket 建好即设 O_NONBLOCK），
*          所以它自己几乎不占时间，一个请求槽就够。
*
* @note 只有内核包含。协议常量在 svcrt_net_abi.h（内核与 App 共用）。
* @author xw
*/
#ifndef __SVCRT_NET_H__
#define __SVCRT_NET_H__

#include "svcrt_types.h"
#include "svcrt_net_abi.h"

/** @brief 网络服务任务的优先级。tcpip 线程是 12（见 lwipopts.h），
 *         服务任务要低一些，让 tcpip 能先跑完它投递的消息。 */
#define SVCRT_NET_TASK_PRIO        (14u)
/** @brief 服务任务栈（字）。只跑 lwIP 调用与一次 4 KB 以内的小拷贝。 */
#define SVCRT_NET_TASK_STACK_WORDS (384u)

/**
* @brief 起网络服务任务（幂等）。由 lwIP 任务在 tcpip_init() 之后调用，
*        这样服务任务开始受理请求时 tcpip 线程一定已经在跑。
* @return 0=成功，负值=服务未编入（SVCRT_USE_LWIP=0）/任务表满
*/
int32 svcrt_net_start(void);

/**
* @brief 网络服务是否已经起来。App 侧用它区分"服务没编进去"和"请求失败"。
* @return 1=可用，0=不可用
*/
uint8 svcrt_net_ready(void);

/**
* @brief SVC 0x1E 的分发入口。**不阻塞**：登记请求或取回结果，随即返回。
* @param p_svc_ctx 系统调用上下文（SVCRT_SVC_ARG/RET 用）
* @return 见 svcrt_net_abi.h 的返回约定
*/
int32 svcrt_net_svc(void *p_svc_ctx);

/**
* @brief 归还某个任务名下的所有 socket。任务被卸载/故障回收时调用，
*        否则泄漏的是内核侧 socket 槽 —— 一个 App 反复崩溃会把它们耗干。
* @param task_id 目标任务号（1 起）
*/
void svcrt_net_release_task(int32 task_id);

#endif /* __SVCRT_NET_H__ */
