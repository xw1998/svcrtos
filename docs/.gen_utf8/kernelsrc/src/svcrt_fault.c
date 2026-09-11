/**
* @brief SVCrtOS 故障记录模块实现
* @details 固定容量环形缓冲保存最近 SVCRT_FAULT_RECORD_NUM 条故障记录，
*          记满后覆盖最旧记录。读写均在中断保护下进行，
*          可安全用于 HardFault 处理路径与 tick 上下文。
*/

#include "svcrt_fault.h"
#include "svcrt_hal.h"

static svcrt_fault_record_t svcrt_faults[SVCRT_FAULT_RECORD_NUM];
static int32 svcrt_fault_wr = 0;        /* 下一个写入位置 */
static int32 svcrt_fault_total = 0;     /* 累计记录数（用于区分空与满） */

void svcrt_fault_module_init(void)
{
    int32 i;
    for(i = 0; i < SVCRT_FAULT_RECORD_NUM; i++)
    {
        svcrt_faults[i].type    = 0;
        svcrt_faults[i].task_id = 0;
        svcrt_faults[i].tick    = 0;
    }
    svcrt_fault_wr = 0;
    svcrt_fault_total = 0;
}

void svcrt_fault_record(uint32 type, int32 task_id)
{
    SVCRT_DISABLE_IRQ();
    svcrt_faults[svcrt_fault_wr].type    = type;
    svcrt_faults[svcrt_fault_wr].task_id = task_id;
    svcrt_faults[svcrt_fault_wr].tick    = svcrt_kernel_tick;
    svcrt_fault_wr = (svcrt_fault_wr + 1) % SVCRT_FAULT_RECORD_NUM;
    if(svcrt_fault_total < SVCRT_FAULT_RECORD_NUM)
    {
        svcrt_fault_total++;
    }
    SVCRT_ENABLE_IRQ();
}

const svcrt_fault_record_t *svcrt_fault_record_get(int32 index)
{
    int32 oldest;
    int32 slot;

    if(index < 0 || index >= svcrt_fault_total)
        return 0;

    /* index=0 指向最旧记录；环形缓冲中最早位置取决于是否已写满 */
    if(svcrt_fault_total < SVCRT_FAULT_RECORD_NUM)
    {
        oldest = 0;
    }
    else
    {
        oldest = svcrt_fault_wr;        /* 写满时 wr 即最旧槽位 */
    }
    slot = (oldest + index) % SVCRT_FAULT_RECORD_NUM;
    return &svcrt_faults[slot];
}

int32 svcrt_fault_record_count_internal(void)
{
    return svcrt_fault_total;
}

int32 svcrt_fault_record_read_internal(int32 index, uint32 *out3)
{
    const svcrt_fault_record_t *p_rec;

    if(out3 == 0)
        return -1;

    p_rec = svcrt_fault_record_get(index);
    if(p_rec == 0)
        return -1;

    out3[0] = p_rec->type;
    out3[1] = (uint32)p_rec->task_id;
    out3[2] = p_rec->tick;
    return 0;
}
