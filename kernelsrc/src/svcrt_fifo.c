/**
* @brief SVCrtOS 环形缓冲区（FIFO）实现
* @details 提供用于内核与驱动间数据传输的 FIFO 环形缓冲区。
* @author xw
* @date 2026.05.03
*/

#include "svcrt_fifo.h"

/* A FIFO whose header magic no longer matches is not flow control but
 * memory damage: every byte handed to it is dropped, and because read()
 * returns a short count either way, the caller cannot tell "the peer
 * said nothing" from "my buffer is gone".  That is how a console with
 * a clobbered receive FIFO looked dead for good while the task kept
 * running and kept transmitting.  Read it, do not infer it. */
static volatile uint32 g_fifo_bad_magic = 0u;
static volatile uint32 g_fifo_bad_magic_bytes = 0u;

/* Calls that found the FIFO header unusable (all FIFOs). */
uint32 svcrt_fifo_bad_magic(void)
{
    return g_fifo_bad_magic;
}

/* Bytes lost through such a FIFO.  Non-zero means silent data loss of a
 * kind no flow-control counter can show: the buffer itself is broken. */
uint32 svcrt_fifo_bad_magic_bytes(void)
{
    return g_fifo_bad_magic_bytes;
}


/* Count one call that could not use its FIFO, and the bytes it lost. */
static void svcrt_fifo_note_bad(svcrt_fifo_t *fifo, int32 len)
{
    (void)fifo;
    g_fifo_bad_magic++;
    if(len > 0)
    {
        g_fifo_bad_magic_bytes += (uint32)len;
    }
}

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
/* Bytes a writer had to leave behind because the FIFO was full.  A short
 * count is legal for this API, so without a counter the loss is invisible
 * from the outside - which is how a shredded console line got read as a
 * transport problem instead of an overflow.  Read it, do not infer it. */
static volatile uint32 g_fifo_write_refused = 0u;

/* Refused byte count over all FIFOs (transmit and receive). */
uint32 svcrt_fifo_write_refused(void)
{
    return g_fifo_write_refused;
}

int32 svcrt_fifo_write(svcrt_fifo_t *fifo, uint8 *pdata, int32 len)
{
    int32 cnt;
    uint16 next;
    if((fifo == 0) || (fifo->magic != SVCRT_FIFO_MAGIC))
    {
        svcrt_fifo_note_bad(fifo, len);
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
    if(cnt < len)
    {
        g_fifo_write_refused += (uint32)(len - cnt);
    }
    return cnt;
}

/* @return 实际读出的字节数（FIFO 空时可能小于 len）。
 * @note   同 svcrt_fifo_write：单消费者模型，不内建临界区。 */
int32 svcrt_fifo_read(svcrt_fifo_t *fifo, uint8 *pdata, int32 len)
{
    int32 cnt;
    if((fifo == 0) || (fifo->magic != SVCRT_FIFO_MAGIC))
    {
        svcrt_fifo_note_bad(fifo, 0);
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
