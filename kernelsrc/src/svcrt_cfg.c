/**
* @brief SVCrtOS 任务配置加载模块
* @details 管理任务表和任务栈初始化。
*          栈帧格式由port层的 svcrt_port_stack_init() 实现，
*          内核不再硬编码任何架构特定的寄存器布局。
* @author xw
* @date 2026.05.03
*/

#include "svcrt_cfg.h"
#include "svcrt_task.h"
#include "svcrt_hal.h"
#include "svcrt_config.h"

svcrt_task_t svcrt_task_table[SVCRT_TASK_MAX_NUM];
int32 svcrt_task_count = 0;

void svcrt_cfg_load(void)
{
    svcrt_task_count = 0;
}

void svcrt_task_stack_init(svcrt_task_t *p_task, void (*entry)(void),
                           uint32 *stack_bottom, uint32 stack_size)
{
    uint32 stack_top = (uint32)stack_bottom + stack_size;

    stack_top &= ~0x7u;

    p_task->stack_bottom = stack_bottom;
    p_task->stack_top    = stack_top;

    p_task->stack_ptr = svcrt_port_stack_init(stack_top, entry);
}
