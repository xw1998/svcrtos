/**
* @brief SVCrtOS App SDK 示例 - 操作蓝灯的用户态应用
* @details 演示外部用户态 App 通过设备框架操作由外部 Driver 注册的蓝灯设备。
*          App 不直接碰硬件，只通过 svcrt_dev_open("BLED") 拿到句柄后读写，
*          硬件操作全部由 BLED_DRV 用户态驱动完成。
*
*          编译为独立 .bin 固件，烧录到 ROM 分区。
*
*          运行依赖：BLED_DRV 驱动固件必须先注册 "BLED" 设备，否则 open 失败。
*/

#include "svcrt.h"

void AppMain(void)
{
    int32 bled;
    uint8 on;

    /* 等待蓝灯驱动注册完成（轮询打开，失败则稍后重试） */
    do {
        bled = svcrt_dev_open("BLED", 0);
        if(bled < 0)
            svcrt_task_wait(100);
    } while(bled < 0);

    while(1)
    {
        on = 1;
        svcrt_dev_write(bled, &on, 1);     /* 点亮蓝灯 */
        svcrt_task_wait(800);

        on = 0;
        svcrt_dev_write(bled, &on, 0);     /* 熄灭蓝灯 */
        svcrt_task_wait(800);
    }
}
