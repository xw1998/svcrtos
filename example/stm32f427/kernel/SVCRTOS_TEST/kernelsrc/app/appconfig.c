/**
* @brief 应用程序配置文件模板
* @details 用户需要根据实际分区需求修改此文件中的配置参数
* @author xw
* @date 2026.05.03
*/

#include "svcrt_app_config.h"

__weak int32 svcrt_app_count = 2;

__weak svcrt_app_cfg_t svcrt_app_cfg_table[] =
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
