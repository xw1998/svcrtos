/**
* @brief SVCrtOS Driver SDK - 驱动入口(用户态模式)
* @details 用户态驱动的主入口，驱动开发者实现 DrvMain() 函数
*          驱动在用户态运行，通过 SVC 调用与内核交互
*/

#include "svcrt_driver_sdk.h"

SVCRT_WEAK void DrvMain(void)
{
}

int main(void)
{
    DrvMain();

    /* 驱动注册完就该让出 CPU。这里不能写空转的 while(1)：
     * 驱动任务优先级(9)高于 App 任务(10)，空转会吃满 CPU 把 App 全饿死。
     * 与 svcrt_app_main.c 的兜底循环保持一致，靠 svcrt_task_wait 让出时间片。 */
    while(1)
    {
        svcrt_task_wait(1000);
    }
}
