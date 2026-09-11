/**
* @brief SVCrtOS 软定时器模块（内核内部头文件）
* @details 提供单次/周期软定时器。到期回调统一在专用定时器服务任务
*          （svcrt_timer_task_install 注册）上下文中执行，
*          不在中断里运行用户回调，保持 SVC 特权隔离模型不被破坏。
*          tick 中断只做倒计时与到期标记，然后唤醒定时器任务。
*/

#ifndef __SVCRT_TIMER_H__
#define __SVCRT_TIMER_H__

#include "svcrt_def.h"
#include "svcrt_task.h"
#include "svcrt_config.h"

#if (SVCRT_USE_TIMER == 1)

#define SVCRT_TIMER_MODE_ONESHOT    0
#define SVCRT_TIMER_MODE_PERIODIC   1

typedef struct {
    char   name[8];
    uint8  used;                 /* 槽位是否占用 */
    uint8  active;               /* 是否在计时 */
    uint8  mode;                 /* SVCRT_TIMER_MODE_x */
    uint8  expired;              /* 已到期待回调标记（定时器任务消费） */
    uint32 period_ticks;         /* 定时周期（tick） */
    uint32 remain_ticks;         /* 剩余 tick */
    void (*cb)(void *arg);       /* 到期回调，在定时器任务上下文执行 */
    void  *arg;                  /* 回调参数 */
} svcrt_timer_obj_t;

void  svcrt_timer_module_init(void);
void  svcrt_timer_task_install(void);

/* 由 svcrt_kernel_tick_handler 在 tick 中断中调用：倒计时 + 到期标记 + 唤醒定时器任务 */
void  svcrt_timer_tick_handler(void);

int32 svcrt_timer_create_internal(char *name);
int32 svcrt_timer_start_internal(int32 handle, uint32 period_ms, uint8 mode,
                                 void (*cb)(void *), void *arg);
int32 svcrt_timer_stop_internal(int32 handle);
int32 svcrt_timer_delete_internal(int32 handle);

#endif /* SVCRT_USE_TIMER */

#endif
