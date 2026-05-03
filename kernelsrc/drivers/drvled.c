/**
* @brief 系统指示灯模块
* @author chenjl
* @date 2019.07.05
*/

#include "drvled.h"

static LED_DEV sysled_1;

DEV_HDR* Led_Open(uint32 id,uint32 p)
{
    GPIO_InitTypeDef  GPIO_InitStr;

    if(sysled_1.opened == 0)
    {
        sysled_1.opened = 1;
        sysled_1.ledon  = 1;

        RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOC,ENABLE);

        GPIO_InitStr.GPIO_Mode  = GPIO_Mode_OUT;
        GPIO_InitStr.GPIO_OType = GPIO_OType_PP;
        GPIO_InitStr.GPIO_Pin   = GPIO_Pin_0;
        GPIO_InitStr.GPIO_PuPd  = GPIO_PuPd_UP;
        GPIO_InitStr.GPIO_Speed = GPIO_Speed_2MHz;
        GPIO_Init(GPIOC,&GPIO_InitStr);

        GPIO_ResetBits(GPIOC,GPIO_Pin_0);

        sysled_1.hdr.blocksize = sizeof(LED_DEV);
    }

    return (DEV_HDR*)&sysled_1;
}

int32 Led_Read(DEV_HDR* p,uint8 *pData,int32 len)
{
    LED_DEV *pDev = (LED_DEV*)p;
    if((p==0) || (sizeof(LED_DEV) != p->blocksize))
    {
        return -1;
    }

    if(pDev->opened)
    {
        return pDev->ledon;
    }
    return -1;
}

int32 Led_Write(DEV_HDR* p,uint8 *pData,int32 len)
{
    LED_DEV *pDev = (LED_DEV*)p;
    if((p==0) || (sizeof(LED_DEV) != p->blocksize))
    {
        return -1;
    }

    if(pDev->opened)
    {
        if(len > 0)
        {
            pDev->ledon = 1;
        }
        else if(len == 0)
        {
            pDev->ledon = 0;
        }
        else
        {
            pDev->ledon = !pDev->ledon;
        }
        if(pDev->ledon == 0)
        {
            GPIO_SetBits(GPIOC,GPIO_Pin_0);
        }
        else
        {
            GPIO_ResetBits(GPIOC,GPIO_Pin_0);
        }
        return 0;
    }
    else
    {
        return -1;
    }
}

int32 Led_Ctrl(DEV_HDR* p,uint32 c,uint32 v)
{
    return 0;
}

DRV_INTERFACE led_drv =
{
    Led_Open,
    Led_Read,
    Led_Write,
    Led_Ctrl
};
