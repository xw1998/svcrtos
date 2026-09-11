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
#if (SVCRT_USE_STACK_USAGE == 1)
/**
* @brief 用填充图案覆盖整段任务栈（栈底保护字除外）
* @param stack_bottom 栈底地址
* @param stack_size   栈大小（字节）
* @note 仅在任务创建/重建时调用，用于后续统计峰值栈用量。
*/
void svcrt_task_stack_fill(uint32 *stack_bottom, uint32 stack_size);
#endif

int32 svcrt_task_register(void (*entry)(void), uint32 *stack_bottom, uint32 stack_size,
                          uint8 priority, uint32 period_ms);
void svcrt_task_stack_init(svcrt_task_t *p_task, void (*entry)(void),
                           uint32 *stack_bottom, uint32 stack_size);

#endif
