/**
* @brief SVCrtOS ????
* @details ñü??
*          ????
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
