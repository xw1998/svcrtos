/**
* @brief SVCrtOS App SDK - 示例应用程序
*/

#include "svcrt.h"

void AppMain(void)
{
    int32 com1 = svcrt_dev_open("COM1", 0);
    int32 led  = svcrt_dev_open("LED", 0);
    int32 evt  = svcrt_event_create("my_event");
    uint8 buf[64];
    uint32 tick = 0;

    while(1)
    {
        svcrt_dev_write(led, (void *)&tick, 4);

        if(com1 >= 0)
        {
            int32 len = svcrt_dev_read(com1, buf, 64);
            if(len > 0)
            {
                svcrt_dev_write(com1, buf, len);
            }
        }

        tick = svcrt_get_time_ms();

        svcrt_task_wait(500);
    }
}
