/**
* @brief SVCrtOS App SDK 示例程序 - LED 闪烁与串口回显
* @details 演示 App SDK 的基本用法：设备 IO、互斥锁、信号量与延时。
*          通过内核加载器/安装任务自动落位到 App 槽位（地址见 config/svcrt_partition.h，
*          本文件不写地址）。
*          所有系统调用都经 SVC 陷入内核，App 不直接访问硬件。
*/

#include "svcrt.h"

void AppMain(void)
{
    int32 led  = svcrt_dev_open("LED", 0);
    int32 com1 = svcrt_dev_open("COM1", 115200);
    int32 sem  = svcrt_sem_create("app_sem", 0);
    int32 mtx  = svcrt_mutex_create("app_mtx");
    uint8 buf[64];
    uint8 on;
    uint32 tick;

    while(1)
    {
        /* 互斥锁保护对 LED 设备的写入 */
        svcrt_mutex_lock(mtx, -1);    /* -1 = 永久等待（0 现在是“不等待”） */
        on = 1;
        svcrt_dev_write(led, &on, 1);
        svcrt_mutex_unlock(mtx);

        svcrt_task_wait(500);

        svcrt_mutex_lock(mtx, -1);    /* -1 = 永久等待（0 现在是“不等待”） */
        on = 0;
        svcrt_dev_write(led, &on, 1);
        svcrt_mutex_unlock(mtx);

        /* 串口回显 */
        if(com1 >= 0)
        {
            int32 len = svcrt_dev_read(com1, buf, sizeof(buf));
            if(len > 0)
            {
                svcrt_dev_write(com1, buf, len);
                /* 收到数据后释放信号量（此处仅演示 post） */
                svcrt_sem_post(sem);
            }
        }

        tick = svcrt_get_time_ms();
        (void)tick;

        svcrt_task_wait(500);
    }
}
