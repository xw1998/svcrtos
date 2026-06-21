/**
* @brief SVCrtOS App SDK 蓝灯应用 - 分区配置
*/

#include "svcrt_app_config.h"

svcrt_app_cfg_t svcrt_app_cfg_table[] = {
    {
        0x2001C000,   /* ram_start */
        0x00002000,   /* ram_size：8KB */
        0x00000800,   /* stack_size：2KB */
        0x08080000,   /* rom_start */
        0x00020000,   /* rom_size：128KB */
        800,          /* period_ms */
        10,           /* priority */
        0             /* share_mem_access */
    }
};

int32 svcrt_app_count = 1;
