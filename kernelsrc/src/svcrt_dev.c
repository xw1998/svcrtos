/**
* @brief SVCrtOS 设备驱动框架实现
* @details 提供统一的设备驱动接口和动态驱动注册机制
* @author xw
* @date 2026.05.03
*/

#include "svcrt_dev.h"

static svcrt_dev_desc_t svcrt_dev_list[SVCRT_DEV_MAX_NUM];
static int32 svcrt_dev_count = 0;

void svcrt_dev_module_init(void)
{
    int32 i;
    for(i = 0; i < SVCRT_DEV_MAX_NUM; i++)
    {
        svcrt_dev_list[i].dev_name[0] = 0;
        svcrt_dev_list[i].drv = 0;
        svcrt_dev_list[i].dev_num = 0;
    }
    svcrt_dev_count = 0;
}

int32 svcrt_dev_register(const char *name, svcrt_dev_drv_t *drv, uint32 dev_num)
{
    int32 i;
    if(name == 0 || drv == 0)
        return -2;

    if(svcrt_dev_count >= SVCRT_DEV_MAX_NUM)
        return -1;

    for(i = 0; i < svcrt_dev_count; i++)
    {
        int32 j;
        uint8 match = 1;
        for(j = 0; j < 8; j++)
        {
            if(svcrt_dev_list[i].dev_name[j] != name[j])
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

    for(i = 0; i < 7; i++)
    {
        svcrt_dev_list[svcrt_dev_count].dev_name[i] = name[i];
        if(name[i] == 0)
            break;
    }
    svcrt_dev_list[svcrt_dev_count].dev_name[7] = 0;
    svcrt_dev_list[svcrt_dev_count].drv = drv;
    svcrt_dev_list[svcrt_dev_count].dev_num = dev_num;
    svcrt_dev_count++;
    return 0;
}

int32 svcrt_dev_unregister(const char *name)
{
    int32 i, j;
    if(name == 0)
        return -1;

    for(i = 0; i < svcrt_dev_count; i++)
    {
        uint8 match = 1;
        for(j = 0; j < 8; j++)
        {
            if(svcrt_dev_list[i].dev_name[j] != name[j])
            {
                match = 0;
                break;
            }
            if(name[j] == 0)
                break;
        }
        if(match)
        {
            for(j = i; j < svcrt_dev_count - 1; j++)
            {
                svcrt_dev_list[j] = svcrt_dev_list[j + 1];
            }
            svcrt_dev_list[svcrt_dev_count - 1].dev_name[0] = 0;
            svcrt_dev_list[svcrt_dev_count - 1].drv = 0;
            svcrt_dev_list[svcrt_dev_count - 1].dev_num = 0;
            svcrt_dev_count--;
            return 0;
        }
    }
    return -1;
}

int32 svcrt_dev_get_count(void)
{
    return svcrt_dev_count;
}

int32 svcrt_dev_open_internal(char *name, uint32 param)
{
    int32 i;
    for(i = 0; i < svcrt_dev_count; i++)
    {
        int32 j;
        uint8 match = 1;
        for(j = 0; j < 8; j++)
        {
            if(svcrt_dev_list[i].dev_name[j] != name[j])
            {
                match = 0;
                break;
            }
            if(name[j] == 0)
                break;
        }
        if(match && svcrt_dev_list[i].drv != 0 && svcrt_dev_list[i].drv->drv_open != 0)
        {
            svcrt_dev_hdr_t *p = svcrt_dev_list[i].drv->drv_open(svcrt_dev_list[i].dev_num, param);
            if(p != 0)
            {
                return (i | SVCRT_DEV_HANDLE_FLAG);
            }
        }
    }
    return -1;
}

int32 svcrt_dev_read_internal(int32 handle, uint8 *pdata, int32 len)
{
    int32 idx = handle & SVCRT_HANDLE_RELMASK;
    if(idx >= svcrt_dev_count)
        return -1;
    if(svcrt_dev_list[idx].drv != 0 && svcrt_dev_list[idx].drv->drv_read != 0)
    {
        return svcrt_dev_list[idx].drv->drv_read(0, pdata, len);
    }
    return -1;
}

int32 svcrt_dev_write_internal(int32 handle, uint8 *pdata, int32 len)
{
    int32 idx = handle & SVCRT_HANDLE_RELMASK;
    if(idx >= svcrt_dev_count)
        return -1;
    if(svcrt_dev_list[idx].drv != 0 && svcrt_dev_list[idx].drv->drv_write != 0)
    {
        return svcrt_dev_list[idx].drv->drv_write(0, pdata, len);
    }
    return -1;
}

int32 svcrt_dev_ctrl_internal(int32 handle, uint32 code, uint32 value)
{
    int32 idx = handle & SVCRT_HANDLE_RELMASK;
    if(idx >= svcrt_dev_count)
        return -1;
    if(svcrt_dev_list[idx].drv != 0 && svcrt_dev_list[idx].drv->drv_ctrl != 0)
    {
        return svcrt_dev_list[idx].drv->drv_ctrl(0, code, value);
    }
    return -1;
}
