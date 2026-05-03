/**
* @brief SVCrtOS App SDK - 示例应用程序
*/

#include "svcrt.h"

void AppMain(void)
{
    int32 com1 = dev_open("COM1", 0);
    int32 led  = dev_open("LED", 0);
    int32 evt  = CreateEvent("my_event");
    uint8 buf[64];
    uint32 tick = 0;

    while(1)
    {
        dev_write(led, (void*)&tick, 4);

        if(com1 >= 0)
        {
            int32 len = dev_read(com1, buf, 64);
            if(len > 0)
            {
                dev_write(com1, buf, len);
            }
        }

        tick = GetSystemTimeMs();

        TaskWait(500);
    }
}
