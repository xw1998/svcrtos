/**
* @brief 设备串口驱动模块（HAL库版本，STM32F401 板）
* @details 本板把 COM1 映射到 USART2：TX = PA2、RX = PA3，均走 AF7。
* @details 使用 HAL 库初始化 UART，中断收发使用寄存器直操作
* @author xw
* @date 2026.05.03
*/

#include "drvuart.h"
#include "stm32f4xx_hal.h"

static void uart_start_first_tx(uart_dev_t *p_dev);

static uart_dev_t uart_dev_list[UART_DEV_NUM];
static uint8      uart_pipe_array[UART_DEV_NUM][2][UART_PIPE_SIZE];

static UART_HandleTypeDef uart_handle[UART_DEV_NUM];

svcrt_dev_hdr_t *uart_drv_open(uint32 name, uint32 baudrate)
{
    uart_dev_t *p_dev = 0;
    GPIO_InitTypeDef  GPIO_InitStr;

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
            p_dev->usart_addr = USART2;

            __HAL_RCC_GPIOA_CLK_ENABLE();
            __HAL_RCC_USART2_CLK_ENABLE();

            GPIO_InitStr.Mode      = GPIO_MODE_AF_PP;
            GPIO_InitStr.Pull      = GPIO_PULLUP;
            GPIO_InitStr.Speed     = GPIO_SPEED_FREQ_LOW;
            GPIO_InitStr.Alternate = GPIO_AF7_USART2;

            GPIO_InitStr.Pin = GPIO_PIN_2;
            HAL_GPIO_Init(GPIOA, &GPIO_InitStr);

            GPIO_InitStr.Pin   = GPIO_PIN_3;
            GPIO_InitStr.Pull  = GPIO_NOPULL;
            HAL_GPIO_Init(GPIOA, &GPIO_InitStr);

            uart_handle[name].Instance = USART2;
            uart_handle[name].Init.BaudRate = baudrate;
            uart_handle[name].Init.WordLength = UART_WORDLENGTH_8B;
            uart_handle[name].Init.StopBits = UART_STOPBITS_1;
            uart_handle[name].Init.Parity = UART_PARITY_NONE;
            uart_handle[name].Init.Mode = UART_MODE_TX_RX;
            uart_handle[name].Init.HwFlowCtl = UART_HWCONTROL_NONE;
            uart_handle[name].Init.OverSampling = UART_OVERSAMPLING_16;
            HAL_UART_Init(&uart_handle[name]);

                        /* Preemption priority must be numerically lower (that is more
             * urgent) than SVCall.  The SVC handler spins while waiting for
             * room in the transmit pipe, so whenever the UART interrupt can
             * not preempt that spin the pipe never drains and bytes are
             * dropped no matter how long the caller waits. */
            HAL_NVIC_SetPriority(USART2_IRQn, 0, 0);
            HAL_NVIC_EnableIRQ(USART2_IRQn);
            break;
        default:
            break;
    }

    __HAL_UART_ENABLE_IT(&uart_handle[name], UART_IT_RXNE);
    __HAL_UART_DISABLE_IT(&uart_handle[name], UART_IT_TXE);

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
    uint32 primask;
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
    if(len <= 0)
    {
        return len;
    }

    /* The transmit interrupt may now preempt an SVC handler that is
     * spinning on this very pipe, so the idle test and the kick have to
     * form one atomic step.  Otherwise the interrupt could run between
     * them, observe tx_idle == 0, and return to a caller that then arms
     * TXEIE on an empty pipe: TXEIE stays set while idle and the UART
     * interrupts in a storm. */
    primask = __get_PRIMASK();
    __disable_irq();
    if(p_dev->tx_idle != 0)
    {
        uart_start_first_tx(p_dev);
    }
    __set_PRIMASK(primask);

    return len;
}

static void uart_start_first_tx(uart_dev_t *p_dev)
{
    p_dev->tx_idle = 0;
    p_dev->usart_addr->CR1 |= USART_CR1_TXEIE;
}

int32 uart_drv_close(svcrt_dev_hdr_t *p)
{
    uart_dev_t *p_dev = (uart_dev_t *)p;
    if((p == 0) || (sizeof(uart_dev_t) != p->block_size))
    {
        return -1;
    }

    p_dev->opened = 0;
    p_dev->usart_addr->CR1 &= ~USART_CR1_UE;
    return 0;
}

int32 uart_drv_ctrl(svcrt_dev_hdr_t *p, uint32 c, uint32 v)
{
    return 0;
}

static void handle_usart_service(uart_dev_t *p_dev)
{
    uint8 d;
    uint32 sr = p_dev->usart_addr->SR;

    if(0 != (sr & (USART_SR_RXNE | USART_SR_ORE)))
    {
        d = (uint8)(p_dev->usart_addr->DR & 0xFF);
        svcrt_fifo_write(p_dev->rx_pipe, &d, 1);
    }

    /* TXE is served independently of RX.  With the old if/else a sustained
     * receive stream kept the transmit branch from ever running, so the
     * transmit pipe stayed full while the host was sending (image download)
     * and every writer above it spun until its budget ran out and dropped. */
    if(0 != (sr & USART_SR_TXE))
    {
        if(p_dev->tx_idle == 0)
        {
            if(svcrt_fifo_read(p_dev->tx_pipe, &d, 1) > 0)
            {
                p_dev->usart_addr->DR = d;
            }
            else
            {
                p_dev->usart_addr->CR1 &= ~USART_CR1_TXEIE;
                p_dev->tx_idle = 1;
                p_dev->usart_addr->CR1 |= USART_CR1_TCIE;
            }
        }
    }

    /* TC needs its own branch: when the line has drained TXE is set as well,
     * so testing TXE alone would never clear TCIE and the interrupt would
     * storm.  The tx_idle test keeps the original "idle only" semantics. */
    if(0 != (sr & USART_SR_TC))
    {
        if(p_dev->tx_idle != 0)
        {
            p_dev->usart_addr->CR1 &= ~USART_CR1_TCIE;
        }
    }
}

void USART2_IRQHandler(void)
{
    handle_usart_service(&uart_dev_list[UART_DEV_COM1]);
}

svcrt_dev_drv_t usart_drv =
{
    uart_drv_open,
    uart_drv_close,
    uart_drv_read,
    uart_drv_write,
    uart_drv_ctrl
};
