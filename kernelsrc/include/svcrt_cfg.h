/**
* @brief SVCrtOS 任务配置与任务表声明
* @details 声明全局任务表、任务计数，以及任务配置加载与任务栈初始化接口。
*          svcrt_cfg_load 在内核启动时加载任务配置，svcrt_task_stack_init 初始化任务栈帧。
*/

#ifndef __SVCRT_CFG_H__
#define __SVCRT_CFG_H__

#include "svcrt_def.h"
#include "svcrt_task.h"
#include "svcrt_config.h"

extern svcrt_task_t svcrt_task_table[SVCRT_TASK_MAX_NUM];
extern int32 svcrt_task_count;

void svcrt_cfg_load(void);
void svcrt_task_stack_init(svcrt_task_t *p_task, void (*entry)(void),
                           uint32 *stack_bottom, uint32 stack_size);

#endif
