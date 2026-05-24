/**
* @brief SVCrtOS 任务配置加载
* @details 定义任务配置表、配置加载函数与任务栈初始化函数
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

/* ============================================================
 * 任务栈初始化
 * @brief 在任务栈顶构造一个伪异常返回上下文,
 *        使得任务首次被调度时能从 entry 处开始执行。
 * ============================================================ */
void svcrt_task_stack_init(svcrt_task_t *p_task, void (*entry)(void),
                           uint32 *stack_bottom, uint32 stack_size)
{
    svcrt_exc_context_t *p_ctx;
    int32  i;
    uint32 stack_top = (uint32)stack_bottom + stack_size;

    /* 栈顶 8 字节对齐(AAPCS 要求) */
    stack_top &= ~0x7u;

    p_task->stack_bottom = stack_bottom;
    p_task->stack_top    = stack_top;

    stack_top -= sizeof(svcrt_exc_context_t);
    p_ctx = (svcrt_exc_context_t *)stack_top;

    /* R4 ~ R11 清零 */
    for(i = 0; i < 8; i++)
    {
        p_ctx->r4_r11[i] = 0;
    }

    /* FPU 高位寄存器 S16~S31 清零(S0~S15 由硬件自动入栈) */
    #if (SVCRT_FPU_USED == 1)
    for(i = 0; i < 16; i++)
    {
        p_ctx->sm[i] = 0;
    }
    #endif

    p_ctx->r0   = 0;
    p_ctx->r1   = 0;
    p_ctx->r2   = 0;
    p_ctx->r3   = 0;
    p_ctx->r12  = 0;
    p_ctx->lr   = 0;
    p_ctx->pc   = (uint32)entry;
    p_ctx->xpsr = 0x01000000;   /* Thumb 状态位必须为 1 */

    p_task->stack_ptr = stack_top;
}
