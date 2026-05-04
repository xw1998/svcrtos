/**
* @brief SVCrtOS Driver SDK - Ê¾ÀýUARTÇý¶¯
*/

#include "svcrt_driver_sdk.h"

typedef struct {
    svcrt_dev_hdr_t hdr;
    uint32  uart_id;
    uint32  baudrate;
} uart_dev_obj_t;

static uart_dev_obj_t uart_dev_obj = {{0}, 0, 115200};

static svcrt_dev_hdr_t *uart_drv_open(uint32 dev_id, uint32 param)
{
    uart_dev_obj.uart_id = dev_id;
    uart_dev_obj.baudrate = param ? param : 115200;
    uart_dev_obj.hdr.block_size = 1;
    return (svcrt_dev_hdr_t *)&uart_dev_obj;
}

static int32 uart_drv_close(svcrt_dev_hdr_t *obj)
{
    return SVCRT_DRV_OK;
}

static int32 uart_drv_read(svcrt_dev_hdr_t *obj, uint8 *pdata, int32 len)
{
    return 0;
}

static int32 uart_drv_write(svcrt_dev_hdr_t *obj, uint8 *pdata, int32 len)
{
    return len;
}

static int32 uart_drv_ctrl(svcrt_dev_hdr_t *obj, uint32 code, uint32 value)
{
    uart_dev_obj_t *p = (uart_dev_obj_t *)obj;
    switch(code)
    {
    case SVCRT_DEV_CTRL_SET_BAUD:
        p->baudrate = value;
        return SVCRT_DRV_OK;
    case SVCRT_DEV_CTRL_GET_STATUS:
        return p->baudrate;
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
