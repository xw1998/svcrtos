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
 *
 * 栈布局(低 → 高):
 *   [stack_ptr] R4-R11 (8 word, 软件保存)
 *               R0,R1,R2,R3,R12,LR,PC,xPSR (8 word, 硬件帧)
 *   [stack_top]
 *
 * 注意:不再为 S16-S31 预留空间,因为 PendSV 不再手动保存 FPU
 *      上下文 (依赖硬件 LSPEN/ASPEN 自动状态保存)。
 * ============================================================ */
void svcrt_task_stack_init(svcrt_task_t *p_task, void (*entry)(void),
                           uint32 *stack_bottom, uint32 stack_size)
{
    uint32 *p_sp;
    uint32 stack_top = (uint32)stack_bottom + stack_size;

    /* 栈顶 8 字节对齐 (AAPCS 要求) */
    stack_top &= ~0x7u;

    p_task->stack_bottom = stack_bottom;
    p_task->stack_top    = stack_top;

    /* 硬件异常帧:8 word */
    p_sp = (uint32 *)stack_top;
    *(--p_sp) = 0x01000000;        /* xPSR  Thumb=1 必须 */
    *(--p_sp) = (uint32)entry;     /* PC    任务入口 */
    *(--p_sp) = 0;                 /* LR */
    *(--p_sp) = 0;                 /* R12 */
    *(--p_sp) = 0;                 /* R3 */
    *(--p_sp) = 0;                 /* R2 */
    *(--p_sp) = 0;                 /* R1 */
    *(--p_sp) = 0;                 /* R0 */

    /* 软件保存的 R4-R11:8 word */
    *(--p_sp) = 0;                 /* R11 */
    *(--p_sp) = 0;                 /* R10 */
    *(--p_sp) = 0;                 /* R9 */
    *(--p_sp) = 0;                 /* R8 */
    *(--p_sp) = 0;                 /* R7 */
    *(--p_sp) = 0;                 /* R6 */
    *(--p_sp) = 0;                 /* R5 */
    *(--p_sp) = 0;                 /* R4 */

    p_task->stack_ptr = (uint32)p_sp;
}
