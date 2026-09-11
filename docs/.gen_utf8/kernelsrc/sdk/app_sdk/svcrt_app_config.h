/**
* @brief SVCrtOS App SDK - 应用程序配置
*/

#ifndef __SVCRT_APP_CONFIG_H__
#define __SVCRT_APP_CONFIG_H__

#include "svcrt_types.h"

typedef struct {
    uint32  ram_start;
    uint32  ram_size;
    uint32  stack_size;
    uint32  rom_start;
    uint32  rom_size;
    uint32  period_ms;
    uint32  priority;
    uint8   share_mem_access;
} svcrt_app_cfg_t;

extern svcrt_app_cfg_t svcrt_app_cfg_table[];
extern int32 svcrt_app_count;

#endif
