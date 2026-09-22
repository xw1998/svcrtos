/**
* @brief 设备串口驱动模块
* @author xw
* @date 2026.05.03
*/

#ifndef __DRV_UART_H__
#define __DRV_UART_H__

#include "svcrt_def.h"
#include "svcrt_fifo.h"
#include "svcrt_dev.h"
#include "stm32f4xx.h"

typedef enum {
    UART_DEV_COM1,
    UART_DEV_NUM
} uart_object_t;

typedef struct {
    svcrt_dev_hdr_t hdr;

    svcrt_fifo_t *tx_pipe;
    svcrt_fifo_t *rx_pipe;
    USART_TypeDef *usart_addr;
    /* ISR shared: tx_idle is written by USART1_IRQHandler and read by */
    /* uart_drv_write, so it must be volatile or the task side can act on */
    /* a stale value, skip uart_start_first_tx() and leave the pipe full */
    /* with TXEIE disabled - the transmit pipe then never drains and every */
    /* writer above it spins and drops bytes. */
    volatile uint8 opened;
    volatile uint8 tx_idle;
} uart_dev_t;

/* 1024, not 128: the loader moves 512 B blocks and fs_put 256 B
 * blocks, and a pipe smaller than a block makes svcrt_fifo_write
 * spin and drop bytes (measured fifo_full_retries=197639).  One
 * byte on the wire takes ~87 us at 115200 8N1, so the task side has
 * time to drain before the next block lands. */
#define UART_PIPE_SIZE  (1024)

extern svcrt_dev_drv_t usart_drv;

#endif
