/**
* @brief SVCrtOS 操作系统内核文件
* @author chenjl
* @date 2019.07.01
*/

#include "kerOs.h"
#include "osconfigure.h"
#include "kernel.h"
#include "kerevent.h"
#include "kermpu.h"
#include "devsio.h"

uint32 KernelTick = 0;
int32  curTaskId = 0;
#if (SVCRT_USE_CPU_LOAD == 1)
uint16 KerCpuLoad = 0;
uint16 kerCpuIdleMill = 0;
#endif

static void Kernel_TickTasks(TASK_CONTEXT *pTask);

void SysTick_Handler(void)
{
    KernelTick++;
    SVCRT_SWITCH_TASK();

    #if (SVCRT_USE_CPU_LOAD == 1)
    if(curTaskId > 0)
    {
        KerCpuLoad++;
        if((KernelTick & 0x3ff) == 0)
        {
            kerCpuIdleMill = KerCpuLoad;
            KerCpuLoad = 0;
        }
    }
    #endif
}

void SVC_Server(SVC_CONTEXT *pSvcContext)
{
    uint32 svc_num = ((char*)pSvcContext->pc)[-2];
    uint32 *p;
    switch(svc_num)
    {
    case 0x13:
        p = (uint32*)pSvcContext->r0;
        switch(p[0])
        {
            case 1:
                pSvcContext->r0 = kerCreateEvent((char*)p[1]);
                break;
            case 2:
                kerWaitEvent(p[1],p[2]);
                break;
            case 3:
                kerSetEvent(p[1]);
                break;
            default:
                break;
        }
        break;
    case 0x12:
        switch(pSvcContext->r0)
        {
            case 1:
                pSvcContext->r0 = kerGetSystemTime();
                break;
            #if (SVCRT_USE_CPU_LOAD == 1)
            case 2:
                pSvcContext->r0 = KerGetCpuIdle();
                break;
            #endif
            default:
                break;
        }
        break;
    case 0x11:
        switch(pSvcContext->r0)
        {
            case 1:
                kerTaskWait(pSvcContext->r1);
                break;
            case 2:
                kerTaskWaitNxtPeriod();
                break;
            case 3:
                kerTaskDelay(pSvcContext->r1);
                break;
            case 4:
                kerTaskKill();
                break;
            default:
                break;
        }
        break;
    case 0x10:
        p = (uint32*)pSvcContext->r0;
        switch(p[0])
        {
            case 1:
                pSvcContext->r0 = kerDevOpen((char*)p[1],p[2]);
                break;
            case 2:
                pSvcContext->r0 = kerDevRead(p[1],(uint8*)p[2],p[3]);
                break;
            case 3:
                pSvcContext->r0 = kerDevWrite(p[1],(uint8*)p[2],p[3]);
                break;
            case 4:
                pSvcContext->r0 = kerDevCtrl(p[1],p[2],p[3]);
                break;
            default:
                pSvcContext->r0 = 0;
                break;
        }
        break;
    default:
        break;
    }
}

void HardFault_Handler(void)
{
    if(curTaskId > 0)
    {
        taskConfigurations[curTaskId - 1].status = TASK_STATUS_INVALID;
        SVCRT_SWITCH_TASK();
    }
    else
    {
        while(1)
        {
            SVCRT_NOP();
        }
    }
    SVCRT_ISB();
    SVCRT_WFE();
}

int32 Kernel_isSwitching()
{
    int32 newIdx = Kernel_NextTask() + 1;
    if(newIdx == curTaskId)
    {
        if(newIdx > 0)
        {
            taskConfigurations[newIdx-1].touchtick = KernelTick;
        }
        return -1;
    }
    else
    {
        return newIdx;
    }
}

uint32 safeBgStackPtr = 0;

int32 Kernel_Active(int32 nwTask,uint32 oldpsp)
{
    int32 tid = curTaskId - 1;

    if(curTaskId == 0)
    {
        safeBgStackPtr = oldpsp;
    }
    else
    {
        taskConfigurations[tid].stack_ptr = oldpsp;

        #if (SVCRT_USE_STACK_CHECK == 1)
        if(*taskConfigurations[tid].stack_buttom != STACK_END_FLAG)
        {
            taskConfigurations[tid].status = TASK_STATUS_INVALID;
        }
        else
        #endif
        {
            if(taskConfigurations[tid].status == TASK_STATUS_RUNNING)
            {
                taskConfigurations[tid].status = TASK_STATUS_READY;
            }
        }
    }
    curTaskId = nwTask;
    if(curTaskId > 0)
    {
        tid = curTaskId - 1;
        taskConfigurations[tid].touchtick = KernelTick;
        taskConfigurations[tid].status = TASK_STATUS_RUNNING;
        kerSetAppMpu(&taskConfigurations[tid]);
        return taskConfigurations[tid].stack_ptr;
    }
    else
    {
        curTaskId = 0;
        return safeBgStackPtr;
    }
}

