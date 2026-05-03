/**
* @brief 设备串口驱动模块
* @author chenjl
* @date 2019.07.03
*/

#ifndef __DRVUART_H__
#define __DRVUART_H__

#include "kerOs.h"
#include "kerfifo.h"
#include "devsio.h"

typedef enum {
    UART_DEV_COM1,
    UART_DEV_NUM
}UART_OBJECTS;

typedef struct {
    DEV_HDR hdr;

    KERFIFO_HEAD* txPipe;
    KERFIFO_HEAD* rxPipe;
    USART_TypeDef* usartAddr;
    uint8   opened;
    uint8   txidle;
}UART_DEV;

#define UART_PIPE_SIZE  (128)

extern DRV_INTERFACE usart_drv;

#endif
