/**
* @brief SVCrtOS 环形缓冲区（FIFO）实现
* @details 提供用于内核与驱动间数据传输的 FIFO 环形缓冲区。
* @author xw
* @date 2026.05.03
*/

#include "svcrt_fifo.h"

svcrt_fifo_t *svcrt_fifo_create(uint8 *buff, int32 size)
{
    svcrt_fifo_t *fifo = (svcrt_fifo_t *)buff;
    if(size > 8)
    {
        fifo->magic = SVCRT_FIFO_MAGIC;
        fifo->rd_idx = 0;
        fifo->wt_idx = 0;
        fifo->size = size - 8;
    }
    else
    {
        fifo = 0;
    }
    return fifo;
}

/* @return 实际写入的字节数（FIFO 满时可能小于 len）。
 * @note   本函数不做临界区保护，按单生产者-单消费者模型使用：
 *         要么只在任务里调用，要么生产者固定为 ISR；混用时由调用方加锁。 */
int32 svcrt_fifo_write(svcrt_fifo_t *fifo, uint8 *pdata, int32 len)
{
    int32 cnt;
    uint16 next;
    if(fifo->magic != SVCRT_FIFO_MAGIC)
    {
        return 0;
    }
    for(cnt = 0; cnt < len; cnt++)
    {
        next = (fifo->wt_idx + 1) % fifo->size;
        if(next == fifo->rd_idx)
        {
            break;          /* FIFO 已满，停止写入以避免覆盖尚未读取的数据 */
        }
        fifo->data[fifo->wt_idx] = pdata[cnt];
        fifo->wt_idx = next;
    }
    return cnt;
}

/* @return 实际读出的字节数（FIFO 空时可能小于 len）。
 * @note   同 svcrt_fifo_write：单消费者模型，不内建临界区。 */
int32 svcrt_fifo_read(svcrt_fifo_t *fifo, uint8 *pdata, int32 len)
{
    int32 cnt;
    if(fifo->magic != SVCRT_FIFO_MAGIC)
    {
        return 0;
    }
    for(cnt = 0; cnt < len; cnt++)
    {
        if(fifo->rd_idx != fifo->wt_idx)
        {
            pdata[cnt] = fifo->data[fifo->rd_idx];
            fifo->rd_idx = (fifo->rd_idx + 1) % fifo->size;
        }
        else
        {
            break;
        }
    }
    return cnt;
}
