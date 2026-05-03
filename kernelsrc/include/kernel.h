/**
* @brief SVCrtOS 操作系统内核头文件
* @author chenjl
* @date 2019.07.01
*/

#ifndef __KERNEL_H__
#define __KERNEL_H__

#include "kerOs.h"
#include "svcrt_config.h"

#if (SVCRT_USE_FPU == 1)
#define SVCRT_FPU_USED 1
#else
#define SVCRT_FPU_USED 0
#endif

typedef enum {
    TASK_STATUS_INVALID,
    TASK_STATUS_READY,
    TASK_STATUS_WAIT,
    TASK_STATUS_RUNNING
}TASK_STATUS;

typedef struct {
    #if (SVCRT_FPU_USED == 1)
    uint32  sm[16];
    #endif
    uint32  r4_r11[8];
    uint32  r0;
    uint32  r1;
    uint32  r2;
    uint32  r3;
    uint32  r12;
    uint32  lr;
    uint32  pc;
    uint32 xpsr;
    #if (SVCRT_FPU_USED == 1)
    uint32  sa[16];
    uint32  fpscr;
    #endif
}EXCEPTION_CONTEXT;

typedef struct {
    uint32 r0;
    uint32 r1;
    uint32 r2;
    uint32 r3;
    uint32 r12;
    uint32 lr;
    uint32 pc;
    uint32 xpsr;
}SVC_CONTEXT;

typedef struct {
    uint32 ram_start;
    uint32 ram_size;
    uint32 stack_size;
    uint32 rom_start;
    uint32 rom_size;
    int32  period;
    uint8  priority;
    uint8  shmAttri;

    uint32 stack_top;
    uint32 *stack_buttom;
    #if (SVCRT_USE_MPU == 1)
    uint32 mpu_bar[8];
    uint32 mpu_asr[8];
    #endif
    TASK_STATUS status;
    int32  period_time;
    int32  wait_time;
    uint32 timtick;
    uint32 touchtick;
    uint32 stack_ptr;
}TASK_CONTEXT;

#if (SVCRT_USE_STACK_CHECK == 1)
#define STACK_END_FLAG  SVCRT_STACK_END_FLAG
#else
#define STACK_END_FLAG  (0xed01)
#endif

#if (SVCRT_USE_CPU_LOAD == 1)
extern uint16 kerCpuIdleMill;
#endif

int32 Kernel_NextTask(void);
TASK_CONTEXT *Kernel_GetCurTask(void);
uint32 kerGetSystemTime(void);
uint32 KerGetSystemTick(void);
uint16 KerGetCpuIdle(void);

void kerTaskWait(uint32 ms);
void kerTaskWaitNxtPeriod(void);
void kerTaskDelay(uint32 us);
void kerTaskKill(void);

void kerActiveHighPrior(uint8 ckPri);

#endif
