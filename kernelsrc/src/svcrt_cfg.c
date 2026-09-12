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
#include "svcrt_fault.h"
#include "svcrt_init.h"

svcrt_task_t svcrt_task_table[SVCRT_TASK_MAX_NUM];
int32 svcrt_task_count = 0;

/* 任务表 RAM 预算守护：把 SVCRT_TASK_MAX_NUM 调得过大时直接编译报错，
 * 避免静态 TCB 表悄悄吃掉内核 RAM。预算见 svcrt_config.h 的
 * SVCRT_TASK_TABLE_RAM_MAX。 */
typedef char svcrt_task_table_ram_check[
    (sizeof(svcrt_task_table) <= (SVCRT_TASK_TABLE_RAM_MAX)) ? 1 : -1];

void svcrt_cfg_load(void)
{
    svcrt_task_count = 0;
}

/* 注册任务的通用辅助：填充 TCB 缺省字段、初始化栈帧并加入任务表。
 * 供应用 main 与内核内置任务（定时器服务）共用。返回 1 起始任务号，失败返回 -1。 */
int32 svcrt_task_register(void (*entry)(void), uint32 *stack_bottom, uint32 stack_size,
                          uint8 priority, uint32 period_ms)
{
    svcrt_task_t *p_task;

    /* 注册失败一律记录故障，不再静默返回：
     * 任务没跑起来时能通过 svcrt_fault_record_read() 看到原因 */
    if(svcrt_task_count >= SVCRT_TASK_MAX_NUM)
    {
        svcrt_fault_record(SVCRT_FAULT_NOSLOT, 0);
        return -1;
    }

    if((entry == 0) || (stack_bottom == 0) || (stack_size < 64u))
    {
        svcrt_fault_record(SVCRT_FAULT_NOSLOT, 0);
        return -1;
    }

    stack_bottom[0] = SVCRT_STACK_END_FLAG_VAL;

    p_task = &svcrt_task_table[svcrt_task_count];
    p_task->ram_start   = (uint32)stack_bottom;
    p_task->ram_size    = stack_size;
    p_task->stack_size  = stack_size;
    p_task->rom_start   = 0;
    p_task->rom_size    = 0;
    p_task->period      = SVCRT_MS_TO_TICK(period_ms);
    p_task->priority      = priority;
    p_task->base_priority = priority;   /* 基准优先级：撤销优先级继承时的恢复依据 */
    p_task->shm_attri   = 0;
    p_task->status      = SVCRT_TASK_READY;
    p_task->period_time = p_task->period;
    p_task->wait_time   = 0;
    p_task->tim_tick    = 0;
    p_task->touch_tick  = 0;
    p_task->wake_reason = 0;
    p_task->recover_pending = 0;

    #if (SVCRT_USE_MPU == 1)
    {
        int32 i;
        for(i = 0; i < SVCRT_MPU_REGION_MAX; i++)
        {
            p_task->mpu.region_base[i] = 0;
            p_task->mpu.region_attr[i] = 0;
        }
    }
    #endif

    svcrt_task_stack_init(p_task, entry, stack_bottom, stack_size);

    svcrt_task_count++;
    return svcrt_task_count;
}

#if (SVCRT_USE_STACK_USAGE == 1)
/**
* @brief 用填充图案覆盖整段任务栈（跳过下标 0 的栈底保护字）
* @param stack_bottom 栈底地址
* @param stack_size   栈大小（字节）
* @details 任务运行过程中会不断覆盖该图案，查询栈用量时扫描仍保持图案的
*          连续区域即可得到“从未使用过”的栈空间，换算后即为峰值用量。
*/
void svcrt_task_stack_fill(uint32 *stack_bottom, uint32 stack_size)
{
    uint32 words = stack_size / 4u;
    uint32 i;

    for(i = 1u; i < words; i++)
    {
        stack_bottom[i] = SVCRT_STACK_FILL_PATTERN;
    }
}
#endif


void svcrt_task_stack_init(svcrt_task_t *p_task, void (*entry)(void),
                           uint32 *stack_bottom, uint32 stack_size)
{
    uint32 stack_top = (uint32)stack_bottom + stack_size;

    stack_top &= ~0x7u;

    #if (SVCRT_USE_STACK_USAGE == 1)
    /* 先填充整段栈，再建立初始栈帧，便于后续统计峰值栈用量 */
    svcrt_task_stack_fill(stack_bottom, stack_size);
    #endif

    p_task->stack_bottom = stack_bottom;
    p_task->stack_top    = stack_top;

    #if (SVCRT_USE_STACK_USAGE == 1)
    p_task->stack_peak_low = stack_top;     /* 峰值统计基准：初始未使用 */
    #endif


    p_task->entry     = entry;
    p_task->stack_ptr = svcrt_port_stack_init(stack_top, entry);
}
