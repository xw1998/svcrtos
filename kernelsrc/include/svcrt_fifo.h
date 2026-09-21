/**
* @brief SVCrtOS 环形缓冲区模块（内核内部）
* @details 提供内核和驱动使用的 FIFO 环形缓冲区
*          此文件仅供内核内部使用
*/

#ifndef __SVCRT_FIFO_H__
#define __SVCRT_FIFO_H__

#include "svcrt_def.h"

#define SVCRT_FIFO_MAGIC    (0xf1f0)

/* @note 并发约束：本 FIFO 采用单生产者-单消费者模型。
 *       wt_idx / rd_idx 会被 ISR 与任务两侧同时访问，因此必须是 volatile，
 *       否则编译器可能在循环中把它们缓存在寄存器里，
 *       导致读到过期值、或把更新延后写回（表现为丢数据/提前判空判满）。
 *       若确实需要多写者或多读者，必须在调用方自行加临界区。 */
typedef struct {
    uint16          magic;
    volatile uint16 wt_idx;     /* 写索引：由生产者更新，消费者读取 */
    volatile uint16 rd_idx;     /* 读索引：由消费者更新，生产者读取 */
    uint16          size;       /* 数据区字节数（不含头部 8 字节） */
    uint8           data[1];
} svcrt_fifo_t;

svcrt_fifo_t *svcrt_fifo_create(uint8 *buff, int32 size);
int32 svcrt_fifo_write(svcrt_fifo_t *fifo, uint8 *pdata, int32 len);
int32 svcrt_fifo_read(svcrt_fifo_t *fifo, uint8 *pdata, int32 len);

/* @return bytes refused because the FIFO was full (all FIFOs, both directions) */
uint32 svcrt_fifo_write_refused(void);

/* @return calls that found a FIFO header unusable (memory damage, not
 *         flow control); non-zero means silent loss the full-count
 *         cannot show. */
uint32 svcrt_fifo_bad_magic(void);
uint32 svcrt_fifo_bad_magic_bytes(void);

#endif
