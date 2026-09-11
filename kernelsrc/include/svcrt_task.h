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

/* ============================================================
 * 系统调用上下文（架构无关）
 * @brief SVC_Server 只接收不透明的上下文指针，参数与返回值统一通过
 *        SVCRT_SVC_ARG / SVCRT_SVC_RET 宏访问（见 svcrt_hal.h）。
 *        各架构的栈帧布局定义在 kernelsrc/port/<族>/<核心>/，
 *        内核不再包含任何寄存器排布，移植新架构无需改动本文件。
 * ============================================================ */
void SVC_Server(void *p_svc_ctx);


/**
* @brief 任务控制块（TCB）
* @details 保存一个任务的全部运行信息：内存区域、优先级、栈、状态、调度计时等。
*          所有任务的 TCB 排成 svcrt_task_table 数组。其中 stack_ptr 在任务被切走时
*          保存其 PSP，切回时据此恢复现场；mpu 是该任务的 MPU 区域快照。
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
    svcrt_arch_mpu_t mpu;           /* 架构无关的 MPU 区域上下文（见 svcrt_arch.h） */
    #endif
    svcrt_task_status_t status;
    int32  period_time;
    int32  wait_time;
    uint32 tim_tick;
    uint32 touch_tick;
    uint32 stack_ptr;
    uint8  recover_pending;                         /* 故障恢复待处理标记（两阶段恢复） */
    void (*entry)(void);                            /* 任务入口（恢复重建用） */
    uint8  wake_reason;                             /* 0=被唤醒 1=超时唤醒 */
    #if (SVCRT_USE_STACK_USAGE == 1)
    uint32 stack_peak_low;                          /* 历史最低栈指针，用于峰值栈用量统计 */
    #endif
} svcrt_task_t;

#if (SVCRT_USE_STACK_CHECK == 1)
#define SVCRT_STACK_END_FLAG_VAL  SVCRT_STACK_END_FLAG
#else
#define SVCRT_STACK_END_FLAG_VAL  (0xed01)
#endif

#if (SVCRT_USE_CPU_LOAD == 1)
extern uint16 svcrt_cpu_idle_millis;
#endif

extern volatile uint32 svcrt_interrupt_nest;
extern uint32 svcrt_kernel_tick;
extern int32  svcrt_current_task_id;
#if (SVCRT_USE_SCHED_LOCK == 1)
/* 调度器锁：>0 表示当前禁止任务切换（用户任务经 SVC 调用 svcrt_sched_lock） */
extern volatile uint32 svcrt_sched_lock_nest;

void   svcrt_sched_lock_internal(void);
uint32 svcrt_sched_unlock_internal(void);
int32  svcrt_sched_lock_count_internal(void);
#endif

#if (SVCRT_USE_STACK_USAGE == 1)
/* 查询任务栈信息：out3[0]=总字节，out3[1]=峰值已用字节，out3[2]=剩余字节 */
int32  svcrt_task_stack_info_internal(int32 task_id, uint32 *out3);
#endif


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
int32  svcrt_task_status_get_internal(int32 task_id);
int32  svcrt_task_recover(int32 task_id);

void svcrt_sched_activate_higher(uint8 ck_pri);

int32 svcrt_sched_is_switching(void);
int32 svcrt_sched_activate(int32 new_task, uint32 old_psp);

void svcrt_kernel_tick_handler(void);
void svcrt_hardfault_handler(void);

#endif
