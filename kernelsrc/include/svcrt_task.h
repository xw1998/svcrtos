/**
* @brief SVCrtOS 任务与调度相关定义（内核内部头文件）
* @details 定义任务控制块、任务状态、SVC 上下文结构，以及任务管理和调度器函数。
*          这些是内核内部接口，应用/驱动一般不直接包含本文件，而是通过 SDK 或 svcrt.h 使用。
*          其中 stack_ptr 字段用于保存/恢复任务的栈指针，是上下文切换的关键。
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
* @brief SVC 调用时硬件压栈的上下文结构
* @details 进入 SVC_Handler 后，硬件已将以下寄存器压入调用者栈帧。
*          SVC_Server() 通过指向该结构的指针读取调用参数（r0~r3）和返回地址（pc），
*          并可将返回值写回 r0。pc 用于回溯取出 SVC 立即数（SVC 号）。
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
* @brief 任务控制块（TCB）
* @details 保存一个任务的全部运行信息：内存区域、优先级、栈、状态、调度计时等。
*          所有任务的 TCB 排成 svcrt_task_table 数组。其中 stack_ptr 在任务被切走时
*          保存其 PSP，切回时据此恢复现场；mpu_bar/mpu_asr 是该任务的 MPU 区域快照。
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
void svcrt_task_block_internal(void);
void svcrt_task_delay_internal(uint32 us);
void svcrt_task_kill_internal(void);

void svcrt_sched_activate_higher(uint8 ck_pri);

int32 svcrt_sched_is_switching(void);
int32 svcrt_sched_activate(int32 new_task, uint32 old_psp);

void svcrt_kernel_tick_handler(void);
void svcrt_hardfault_handler(void);

#endif
