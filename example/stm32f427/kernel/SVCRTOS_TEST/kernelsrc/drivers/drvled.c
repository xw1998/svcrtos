/**
* @brief 系统指示灯模块
* @author xw
* @date 2026.05.03
*/

#include "drvled.h"

static led_dev_t sysled_1;

svcrt_dev_hdr_t *led_drv_open(uint32 id, uint32 p)
{
    GPIO_InitTypeDef  GPIO_InitStr;

    if(sysled_1.opened == 0)
    {
        sysled_1.opened = 1;
        sysled_1.led_on = 1;

        RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOC, ENABLE);

        GPIO_InitStr.GPIO_Mode  = GPIO_Mode_OUT;
        GPIO_InitStr.GPIO_OType = GPIO_OType_PP;
        GPIO_InitStr.GPIO_Pin   = GPIO_Pin_0;
        GPIO_InitStr.GPIO_PuPd  = GPIO_PuPd_UP;
        GPIO_InitStr.GPIO_Speed = GPIO_Speed_2MHz;
        GPIO_Init(GPIOC, &GPIO_InitStr);

        GPIO_ResetBits(GPIOC, GPIO_Pin_0);

        sysled_1.hdr.block_size = sizeof(led_dev_t);
    }

    return (svcrt_dev_hdr_t *)&sysled_1;
}

int32 led_drv_read(svcrt_dev_hdr_t *p, uint8 *pdata, int32 len)
{
    led_dev_t *p_dev = (led_dev_t *)p;
    if((p == 0) || (sizeof(led_dev_t) != p->block_size))
    {
        return -1;
    }

    if(p_dev->opened)
    {
        return p_dev->led_on;
    }
    return -1;
}

int32 led_drv_write(svcrt_dev_hdr_t *p, uint8 *pdata, int32 len)
{
    led_dev_t *p_dev = (led_dev_t *)p;
    if((p == 0) || (sizeof(led_dev_t) != p->block_size))
    {
        return -1;
    }

    if(p_dev->opened)
    {
        if(len > 0)
        {
            p_dev->led_on = 1;
        }
        else if(len == 0)
        {
            p_dev->led_on = 0;
        }
        else
        {
            p_dev->led_on = !p_dev->led_on;
        }
        if(p_dev->led_on == 0)
        {
            GPIO_SetBits(GPIOC, GPIO_Pin_0);
        }
        else
        {
            GPIO_ResetBits(GPIOC, GPIO_Pin_0);
        }
        return 0;
    }
    else
    {
        return -1;
    }
}

int32 led_drv_close(svcrt_dev_hdr_t *p)
{
    led_dev_t *p_dev = (led_dev_t *)p;
    if((p == 0) || (sizeof(led_dev_t) != p->block_size))
    {
        return -1;
    }

    p_dev->opened = 0;
    GPIO_SetBits(GPIOC, GPIO_Pin_0);
    return 0;
}

int32 led_drv_ctrl(svcrt_dev_hdr_t *p, uint32 c, uint32 v)
{
    return 0;
}

svcrt_dev_drv_t led_drv =
{
    led_drv_open,
    led_drv_close,
    led_drv_read,
    led_drv_write,
    led_drv_ctrl
};
