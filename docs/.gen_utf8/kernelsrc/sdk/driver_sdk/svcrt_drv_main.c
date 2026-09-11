/**
* @brief SVCrtOS Driver SDK - 驱动入口(用户态模式)
* @details 用户态驱动的主入口，驱动开发者实现 DrvMain() 函数
*          驱动在用户态运行，通过 SVC 调用与内核交互
*/

#include "svcrt_driver_sdk.h"

__weak void DrvMain(void)
{
}

int main(void)
{
    DrvMain();
    while(1)
    {
    }
}
