/**
* @brief fifo module
* @author chenjl
* @date 2019.07.02
*/

#ifndef __KERFIFO_H__
#define __KERFIFO_H__

#include "kerOs.h"

#define FIFI_MAGIC      (0xf1f0)

typedef struct {
    uint16 magic;
    uint16 wtidx;
    uint16 rdidx;
    uint16 size;

    uint8  data[1];
}KERFIFO_HEAD;

KERFIFO_HEAD* kerFifo_Create(uint8 *fifobuff,int32 size);
int32 kerFifo_Write(KERFIFO_HEAD* fifo,uint8 *pdata,int32 len);
int32 kerFifo_Read(KERFIFO_HEAD* fifo,uint8 *pdata,int32 len);

#endif
