/**
* @brief SVCrtOS 任务管理模块（内核内部）
* @details 定义任务控制块、任务状态和内核内部任务操作函数。
*          任务上下文保存/恢复由port层实现，内核不定义具体寄存器布局。
*          应用程序使用的接口请参见 svcrt.h
*/

#ifndef __SVCRT_TASK_H__
#define __SVCRT_TASK_H__

#include "svcrt_def.h"
#include "svcrt_hal.h"
#include "svcrt_config.h"

typedef enum {
    SVCRT_TASK_INVALID,
    SVCRT_TASK_READY,
    SVCRT_TASK_WAIT,
    SVCRT_TASK_RUNNING
} svcrt_task_status_t;

/**
* @brief SVC服务调用上下文
* @details 由port层的SVC_Handler传递给内核 SVC_Server()。
*          不同架构的寄存器布局不同，port层负责将架构特定的
*          栈帧指针转换为内核可操作的统一结构。
*          内核通过此结构读写SVC调用的参数和返回值。
*/
typedef struct {
    uint32 r0;
    uint32 r1;
    uint32 r2;
    uint32 r3;
    uint32 r12;
    uint32 lr;
    uint32 pc;
    uint32 xpsr;
} svcrt_svc_context_t;

/**
* @brief 任务控制块
* @details 包含任务的所有管理信息，不包含具体寄存器布局
*          寄存器保存区由port层的上下文切换汇编管理，
*          内核仅通过 stack_ptr 保存/恢复上下文指针
*/
typedef struct {
    uint32 ram_start;
    uint32 ram_size;
    uint32 stack_size;
    uint32 rom_start;
    uint32 rom_size;
    int32  period;
    uint8  priority;
    uint8  shm_attri;

    uint32 stack_top;
    uint32 *stack_bottom;
    #if (SVCRT_USE_MPU == 1)
    uint32 mpu_bar[8];
    uint32 mpu_asr[8];
    #endif
    svcrt_task_status_t status;
    int32  period_time;
    int32  wait_time;
    uint32 tim_tick;
    uint32 touch_tick;
    uint32 stack_ptr;
} svcrt_task_t;

#if (SVCRT_USE_STACK_CHECK == 1)
#define SVCRT_STACK_END_FLAG_VAL  SVCRT_STACK_END_FLAG
#else
#define SVCRT_STACK_END_FLAG_VAL  (0xed01)
#endif

#if (SVCRT_USE_CPU_LOAD == 1)
extern uint16 svcrt_cpu_idle_millis;
#endif

extern uint32 svcrt_kernel_tick;
extern int32  svcrt_current_task_id;

int32  svcrt_sched_next(void);
svcrt_task_t *svcrt_task_get_current(void);
uint32 svcrt_kernel_get_time(void);
uint32 svcrt_kernel_get_tick(void);
uint16 svcrt_kernel_get_cpu_idle(void);

void svcrt_task_wait_internal(uint32 ms);
void svcrt_task_wait_period_internal(void);
void svcrt_task_delay_internal(uint32 us);
void svcrt_task_kill_internal(void);

void svcrt_sched_activate_higher(uint8 ck_pri);

int32 svcrt_sched_is_switching(void);
int32 svcrt_sched_activate(int32 new_task, uint32 old_psp);

void svcrt_kernel_tick_handler(void);
void svcrt_hardfault_handler(void);

#endif
