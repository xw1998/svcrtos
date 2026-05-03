/**
* @brief 设备接口模块
* @author chenjl
* @date 2019.07.05
*/

#include "devsio.h"
#include "drvuart.h"
#include "drvled.h"
#include "string.h"

#define DEVS_NUM    (2)

static DEV_DESCRIPT dev_list[DEVS_NUM] =
{
    {"COM1",&usart_drv,UART_DEV_COM1},
    {"LED",&led_drv,0},
};

static DEV_HDR* dev_handles[DEVS_NUM];

int32 kerDevOpen(char *name,uint32 param)
{
    int16 i;
    for(i=0;i<DEVS_NUM;i++)
    {
        if(strcmp(name,dev_list[i].dev_name) == 0)
        {
            dev_handles[i] = dev_list[i].drv->DrvOpen(
                dev_list[i].dev_num,param);
            if(dev_handles[i] != 0)
            {
                return DEV_HANDLE_FLAG | i;
            }
            else
            {
                return -1;
            }
        }
    }
    return -1;
}

int32 kerDevRead(int32 handle,uint8 *pData,int32 len)
{
    int32 ridx = handle & HANDLE_RELMASK;

    if(DEV_HANDLE_FLAG != (handle & HANDLE_MASK))
    {
        return 0;
    }
    return dev_list[ridx].drv->DrvRead(dev_handles[ridx],pData,len);
}

int32 kerDevWrite(int32 handle,uint8 *pData,int32 len)
{
    int32 ridx = handle & HANDLE_RELMASK;

    if(DEV_HANDLE_FLAG != (handle & HANDLE_MASK))
    {
        return 0;
    }
    return dev_list[ridx].drv->DrvWrite(dev_handles[ridx],pData,len);
}

int32 kerDevCtrl(int32 handle,uint32 code,uint32 value)
{
    int32 ridx = handle & HANDLE_RELMASK;

    if(DEV_HANDLE_FLAG != (handle & HANDLE_MASK))
    {
        return 0;
    }
    return dev_list[ridx].drv->DrvCtrl(dev_handles[ridx],code,value);
}
