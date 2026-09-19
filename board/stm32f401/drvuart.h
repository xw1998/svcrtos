/**
* @brief 设备串口驱动模块（STM32F401 板：COM1 = USART2，PA2=TX / PA3=RX）
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

#define UART_PIPE_SIZE  (128)

extern svcrt_dev_drv_t usart_drv;

#endif
