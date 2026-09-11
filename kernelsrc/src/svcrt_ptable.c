/**
* @file svcrt_ptable.c
* @brief SVCrtOS 分区表运行期实现
* @details 唯一允许包含 config/svcrt_partition.h 的地方之一：把编译期布局
*          翻译为共享内存中的 svcrt_partition_table_t。
*
*          共享内存位于 SHARE_RAM_BASE（config/svcrt_partition.h 中定义），
*          该区域不在任何工程的 .sct 中分配，专供分区表与内核/App 数据交换使用。
*
* @note 本文件不得包含任何芯片寄存器操作，只做数据搬运。
*/

#include "svcrt_ptable.h"
#include "svcrt_share.h"
#include "svcrt_app_image.h"
#include "svcrt_partition.h"    /* 内核专属：全工程唯一地址源头 */

/* 共享内存中的分区表实例（按绝对地址访问，不占用链接器分配的 RAM） */
static svcrt_partition_table_t *svcrt_ptable_ptr(void)
{
    return (svcrt_partition_table_t *)SHARE_RAM_BASE;
}

void svcrt_ptable_init(void)
{
    svcrt_partition_table_t *pt = svcrt_ptable_ptr();
    uint32 i;

    pt->magic          = SVCRT_PARTITION_MAGIC;
    pt->version        = SVCRT_PARTITION_VERSION;
    pt->hw_compat_id   = SVCRT_HW_COMPAT_ID;

    pt->kernel_base    = KERNEL_BASE;
    pt->kernel_size    = KERNEL_SIZE;
    pt->driver_pool_base = DRIVER_POOL_BASE;
    pt->driver_pool_size = DRIVER_POOL_SIZE;
    pt->app_user_base  = APP_USER_BASE;
    pt->app_user_size  = APP_USER_SIZE;
    pt->app_slot_size  = APP_SLOT_SIZE;
    pt->app_max_count  = APP_MAX_COUNT;

    pt->share_ram_base = SHARE_RAM_BASE;
    pt->share_ram_size = SHARE_RAM_SIZE;
    pt->kernel_ram_base = KERNEL_RAM_BASE;
    pt->kernel_ram_size = KERNEL_RAM_SIZE;
    pt->driver_ram_base = DRIVER_RAM_BASE;
    pt->driver_ram_size = DRIVER_RAM_SIZE;
    pt->app_ram_base   = APP_RAM_BASE;
    pt->app_ram_size   = APP_RAM_SIZE;

    for(i = 0; i < 8u; i++)
    {
        pt->slot_state[i]   = SVCRT_APP_SLOT_EMPTY;
        pt->slot_entry[i]   = 0;
        pt->slot_task_id[i] = 0;
    }
}

svcrt_partition_table_t *svcrt_ptable_get(void)
{
    svcrt_partition_table_t *pt = svcrt_ptable_ptr();

    if(pt->magic != SVCRT_PARTITION_MAGIC)
    {
        /* 极早期被调用：就地补一次初始化，保证调用方总能拿到有效表 */
        svcrt_ptable_init();
    }

    return pt;
}

int32 svcrt_ptable_read_header(uint32 slot, svcrt_app_header_t *out)
{
    svcrt_partition_table_t *pt = svcrt_ptable_get();
    const svcrt_app_header_t *p_hdr;

    if(slot >= pt->app_max_count)
    {
        return -1;
    }

    p_hdr = (const svcrt_app_header_t *)(pt->app_user_base + slot * pt->app_slot_size);

    if(p_hdr->magic != SVCRT_APP_MAGIC)
    {
        return -2;
    }

    if(out != 0)
    {
        *out = *p_hdr;
    }

    return 0;
}

int32 svcrt_ptable_set_slot(uint32 slot, uint32 state, uint32 entry, uint32 task_id)
{
    svcrt_partition_table_t *pt = svcrt_ptable_get();

    if(slot >= pt->app_max_count || slot >= 8u)
    {
        return -1;
    }

    pt->slot_state[slot]   = state;
    pt->slot_entry[slot]   = entry;
    pt->slot_task_id[slot] = task_id;

    return 0;
}
