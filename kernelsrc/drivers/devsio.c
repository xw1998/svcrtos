/**
* @brief 板级设备初始化
* @details 注册板载设备到内核驱动框架
*          此文件应根据实际硬件平台修改
*/

#include "svcrt_dev.h"
#include "drv_uart.h"
#include "drv_led.h"
#include "string.h"

#define BOARD_DEV_NUM    (2)

static svcrt_dev_desc_t board_dev_list[BOARD_DEV_NUM] =
{
    {"COM1", &usart_drv, UART_DEV_COM1},
    {"LED",  &led_drv,  0},
};

static svcrt_dev_hdr_t *board_dev_handles[BOARD_DEV_NUM];

int32 svcrt_dev_open_internal(char *name, uint32 param)
{
    int16 i;
    for(i = 0; i < BOARD_DEV_NUM; i++)
    {
        if(strcmp(name, board_dev_list[i].dev_name) == 0)
        {
            board_dev_handles[i] = board_dev_list[i].drv->drv_open(
                board_dev_list[i].dev_num, param);
            if(board_dev_handles[i] != 0)
            {
                return SVCRT_DEV_HANDLE_FLAG | i;
            }
            else
            {
                return -1;
            }
        }
    }
    return -1;
}

int32 svcrt_dev_read_internal(int32 handle, uint8 *pdata, int32 len)
{
    int32 ridx = handle & SVCRT_HANDLE_RELMASK;

    if(SVCRT_DEV_HANDLE_FLAG != (handle & SVCRT_HANDLE_MASK))
    {
        return 0;
    }
    return board_dev_list[ridx].drv->drv_read(board_dev_handles[ridx], pdata, len);
}

int32 svcrt_dev_write_internal(int32 handle, uint8 *pdata, int32 len)
{
    int32 ridx = handle & SVCRT_HANDLE_RELMASK;

    if(SVCRT_DEV_HANDLE_FLAG != (handle & SVCRT_HANDLE_MASK))
    {
        return 0;
    }
    return board_dev_list[ridx].drv->drv_write(board_dev_handles[ridx], pdata, len);
}

int32 svcrt_dev_ctrl_internal(int32 handle, uint32 code, uint32 value)
{
    int32 ridx = handle & SVCRT_HANDLE_RELMASK;

    if(SVCRT_DEV_HANDLE_FLAG != (handle & SVCRT_HANDLE_MASK))
    {
        return 0;
    }
    return board_dev_list[ridx].drv->drv_ctrl(board_dev_handles[ridx], code, value);
}
