/**
* @brief SVCrtOS App SDK - 应用程序入口
*/

#include "svcrt.h"

__weak void AppMain(void)
{
}

int main(void)
{
    AppMain();
    while(1)
    {
        svcrt_task_wait(1000);
    }
}
