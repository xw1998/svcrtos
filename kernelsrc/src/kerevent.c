/**
* @brief SVCrtOS ÊÂ¼þÄ£¿é
* @author chenjl
* @date 2019.07.02
*/

#include "kerevent.h"

static KER_EVENT_STR kerEvents[SVCRT_EVENT_NUM];

void kerEventModuleInit(void)
{
    int32 i,j;
    for(i=0;i<SVCRT_EVENT_NUM;i++)
    {
        for(j=0;j<8;j++)
            kerEvents[i].eventName[j] = 0;
        for(j=0;j<SVCRT_MAX_EVENT_WAITERS;j++)
            kerEvents[i].waitingTasks[j] = 0;
    }
}

int32 kerCreateEvent(char *name)
{
    int32 i;
    for(i=0;i<SVCRT_EVENT_NUM;i++)
    {
        if(kerEvents[i].eventName[0] == 0)
        {
            int32 j;
            for(j=0;j<15;j++)
            {
                kerEvents[i].eventName[j] = name[j];
                if(name[j] == 0)
                    break;
            }
            kerEvents[i].eventName[15] = 0;
            return (i | EVENT_HANDLE_FLAG);
        }
    }
    return -1;
}

int32 kerWaitEvent(int32 eventhandle,int32 timeout_ms)
{
    int32 idx = eventhandle & HANDLE_RELMASK;
    int32 j;
    TASK_CONTEXT *pTsk;
    if(idx >= SVCRT_EVENT_NUM)
        return -1;

    pTsk = Kernel_GetCurTask();
    if(pTsk == 0)
        return -1;

    for(j=0;j<SVCRT_MAX_EVENT_WAITERS;j++)
    {
        if(kerEvents[idx].waitingTasks[j] == 0)
        {
            kerEvents[idx].waitingTasks[j] = pTsk;
            break;
        }
    }

    if(timeout_ms > 0)
    {
        kerTaskWait(timeout_ms);
    }
    else
    {
        kerTaskWaitNxtPeriod();
    }
    return 0;
}

void kerSetEvent(int32 eventhandle)
{
    int32 idx = eventhandle & HANDLE_RELMASK;
    int32 j;
    if(idx >= SVCRT_EVENT_NUM)
        return;

    for(j=0;j<SVCRT_MAX_EVENT_WAITERS;j++)
    {
        if(kerEvents[idx].waitingTasks[j] != 0)
        {
            kerEvents[idx].waitingTasks[j]->status = TASK_STATUS_READY;
            kerEvents[idx].waitingTasks[j] = 0;
        }
    }
}
