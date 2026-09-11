/**
* @brief SVCrtOS 环形缓冲区模块（内核内部）
* @details 提供内核和驱动使用的 FIFO 环形缓冲区
*          此文件仅供内核内部使用
*/

#ifndef __SVCRT_FIFO_H__
#define __SVCRT_FIFO_H__

#include "svcrt_def.h"

#define SVCRT_FIFO_MAGIC    (0xf1f0)

typedef struct {
    uint16 magic;
    uint16 wt_idx;
    uint16 rd_idx;
    uint16 size;
    uint8  data[1];
} svcrt_fifo_t;

svcrt_fifo_t *svcrt_fifo_create(uint8 *buff, int32 size);
int32 svcrt_fifo_write(svcrt_fifo_t *fifo, uint8 *pdata, int32 len);
int32 svcrt_fifo_read(svcrt_fifo_t *fifo, uint8 *pdata, int32 len);

#endif
