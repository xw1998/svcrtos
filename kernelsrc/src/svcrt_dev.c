/**
* @brief SVCrtOS 设备驱动框架实现
* @details 提供统一的设备驱动接口和动态驱动注册机制
*          内置驱动和可安装驱动共用同一套设备表
* @author xw
* @date 2026.05.03
*/

#include "svcrt_dev.h"

static svcrt_dev_desc_t svcrt_dev_list[SVCRT_DEV_MAX_NUM];
static svcrt_dev_hdr_t *svcrt_dev_handles[SVCRT_DEV_MAX_NUM];
static uint8 svcrt_dev_refs[SVCRT_DEV_MAX_NUM];
static int32 svcrt_dev_count = 0;

void svcrt_dev_module_init(void)
{
    int32 i;
    for(i = 0; i < SVCRT_DEV_MAX_NUM; i++)
    {
        svcrt_dev_list[i].dev_name[0] = 0;
        svcrt_dev_list[i].drv = 0;
        svcrt_dev_list[i].dev_num = 0;
        svcrt_dev_handles[i] = 0;
        svcrt_dev_refs[i]    = 0;
    }
    svcrt_dev_count = 0;
}

int32 svcrt_dev_register(const char *name, svcrt_dev_drv_t *drv, uint32 dev_num)
{
    int32 i;
    int32 slot = -1;

    if(name == 0 || drv == 0)
        return -2;

    /* 空槽位以 dev_name[0]==0 标记。注销设备时只清空槽位、不移动数组元素：
     * 设备句柄就是槽位下标（i | SVCRT_DEV_HANDLE_FLAG），一旦移动，
     * 已发放的句柄就会指向另一个设备。 */
    for(i = 0; i < SVCRT_DEV_MAX_NUM; i++)
    {
        int32 j;
        uint8 match = 1;

        if(svcrt_dev_list[i].dev_name[0] == 0)
        {
            if(slot < 0 && svcrt_dev_handles[i] == 0)
            {
                slot = i;
            }
            continue;
        }

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

    if(slot < 0)
        return -1;

    for(i = 0; i < 7; i++)
    {
        svcrt_dev_list[slot].dev_name[i] = name[i];
        if(name[i] == 0)
            break;
    }
    svcrt_dev_list[slot].dev_name[7] = 0;
    svcrt_dev_list[slot].drv = drv;
    svcrt_dev_list[slot].dev_num = dev_num;
    svcrt_dev_count++;
    return 0;
}

int32 svcrt_dev_unregister(const char *name)
{
    int32 i, j;
    if(name == 0)
        return -1;

    for(i = 0; i < SVCRT_DEV_MAX_NUM; i++)
    {
        uint8 match = 1;

        if(svcrt_dev_list[i].dev_name[0] == 0)
        {
            continue;                       /* 空槽位 */
        }

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
            /* 墓碑式清空：保持槽位下标不变，已发放的句柄仍指向原槽位，
             * 此时该槽位 drv==0 / handles==0，后续读写会安全地返回 -1，
             * 而不是（像整体前移那样）指向另一个设备。 */
            svcrt_dev_list[i].dev_name[0] = 0;
            svcrt_dev_list[i].drv         = 0;
            svcrt_dev_list[i].dev_num     = 0;
            svcrt_dev_handles[i]          = 0;
            svcrt_dev_refs[i]             = 0;
            if(svcrt_dev_count > 0)
            {
                svcrt_dev_count--;
            }
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
    /* 槽位可能不连续（注销留下的空槽位），必须遍历整个表 */
    for(i = 0; i < SVCRT_DEV_MAX_NUM; i++)
    {
        int32 j;
        uint8 match = 1;
        if(svcrt_dev_list[i].dev_name[0] == 0)
        {
            continue;
        }
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
        /* Already open: hand back the same instance and only count the
         * new opener. Calling drv_open a second time re-initialises the
         * hardware and installs a fresh instance, which silently orphans
         * the handle the first opener still holds - an App opening the
         * console UART used to leave the kernel shell unable to receive
         * input for good. */
        if(match && (svcrt_dev_handles[i] != 0))
        {
            if(svcrt_dev_refs[i] < 255)
            {
                svcrt_dev_refs[i]++;
            }
            return (i | SVCRT_DEV_HANDLE_FLAG);
        }

        if(match && svcrt_dev_list[i].drv != 0 && svcrt_dev_list[i].drv->drv_open != 0)
        {
            svcrt_dev_hdr_t *p = svcrt_dev_list[i].drv->drv_open(svcrt_dev_list[i].dev_num, param);
            if(p != 0)
            {
                svcrt_dev_handles[i] = p;
                svcrt_dev_refs[i]    = 1;
                return (i | SVCRT_DEV_HANDLE_FLAG);
            }
        }
    }
    return -1;
}

int32 svcrt_dev_close_internal(int32 handle)
{
    int32 idx = handle & SVCRT_HANDLE_RELMASK;

    if(SVCRT_DEV_HANDLE_FLAG != (handle & SVCRT_HANDLE_MASK))
        return -1;
    /* 句柄是槽位下标：合法范围是整个设备表，而不是当前在用数量 */
    if(idx < 0 || idx >= SVCRT_DEV_MAX_NUM)
        return -1;

    /* One instance can have several openers: only the last close may
     * touch the hardware. Otherwise an App closing the console it shares
     * with the shell would take the shell's console away. */
    if(svcrt_dev_refs[idx] > 1)
    {
        svcrt_dev_refs[idx]--;
        return 0;
    }
    svcrt_dev_refs[idx] = 0;

    if(svcrt_dev_list[idx].drv != 0 && svcrt_dev_list[idx].drv->drv_close != 0)
    {
        int32 ret = svcrt_dev_list[idx].drv->drv_close(svcrt_dev_handles[idx]);
        svcrt_dev_handles[idx] = 0;
        return ret;
    }

    svcrt_dev_handles[idx] = 0;
    return 0;
}

int32 svcrt_dev_read_internal(int32 handle, uint8 *pdata, int32 len)
{
    int32 idx = handle & SVCRT_HANDLE_RELMASK;

    if(SVCRT_DEV_HANDLE_FLAG != (handle & SVCRT_HANDLE_MASK))
        return -1;
    /* 句柄是槽位下标：合法范围是整个设备表，而不是当前在用数量 */
    if(idx < 0 || idx >= SVCRT_DEV_MAX_NUM)
        return -1;
    if(svcrt_dev_handles[idx] == 0)
        return -1;

    if(svcrt_dev_list[idx].drv != 0 && svcrt_dev_list[idx].drv->drv_read != 0)
    {
        return svcrt_dev_list[idx].drv->drv_read(svcrt_dev_handles[idx], pdata, len);
    }
    return -1;
}

int32 svcrt_dev_write_internal(int32 handle, uint8 *pdata, int32 len)
{
    int32 idx = handle & SVCRT_HANDLE_RELMASK;

    if(SVCRT_DEV_HANDLE_FLAG != (handle & SVCRT_HANDLE_MASK))
        return -1;
    /* 句柄是槽位下标：合法范围是整个设备表，而不是当前在用数量 */
    if(idx < 0 || idx >= SVCRT_DEV_MAX_NUM)
        return -1;
    if(svcrt_dev_handles[idx] == 0)
        return -1;

    if(svcrt_dev_list[idx].drv != 0 && svcrt_dev_list[idx].drv->drv_write != 0)
    {
        return svcrt_dev_list[idx].drv->drv_write(svcrt_dev_handles[idx], pdata, len);
    }
    return -1;
}

int32 svcrt_dev_ctrl_internal(int32 handle, uint32 code, uint32 value)
{
    int32 idx = handle & SVCRT_HANDLE_RELMASK;

    if(SVCRT_DEV_HANDLE_FLAG != (handle & SVCRT_HANDLE_MASK))
        return -1;
    /* 句柄是槽位下标：合法范围是整个设备表，而不是当前在用数量 */
    if(idx < 0 || idx >= SVCRT_DEV_MAX_NUM)
        return -1;
    if(svcrt_dev_handles[idx] == 0)
        return -1;

    if(svcrt_dev_list[idx].drv != 0 && svcrt_dev_list[idx].drv->drv_ctrl != 0)
    {
        return svcrt_dev_list[idx].drv->drv_ctrl(svcrt_dev_handles[idx], code, value);
    }
    return -1;
}
