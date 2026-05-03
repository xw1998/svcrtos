/**
* @brief SVCrtOS Æô¶¯ÎÄ¼þ
* @author chenjl
* @date 2019.07.01
*/

#include "osconfigure.h"
#include "kernel.h"
#include "kerevent.h"
#include "kermpu.h"
#include "devsio.h"
#include "svcrt_config.h"

static void StartTask(void);
static void OsInit(void);
static uint32 safetyStack[100];

int main(void)
{
    svcrtPortBoardInit();

    LoadConfiguration();

    svcrtPortIrqInit();

    OsInit();

    svcrtPortStartTimer(SVCRT_TICK_PERIOD_US);

    StartTask();
    return 0;
}

static void StartTask(void)
{
    SVCRT_SET_PSP((uint32)(safetyStack+99));

    #if (SVCRT_USE_MPU == 1)
    svcrtPortSetIdleMpu((uint32)StartTask, (uint32)safetyStack, sizeof(safetyStack));
    #endif

    #if (SVCRT_USE_PRIV == 1)
    SVCRT_SET_CONTROL(0x3 | SVCRT_GET_CONTROL());
    #else
    SVCRT_SET_CONTROL(0x2 | SVCRT_GET_CONTROL());
    #endif
    SVCRT_ISB();
    SVCRT_WFI();

    while(1)
    {
    }
}

static void OsInit(void)
{
    kerEventModuleInit();

    #if (SVCRT_USE_MPU == 1)
    kerMpuMdlInit();
    #endif
}
