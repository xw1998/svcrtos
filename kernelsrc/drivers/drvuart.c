/**
* @brief 设备串口驱动模块
* @author xw
* @date 2026.05.03
*/

#include "drv_uart.h"

static void uart_start_first_tx(uart_dev_t *p_dev);

static uart_dev_t uart_dev_list[UART_DEV_NUM];
static uint8      uart_pipe_array[UART_DEV_NUM][2][UART_PIPE_SIZE];

svcrt_dev_hdr_t *uart_drv_open(uint32 name, uint32 baudrate)
{
    uart_dev_t *p_dev = 0;
    GPIO_InitTypeDef  GPIO_InitStr;
    USART_InitTypeDef USART_InitStr;
    NVIC_InitTypeDef  NVIC_InitStr;

    if(name >= UART_DEV_NUM)
    {
        return 0;
    }
    else
    {
        p_dev = &uart_dev_list[name];
    }

    if(p_dev->opened != 0)
    {
        return (svcrt_dev_hdr_t *)p_dev;
    }

    p_dev->opened = 1;
    p_dev->tx_idle = 1;
    p_dev->tx_pipe = svcrt_fifo_create(uart_pipe_array[name][0], UART_PIPE_SIZE);
    p_dev->rx_pipe = svcrt_fifo_create(uart_pipe_array[name][1], UART_PIPE_SIZE);

    switch(name)
    {
        case UART_DEV_COM1:
            p_dev->usart_addr = USART1;

            RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOA, ENABLE);
            RCC_APB2PeriphClockCmd(RCC_APB2Periph_USART1, ENABLE);

            GPIO_InitStr.GPIO_Mode  = GPIO_Mode_AF;
            GPIO_InitStr.GPIO_OType = GPIO_OType_PP;
            GPIO_InitStr.GPIO_Pin   = GPIO_Pin_9;
            GPIO_InitStr.GPIO_PuPd  = GPIO_PuPd_UP;
            GPIO_InitStr.GPIO_Speed = GPIO_Speed_2MHz;
            GPIO_Init(GPIOA, &GPIO_InitStr);
            GPIO_PinAFConfig(GPIOA, GPIO_PinSource9, GPIO_AF_USART1);

            GPIO_InitStr.GPIO_Pin   = GPIO_Pin_10;
            GPIO_InitStr.GPIO_OType = GPIO_OType_OD;
            GPIO_InitStr.GPIO_PuPd  = GPIO_PuPd_NOPULL;
            GPIO_Init(GPIOA, &GPIO_InitStr);
            GPIO_PinAFConfig(GPIOA, GPIO_PinSource10, GPIO_AF_USART1);

            USART_InitStr.USART_BaudRate = baudrate;
            NVIC_InitStr.NVIC_IRQChannel = USART1_IRQn;
            break;
        default:
            break;
    }

    USART_InitStr.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    USART_InitStr.USART_Mode     = USART_Mode_Rx | USART_Mode_Tx;
    USART_InitStr.USART_Parity   = USART_Parity_No;
    USART_InitStr.USART_StopBits = USART_StopBits_1;
    USART_InitStr.USART_WordLength = USART_WordLength_8b;

    USART_Init(p_dev->usart_addr, &USART_InitStr);

    NVIC_InitStr.NVIC_IRQChannelCmd = ENABLE;
    NVIC_InitStr.NVIC_IRQChannelPreemptionPriority = 10;
    NVIC_InitStr.NVIC_IRQChannelSubPriority  = 10;
    NVIC_Init(&NVIC_InitStr);
    USART_ITConfig(p_dev->usart_addr, USART_IT_RXNE, ENABLE);
    USART_ITConfig(p_dev->usart_addr, USART_IT_TXE, DISABLE);

    USART_Cmd(p_dev->usart_addr, ENABLE);
    p_dev->hdr.block_size = sizeof(uart_dev_t);
    return (svcrt_dev_hdr_t *)p_dev;
}

int32 uart_drv_read(svcrt_dev_hdr_t *p, uint8 *pdata, int32 len)
{
    uart_dev_t *p_dev = (uart_dev_t *)p;
    if((p == 0) || (sizeof(uart_dev_t) != p->block_size))
    {
        return -1;
    }
    return svcrt_fifo_read(p_dev->rx_pipe, pdata, len);
}

int32 uart_drv_write(svcrt_dev_hdr_t *p, uint8 *pdata, int32 len)
{
    uart_dev_t *p_dev = (uart_dev_t *)p;
    if((p == 0) || (sizeof(uart_dev_t) != p->block_size))
    {
        return -1;
    }

    if(p_dev->opened == 0)
    {
        return 0;
    }
    len = svcrt_fifo_write(p_dev->tx_pipe, pdata, len);
    if(p_dev->tx_idle != 0)
    {
        uart_start_first_tx(p_dev);
    }
    return len;
}

static void uart_start_first_tx(uart_dev_t *p_dev)
{
    p_dev->tx_idle = 0;
    USART_ITConfig(p_dev->usart_addr, USART_IT_TXE, ENABLE);
}

int32 uart_drv_ctrl(svcrt_dev_hdr_t *p, uint32 c, uint32 v)
{
    return 0;
}

static void handle_usart_service(uart_dev_t *p_dev)
{
    uint8 d;
    uint16 sr = p_dev->usart_addr->SR;

    if(0 != (sr & (USART_FLAG_RXNE | USART_FLAG_ORE)))
    {
        d = p_dev->usart_addr->DR;
        svcrt_fifo_write(p_dev->rx_pipe, &d, 1);
    }
    else
    {
        if(p_dev->tx_idle == 0)
        {
            if(svcrt_fifo_read(p_dev->tx_pipe, &d, 1) > 0)
            {
                p_dev->usart_addr->DR = d;
            }
            else
            {
                USART_ITConfig(p_dev->usart_addr, USART_IT_TXE, DISABLE);
                p_dev->tx_idle = 1;
                USART_ITConfig(p_dev->usart_addr, USART_IT_TC, ENABLE);
            }
        }
        else
        {
            USART_ITConfig(p_dev->usart_addr, USART_IT_TC, DISABLE);
        }
    }
}

void USART1_IRQHandler(void)
{
    handle_usart_service(&uart_dev_list[UART_DEV_COM1]);
}

svcrt_dev_drv_t usart_drv =
{
    uart_drv_open,
    uart_drv_read,
    uart_drv_write,
    uart_drv_ctrl
};
