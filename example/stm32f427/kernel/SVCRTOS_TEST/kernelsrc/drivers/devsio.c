/**
* @brief 板载设备注册
* @details 将内核内置驱动注册到统一设备表
*          此文件应根据实际硬件平台修改
* @author xw
* @date 2026.05.03
*/

#include "svcrt_dev.h"
#include "drvuart.h"
#include "drvled.h"

void svcrt_dev_board_init(void)
{
    svcrt_dev_register("COM1", &usart_drv, UART_DEV_COM1);
    svcrt_dev_register("LED",  &led_drv,  0);
}
