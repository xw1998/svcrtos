/**
* @brief SVCrtOS Driver SDK - Ê¾ÀýUARTÇý¶¯
*/

#include "svcrt_driver_sdk.h"

typedef struct {
    DEV_HDR hdr;
    uint32  uart_id;
    uint32  baudrate;
} UART_DEV_OBJ;

static UART_DEV_OBJ uart_dev_obj = {0, 0, 115200};

static DEV_HDR* uart_drv_open(uint32 devid, uint32 param)
{
    uart_dev_obj.uart_id = devid;
    uart_dev_obj.baudrate = param ? param : 115200;
    uart_dev_obj.hdr.blocksize = 1;
    return (DEV_HDR*)&uart_dev_obj;
}

static int32 uart_drv_close(DEV_HDR *obj)
{
    return SVCRT_DRV_OK;
}

static int32 uart_drv_read(DEV_HDR *obj, uint8 *pdata, int32 len)
{
    return 0;
}

static int32 uart_drv_write(DEV_HDR *obj, uint8 *pdata, int32 len)
{
    return len;
}

static int32 uart_drv_ctrl(DEV_HDR *obj, uint32 code, uint32 value)
{
    UART_DEV_OBJ *p = (UART_DEV_OBJ*)obj;
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

static SVCRT_DRV_INTERFACE uart_drv = {
    uart_drv_open,
    uart_drv_close,
    uart_drv_read,
    uart_drv_write,
    uart_drv_ctrl
};

int32 uart_drv_install(void)
{
    return svcrtDrvRegister("COM1", &uart_drv, 0);
}
