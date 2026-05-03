/**
* @brief 应用程序配置文件
* @author chenjl
* @date 2019.07.15
*/

#ifndef __APPCONFIG_H__
#define __APPCONFIG_H__

#include "kerOs.h"

typedef struct {
    uint32  ram_start;
    uint32  ram_size;
    uint32  stack_size;
    uint32  rom_start;
    uint32  rom_size;
    uint32  period_ms;
    uint32  priority;
    uint8   share_mem_access;
}APPCONFIG_STR;

extern APPCONFIG_STR app_configures[];
extern int32 app_num;

#endif
