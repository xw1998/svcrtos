/**
* @brief SVCrtOS 任务配置加载
* @details 定义任务配置表和配置加载函数
* @author xw
* @date 2026.05.03
*/

#include "svcrt_cfg.h"
#include "svcrt_task.h"
#include "svcrt_config.h"

svcrt_task_t svcrt_task_table[SVCRT_TASK_MAX_NUM];
int32 svcrt_task_count = 0;

void svcrt_cfg_load(void)
{
    svcrt_task_count = 0;
}
