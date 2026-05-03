/**
* @brief SVCrtOS 设备IO框架
* @details 提供统一的设备驱动接口和动态驱动注册机制
* @author chenjl
* @date 2019.07.05
*/

#include "devsio.h"

static DEV_DESCRIPT dev_list[SVCRT_DEV_MAX_NUM];
static int32 dev_count = 0;

void kerDevModuleInit(void)
{
    int32 i;
    for(i=0;i<SVCRT_DEV_MAX_NUM;i++)
    {
        dev_list[i].dev_name[0] = 0;
        dev_list[i].drv = 0;
        dev_list[i].dev_num = 0;
    }
    dev_count = 0;
}

int32 kerDevRegister(const char *name, DRV_INTERFACE *drv, uint32 dev_num)
{
    int32 i;
    if(name == 0 || drv == 0)
        return -2;

    if(dev_count >= SVCRT_DEV_MAX_NUM)
        return -1;

    for(i=0;i<dev_count;i++)
    {
        int32 j;
        uint8 match = 1;
        for(j=0;j<8;j++)
        {
            if(dev_list[i].dev_name[j] != name[j])
            {
                match = 0;
                break;
            }
            if(name[j] == 0)
                break;
        }
        if(match)
            return -2;
    }

    for(i=0;i<7;i++)
    {
        dev_list[dev_count].dev_name[i] = name[i];
        if(name[i] == 0)
            break;
    }
    dev_list[dev_count].dev_name[7] = 0;
    dev_list[dev_count].drv = drv;
    dev_list[dev_count].dev_num = dev_num;
    dev_count++;
    return 0;
}

int32 kerDevUnregister(const char *name)
{
    int32 i,j;
    if(name == 0)
        return -1;

    for(i=0;i<dev_count;i++)
    {
        uint8 match = 1;
        for(j=0;j<8;j++)
        {
            if(dev_list[i].dev_name[j] != name[j])
            {
                match = 0;
                break;
            }
            if(name[j] == 0)
                break;
        }
        if(match)
        {
            for(j=i;j<dev_count-1;j++)
            {
                dev_list[j] = dev_list[j+1];
            }
            dev_list[dev_count-1].dev_name[0] = 0;
            dev_list[dev_count-1].drv = 0;
            dev_list[dev_count-1].dev_num = 0;
            dev_count--;
            return 0;
        }
    }
    return -1;
}

int32 kerDevGetCount(void)
{
    return dev_count;
}

int32 kerDevOpen(char *name,uint32 param)
{
    int32 i;
    for(i=0;i<dev_count;i++)
    {
        int32 j;
        uint8 match = 1;
        for(j=0;j<8;j++)
        {
            if(dev_list[i].dev_name[j] != name[j])
            {
                match = 0;
                break;
            }
            if(name[j] == 0)
                break;
        }
        if(match && dev_list[i].drv != 0 && dev_list[i].drv->DrvOpen != 0)
        {
            DEV_HDR *p = dev_list[i].drv->DrvOpen(dev_list[i].dev_num, param);
            if(p != 0)
            {
                return (i | DEV_HANDLE_FLAG);
            }
        }
    }
    return -1;
}

int32 kerDevRead(int32 handle,uint8 *pData,int32 len)
{
    int32 idx = handle & HANDLE_RELMASK;
    if(idx >= dev_count)
        return -1;
    if(dev_list[idx].drv != 0 && dev_list[idx].drv->DrvRead != 0)
    {
        return dev_list[idx].drv->DrvRead(0, pData, len);
    }
    return -1;
}

int32 kerDevWrite(int32 handle,uint8 *pData,int32 len)
{
    int32 idx = handle & HANDLE_RELMASK;
    if(idx >= dev_count)
        return -1;
    if(dev_list[idx].drv != 0 && dev_list[idx].drv->DrvWrite != 0)
    {
        return dev_list[idx].drv->DrvWrite(0, pData, len);
    }
    return -1;
}

int32 kerDevCtrl(int32 handle,uint32 code,uint32 value)
{
    int32 idx = handle & HANDLE_RELMASK;
    if(idx >= dev_count)
        return -1;
    if(dev_list[idx].drv != 0 && dev_list[idx].drv->DrvCtrl != 0)
    {
        return dev_list[idx].drv->DrvCtrl(0, code, value);
    }
    return -1;
}
