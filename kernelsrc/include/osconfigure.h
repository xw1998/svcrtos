/**
* @brief SVCrtOS 操作系统参数配置文件
* @author chenjl
* @date 2019.07.01
*/

#ifndef __OSCONFIGURE_H__
#define __OSCONFIGURE_H__

#include "kerOs.h"
#include "kernel.h"
#include "svcrt_config.h"

extern TASK_CONTEXT taskConfigurations[SVCRT_TASK_MAX_NUM];

extern void LoadConfiguration(void);
extern int32 task_init_num;

#endif
