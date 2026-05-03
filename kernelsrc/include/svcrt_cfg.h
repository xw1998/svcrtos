/**
* @brief SVCrtOS 任务配置模块（内核内部）
* @details 定义任务配置表和配置加载接口
*          此文件仅供内核内部使用
*/

#ifndef __SVCRT_CFG_H__
#define __SVCRT_CFG_H__

#include "svcrt_def.h"
#include "svcrt_task.h"
#include "svcrt_config.h"

extern svcrt_task_t svcrt_task_table[SVCRT_TASK_MAX_NUM];
extern int32 svcrt_task_count;

void svcrt_cfg_load(void);

#endif
