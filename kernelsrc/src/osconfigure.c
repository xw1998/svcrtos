/**
* @brief SVCrtOS 操作系统参数配置
* @author chenjl
* @date 2019.07.01
*/

#include "osconfigure.h"
#include "kernel.h"
#include "svcrt_config.h"

TASK_CONTEXT taskConfigurations[SVCRT_TASK_MAX_NUM];
int32 task_init_num = 0;

void LoadConfiguration(void)
{
    task_init_num = 0;
}
