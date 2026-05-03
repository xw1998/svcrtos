/**
* @brief 设备串口驱动模块
* @author chenjl
* @date 2019.07.03
*/

#include "drvuart.h"


static void Uart_StartFirstTx(UART_DEV *pDev);

static UART_DEV uart_dev_list[UART_DEV_NUM];
static uint8    uart_pipe_arry[UART_DEV_NUM][2][UART_PIPE_SIZE];

DEV_HDR* Uart_Open(uint32 name,uint32 baudrate)
{
    UART_DEV* pDev = 0;
    GPIO_InitTypeDef  GPIO_InitStr;
    USART_InitTypeDef USART_InitStr;
    NVIC_InitTypeDef  NVIC_InitStr;

    if(name >= UART_DEV_NUM)
    {
        return 0;
    }
    else
    {
        pDev = &uart_dev_list[name];
    }
    if(pDev->opened != 0)
    {
        return (DEV_HDR*)pDev;
    }
    pDev->opened = 1;
    pDev->txidle = 1;
    pDev->txPipe = kerFifo_Create(uart_pipe_arry[name][0],UART_PIPE_SIZE);
    pDev->rxPipe = kerFifo_Create(uart_pipe_arry[name][1],UART_PIPE_SIZE);

    switch(name)
    {
        case UART_DEV_COM1:
            pDev->usartAddr = USART1;

            RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOA,ENABLE);
            RCC_APB2PeriphClockCmd(RCC_APB2Periph_USART1,ENABLE);

            GPIO_InitStr.GPIO_Mode  = GPIO_Mode_AF;
            GPIO_InitStr.GPIO_OType = GPIO_OType_PP;
            GPIO_InitStr.GPIO_Pin   = GPIO_Pin_9;
            GPIO_InitStr.GPIO_PuPd  = GPIO_PuPd_UP;
            GPIO_InitStr.GPIO_Speed = GPIO_Speed_2MHz;
            GPIO_Init(GPIOA,&GPIO_InitStr);
            GPIO_PinAFConfig(GPIOA,GPIO_PinSource9,GPIO_AF_USART1);

            GPIO_InitStr.GPIO_Pin   = GPIO_Pin_10;
            GPIO_InitStr.GPIO_OType = GPIO_OType_OD;
            GPIO_InitStr.GPIO_PuPd  = GPIO_PuPd_NOPULL;
            GPIO_Init(GPIOA,&GPIO_InitStr);
            GPIO_PinAFConfig(GPIOA,GPIO_PinSource10,GPIO_AF_USART1);

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
    USART_InitStr.USART_WordLength= USART_WordLength_8b;

    USART_Init(pDev->usartAddr,&USART_InitStr);

    NVIC_InitStr.NVIC_IRQChannelCmd = ENABLE;
    NVIC_InitStr.NVIC_IRQChannelPreemptionPriority = 10;
    NVIC_InitStr.NVIC_IRQChannelSubPriority  = 10;
    NVIC_Init(&NVIC_InitStr);
    USART_ITConfig(pDev->usartAddr,USART_IT_RXNE,ENABLE);
    USART_ITConfig(pDev->usartAddr,USART_IT_TXE,DISABLE);

    USART_Cmd(pDev->usartAddr,ENABLE);
    pDev->hdr.blocksize = sizeof(UART_DEV);
    return (DEV_HDR*)pDev;
}

int32 Uart_Read(DEV_HDR* p,uint8 *pData,int32 len)
{
    UART_DEV *pDev = (UART_DEV *)p;
    if((p==0) || (sizeof(UART_DEV) != p->blocksize))
    {
        return -1;
    }
    return kerFifo_Read(pDev->rxPipe,pData,len);
}

int32 Uart_Write(DEV_HDR *p,uint8 *pData,int32 len)
{
    UART_DEV *pDev = (UART_DEV *)p;
    if((p == 0) || (sizeof(UART_DEV) != p->blocksize))
    {
        return -1;
    }

    if(pDev->opened == 0)
    {
        return 0;
    }
    len = kerFifo_Write(pDev->txPipe,pData,len);
    if(pDev->txidle != 0)
    {
        Uart_StartFirstTx(pDev);
    }
    return len;
}

static void Uart_StartFirstTx(UART_DEV *pDev)
{
    pDev->txidle = 0;
    USART_ITConfig(pDev->usartAddr,USART_IT_TXE,ENABLE);
}

int32 Uart_Ctrl(DEV_HDR* p,uint32 c,uint32 v)
{
    return 0;
}

static void Handle_UsartService(UART_DEV *pDev)
{
    uint8 d;
    uint16 sr = pDev->usartAddr->SR;

    if(0 != (sr & (USART_FLAG_RXNE | USART_FLAG_ORE)))
    {
        d = pDev->usartAddr->DR;
        kerFifo_Write(pDev->rxPipe,&d,1);
    }
    else
    {
        if(pDev->txidle == 0)
        {
            if(kerFifo_Read(pDev->txPipe,&d,1) > 0)
            {
                pDev->usartAddr->DR = d;
            }
            else
            {
                USART_ITConfig(pDev->usartAddr,USART_IT_TXE,DISABLE);
                pDev->txidle = 1;
                USART_ITConfig(pDev->usartAddr,USART_IT_TC,ENABLE);
            }
        }
        else
        {
            USART_ITConfig(pDev->usartAddr,USART_IT_TC,DISABLE);
        }

    }
}

void USART1_IRQHandler(void)
{
    Handle_UsartService(&uart_dev_list[UART_DEV_COM1]);
}

DRV_INTERFACE usart_drv =
{
    Uart_Open,
    Uart_Read,
    Uart_Write,
    Uart_Ctrl
};
