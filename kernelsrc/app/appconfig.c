/**
* @brief 应用程序配置文件模板
* @details 用户需要根据实际分区需求修改此文件中的配置参数
* @author chenjl
* @date 2019.07.15
*/

#include "appconfig.h"

__weak int32 app_num = 2;

__weak APPCONFIG_STR app_configures[] =
{
    {
        0x20000000,
        0x1000,
        0x400,
        0x08020000,
        0x20000,
        1000,
        10,
        0
    },
    {
        0x20001000,
        0x1000,
        0x400,
        0x08040000,
        0x20000,
        1000,
        10,
        0
    }
};