int32 Kernel_NextTask(void)
{
    uint8 tmpPri = 255;
    uint32 tmpTouch = 0xffffffff;
    uint8 idx;
    int32 r = -1;
    for(idx=0;idx<task_init_num;idx++)
    {
        Kernel_TickTasks(taskConfigurations+idx);

        if((taskConfigurations[idx].status == TASK_STATUS_READY) ||
            (taskConfigurations[idx].status == TASK_STATUS_RUNNING))
        {
            if(taskConfigurations[idx].priority < tmpPri)
            {
                tmpPri = taskConfigurations[idx].priority;
                tmpTouch = taskConfigurations[idx].touchtick;
                r = idx;
            }
            else if(taskConfigurations[idx].priority == tmpPri)
            {
                if(taskConfigurations[idx].touchtick < tmpTouch)
                {
                    tmpTouch = taskConfigurations[idx].touchtick;
                    r = idx;
                }
            }
        }
    }
    return r;
}

static void Kernel_TickTasks(TASK_CONTEXT *pTask)
{
    uint32 usedtick = KernelTick;
    int32 escaptick = usedtick - pTask->timtick;

    if((pTask->status == TASK_STATUS_RUNNING) ||
        (pTask->status == TASK_STATUS_INVALID))
    {
        return;
    }

    pTask->timtick = usedtick;
    pTask->period_time -= escaptick;
    if(pTask->period_time < 0)
    {
        pTask->period_time += pTask->period;
        if(pTask->status == TASK_STATUS_WAIT)
        {
            pTask->status = TASK_STATUS_READY;
        }
    }

    if(pTask->wait_time > 0)
    {
        pTask->wait_time -= escaptick;
        if(pTask->wait_time <= 0)
        {
            if(pTask->status == TASK_STATUS_WAIT)
            {
                pTask->status = TASK_STATUS_READY;
            }
        }
        else
        {
            if(pTask->status != TASK_STATUS_INVALID)
            {
                pTask->status = TASK_STATUS_WAIT;
            }
        }
    }
}

TASK_CONTEXT *Kernel_GetCurTask(void)
{
    TASK_CONTEXT *pTsk = 0;
    SVCRT_DISABLE_IRQ();
    if(curTaskId > 0)
    {
        pTsk = &taskConfigurations[curTaskId - 1];
    }
    SVCRT_ENABLE_IRQ();
    return pTsk;
}

void kerTaskWait(uint32 ms)
{
    TASK_CONTEXT *pTsk;
    SVCRT_DISABLE_IRQ();
    if(curTaskId > 0)
    {
        pTsk = &taskConfigurations[curTaskId - 1];
        pTsk->wait_time = SVCRT_MS_TO_TICK(ms);
        pTsk->status = TASK_STATUS_WAIT;
        SVCRT_SWITCH_TASK();
    }
    SVCRT_ENABLE_IRQ();
}

void kerTaskWaitNxtPeriod(void)
{
    TASK_CONTEXT *pTsk;
    SVCRT_DISABLE_IRQ();
    if(curTaskId > 0)
    {
        pTsk = &taskConfigurations[curTaskId - 1];
        pTsk->status = TASK_STATUS_WAIT;
        SVCRT_SWITCH_TASK();
    }
    SVCRT_ENABLE_IRQ();
}

void kerTaskDelay(uint32 us)
{
    int32 tm_start = svcrtPortGetSysTickVal();
    int32 wait_clk = us * (SystemCoreClock / 1000000);
    int32 tm_end;
    int32 tm_diff;

    while(wait_clk > 0)
    {
        tm_end = svcrtPortGetSysTickVal();
        tm_diff = tm_start - tm_end;
        if(tm_diff < 0)
            tm_diff += svcrtPortGetSysTickLoad();
        tm_start = tm_end;

        wait_clk -= tm_diff;
    }
}

void kerTaskSwitch(void)
{
    SVCRT_SWITCH_TASK();
    SVCRT_WFE();
}

void kerTaskKill(void)
{
    if(curTaskId > 0)
    {
        taskConfigurations[curTaskId-1].status = TASK_STATUS_INVALID;
    }

    {
        SVCRT_SWITCH_TASK();
        SVCRT_WFE();
    }
}

uint32 kerGetSystemTime(void)
{
    return KernelTick / (1000 / SVCRT_TICK_PERIOD_US);
}

uint32 KerGetSystemTick(void)
{
    return KernelTick;
}

uint16 KerGetCpuIdle(void)
{
    #if (SVCRT_USE_CPU_LOAD == 1)
    return kerCpuIdleMill;
    #else
    return 0;
    #endif
}

void kerActiveHighPrior(uint8 ckPri)
{
    if(curTaskId > 0)
    {
        if(taskConfigurations[curTaskId-1].priority > ckPri)
        {
            SVCRT_SWITCH_TASK();
        }
    }
    else
    {
        SVCRT_SWITCH_TASK();
    }
}
