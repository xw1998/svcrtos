/**
* @brief SVCrtOS ??????????????????
* @details ??????????????????????????????????????
*          ????????????????????????????? svcrt.h
*/

#ifndef __SVCRT_TASK_H__
#define __SVCRT_TASK_H__

#include "svcrt_def.h"
#include "svcrt_port.h"
#include "svcrt_config.h"

#if (SVCRT_USE_FPU == 1)
#define SVCRT_FPU_USED 1
#else
#define SVCRT_FPU_USED 0
#endif

typedef enum {
    SVCRT_TASK_INVALID,
    SVCRT_TASK_READY,
    SVCRT_TASK_WAIT,
    SVCRT_TASK_RUNNING
} svcrt_task_status_t;

typedef struct {
    uint32  r4_r11[8];
    #if (SVCRT_FPU_USED == 1)
    uint32  sm[16];
    #endif
    uint32  r0;
    uint32  r1;
    uint32  r2;
    uint32  r3;
    uint32  r12;
    uint32  lr;
    uint32  pc;
    uint32  xpsr;
} svcrt_exc_context_t;

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
