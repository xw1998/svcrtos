/**
* @brief fifo module
* @author chenjl
* @date 2019.07.02
*/

#include "kerfifo.h"

KERFIFO_HEAD* kerFifo_Create(uint8 *fifobuff,int32 size)
{
    KERFIFO_HEAD *fifo = (KERFIFO_HEAD*)fifobuff;
    if(size > 8)
    {
        fifo->magic = FIFI_MAGIC;
        fifo->rdidx = 0;
        fifo->wtidx = 0;
        fifo->size = size - 8;
    }
    else
    {
        fifo = 0;
    }
    return fifo;
}

int32 kerFifo_Write(KERFIFO_HEAD* fifo,uint8 *pdata,int32 len)
{
    int32 cpycnt;
    if(fifo->magic != FIFI_MAGIC)
    {
        return 0;
    }
    for(cpycnt = 0; cpycnt < len; cpycnt++)
    {
        fifo->data[fifo->wtidx] = pdata[cpycnt];
        fifo->wtidx = (fifo->wtidx + 1) % fifo->size;
    }
    return cpycnt;
}

int32 kerFifo_Read(KERFIFO_HEAD* fifo,uint8 *pdata,int32 len)
{
    int32 cpycnt;
    if(fifo->magic != FIFI_MAGIC)
    {
        return 0;
    }
    for(cpycnt = 0; cpycnt < len; cpycnt++)
    {
        if(fifo->rdidx != fifo->wtidx)
        {
            pdata[cpycnt] = fifo->data[fifo->rdidx];
            fifo->rdidx = (fifo->rdidx + 1) % fifo->size;
        }
        else
        {
            break;
        }
    }
    return cpycnt;
}
