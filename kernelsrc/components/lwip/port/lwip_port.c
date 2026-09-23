/**
* @file lwip_port.c
* @brief 把 lwIP 起起来。
* @details 这块板上没有以太网 PHY（仓库里只有 CubeMX 留下的 ETH/PHY 宏，没有
*          任何 ETH 初始化），所以现在的网络面是**回环**：lwIP 自己带一张 lo
*          网卡，由 lwip_init() 里的 netif_init() 建好（127.0.0.1/8，input 接
*          tcpip_input）。这不影响 socket/select 的语义验证——连接、收发、
*          关闭、超时走的是同一条 TCP 状态机，只是包不出芯片。
*
*          这里只做一件事：**在一个内核任务里调一次 tcpip_init()**。
*          为什么不能放在 main()：tcpip_init() 会等 tcpip 线程把初始化做完
*          （内部一个信号量），而 main() 跑在调度器启动之前，等不到任何任务。
*
*          为什么不需要周期调 netif_poll()：回环是
*          LWIP_NETIF_LOOPBACK_MULTITHREADING=1（见 lwipopts.h 里的说明），
*          netif_loop_output() 会用 tcpip_try_callback() 把 netif_poll 排给
*          tcpip 线程，包由它自己喂回协议栈。只有多线程 =0 的轮询模式下，
*          "没人调 netif_poll_all()" 才会表现为"连接永远建不起来且不报错"。
*
* @author xw
* @date 2026.09.23
*/
#include "lwip/opt.h"
#include "lwip/tcpip.h"
#include "lwip/netif.h"

#include "svcrt.h"
#include "svcrt_cfg.h"
#include "svcrt_task.h"      /* wait_period_internal */
#include "svcrt_log.h"

#include "lwip_port.h"

#if (SVCRT_USE_LWIP == 1)

static uint32 g_lwip_task_stack[256];    /* 1 KB：只调一次 tcpip_init */

static void svcrt_lwip_task(void)
{
    /* 第一轮：把 lwIP 与 tcpip 线程起起来。tcpip_init() 会等线程把初始化
     * 做完（内部一个信号量），所以这一轮比其他轮久——本任务由内核按周期
     * 调度，第一轮超时不影响后续节拍。 */
    tcpip_init(NULL, NULL);
    SVCRT_LOGI("LWIP", "tcpip up, loopif 127.0.0.1");

    /* 之后没有周期工作。任务不能退出（内核没有"结束自己"这个语义），所以
     * 按周期躺着；将来接真实以太网驱动时，收包搬运就落在这个循环里。 */
    for(;;)
    {
        svcrt_task_wait_period_internal();
    }
}

int32 svcrt_lwip_start(void)
{
    return svcrt_task_register(svcrt_lwip_task,
                               g_lwip_task_stack,
                               (uint32)sizeof(g_lwip_task_stack),
                               (uint8)SVCRT_LWIP_TASK_PRIO,
                               (uint32)SVCRT_LWIP_TASK_PERIOD_MS);
}

#else   /* SVCRT_USE_LWIP != 1 */

int32 svcrt_lwip_start(void)
{
    return 0;       /* 关掉时不注册任务，也不碰 lwIP 的符号 */
}

#endif  /* SVCRT_USE_LWIP */
