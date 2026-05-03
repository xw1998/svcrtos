/**
* @brief SVCrtOS ÊÂ¼þÄ£¿é
* @author chenjl
* @date 2019.07.02
*/

#ifndef __KEREVENT_H__
#define __KEREVENT_H__

#include "kerOs.h"
#include "kernel.h"
#include "svcrt_config.h"

typedef struct {
    char eventName[16];
    TASK_CONTEXT *waitingTasks[SVCRT_MAX_EVENT_WAITERS];
}KER_EVENT_STR;

void kerEventModuleInit(void);
int32 kerCreateEvent(char *name);
int32 kerWaitEvent(int32 eventhandle,int32 timeout_ms);
void kerSetEvent(int32 eventhandle);

#endif
