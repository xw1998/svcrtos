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
        TaskWait(1000);
    }
}
