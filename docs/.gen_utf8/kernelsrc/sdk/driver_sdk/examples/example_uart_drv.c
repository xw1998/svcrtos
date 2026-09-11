/**
* @brief SVCrtOS Driver SDK - ???UART????
* @details ??о?????????????????????? svcrt_dev_drv_t ?????????????
*          ???????????????????λ??????????λ??????о? HAL/LL ?????ɡ?
*          ????????????2??????????????????á????????
*/

#include "svcrt_driver_sdk.h"

#define UART_DRV_MAX_INST   2
#define UART_RX_BUF_SIZE    64

typedef struct {
    svcrt_dev_hdr_t hdr;
    uint32  uart_id;                    /* ????????? */
    uint32  baudrate;                   /* ????????? */
    uint8   opened;                     /* ?????? */
    uint8   rx_buf[UART_RX_BUF_SIZE];   /* ??????壨??????ж???? */
    uint16  rx_head;
    uint16  rx_tail;
} uart_dev_obj_t;

static uart_dev_obj_t uart_objs[UART_DRV_MAX_INST];

static svcrt_dev_hdr_t *uart_drv_open(uint32 dev_id, uint32 param)
{
    uart_dev_obj_t *p;
    if(dev_id >= UART_DRV_MAX_INST)
        return 0;

    p = &uart_objs[dev_id];
    p->uart_id        = dev_id;
    p->baudrate       = param ? param : 115200;
    p->hdr.block_size = 1;
    p->rx_head        = 0;
    p->rx_tail        = 0;
    p->opened         = 1;

    /* TODO(???): ??????? UART ????
     * - ???? GPIO ????? TX/RX
     * - ???ò????? p->baudrate
     * - ??? UART ?? RX ?ж? */

    return (svcrt_dev_hdr_t *)p;
}

static int32 uart_drv_close(svcrt_dev_hdr_t *obj)
{
    uart_dev_obj_t *p = (uart_dev_obj_t *)obj;
    p->opened = 0;
    /* TODO(???): ??? UART ???衢?????ж? */
    return SVCRT_DRV_OK;
}

static int32 uart_drv_read(svcrt_dev_hdr_t *obj, uint8 *pdata, int32 len)
{
    uart_dev_obj_t *p = (uart_dev_obj_t *)obj;
    int32 cnt = 0;

    if(pdata == 0 || len <= 0)
        return SVCRT_DRV_INVALID_PARAM;
    if(!p->opened)
        return SVCRT_DRV_ERROR;

    /* ????????????λ?????????????? RX ?ж???? */
    while(cnt < len && p->rx_head != p->rx_tail)
    {
        pdata[cnt++] = p->rx_buf[p->rx_tail];
        p->rx_tail = (p->rx_tail + 1) % UART_RX_BUF_SIZE;
    }
    return cnt;
}

static int32 uart_drv_write(svcrt_dev_hdr_t *obj, uint8 *pdata, int32 len)
{
    uart_dev_obj_t *p = (uart_dev_obj_t *)obj;
    int32 cnt;

    if(pdata == 0 || len <= 0)
        return SVCRT_DRV_INVALID_PARAM;
    if(!p->opened)
        return SVCRT_DRV_ERROR;

    for(cnt = 0; cnt < len; cnt++)
    {
        /* TODO(???): ??????????????д?? pdata[cnt]
         * while(!(UARTx->SR & TXE)); UARTx->DR = pdata[cnt]; */
        (void)pdata;
    }
    return len;
}

static int32 uart_drv_ctrl(svcrt_dev_hdr_t *obj, uint32 code, uint32 value)
{
    uart_dev_obj_t *p = (uart_dev_obj_t *)obj;
    switch(code)
    {
    case SVCRT_DEV_CTRL_SET_BAUD:
        p->baudrate = value;
        /* TODO(???): ???????ò????? */
        return SVCRT_DRV_OK;
    case SVCRT_DEV_CTRL_GET_STATUS:
        return (int32)p->baudrate;
    case SVCRT_DEV_CTRL_RESET:
        p->rx_head = 0;
        p->rx_tail = 0;
        return SVCRT_DRV_OK;
    default:
        return SVCRT_DRV_ERROR;
    }
}

static svcrt_dev_drv_t uart_drv = {
    uart_drv_open,
    uart_drv_close,
    uart_drv_read,
    uart_drv_write,
    uart_drv_ctrl
};

int32 uart_drv_install(void)
{
    return svcrt_drv_register("COM1", &uart_drv, 0);
}
