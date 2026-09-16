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
#include "svcrt_spin.h"

/* Guards the (state, entry, task_id) triple of every slot.
 *
 * Two writers can meet here: the installer task (normal task context) and the
 * crash policy in svcrt_loader_on_fault(), which runs from exception context.
 * Without the lock a reader could observe state = LOADED together with a stale
 * entry or task id. The irqsave variant is used because the fault path runs
 * with exceptions masked already - disabling interrupts makes a nested take
 * safe (the lock stays re-entrant per CPU, see svcrt_spin_lock_irqsave). */
static svcrt_spinlock_t svcrt_ptable_lock = SVCRT_SPINLOCK_INIT;

/* 槽位数组的固定长度必须真实存在于结构体里，否则下面的循环会越界。
 * 两边都是编译期常量，配置错了直接编译不过。 */
typedef char svcrt_ptable_slot_array_check[
    ((DRIVER_MAX_COUNT <= SVCRT_SLOT_ARRAY_MAX) &&
     (APP_MAX_COUNT    <= SVCRT_SLOT_ARRAY_MAX)) ? 1 : -1];

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
    pt->driver_slot_size = DRIVER_SLOT_SIZE;
    pt->driver_max_count = DRIVER_MAX_COUNT;
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
    pt->driver_slot_ram_size = DRIVER_SLOT_RAM_SIZE;
    pt->app_ram_base   = APP_RAM_BASE;
    pt->app_ram_size   = APP_RAM_SIZE;
    pt->app_slot_ram_size = APP_SLOT_RAM_SIZE;

    for(i = 0; i < SVCRT_SLOT_ARRAY_MAX; i++)
    {
        pt->slot_state[i]    = SVCRT_APP_SLOT_EMPTY;
        pt->slot_entry[i]    = 0;
        pt->slot_task_id[i]  = 0;
        pt->slot_crash_cnt[i] = 0;

        pt->driver_slot_state[i]    = SVCRT_APP_SLOT_EMPTY;
        pt->driver_slot_entry[i]    = 0;
        pt->driver_slot_task_id[i]  = 0;
        pt->driver_slot_crash_cnt[i] = 0;

        /* 自启标志在镜像认定时由 loader 按镜像头 flags 回填；
         * 这里先清 0，避免未认定的槽位带着上一轮（共享 RAM 未清零时）的残留值。 */
        pt->slot_autostart[i]        = 0;
        pt->driver_slot_autostart[i] = 0;
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
    uint32 irq_state;

    if(slot >= pt->app_max_count || slot >= SVCRT_SLOT_ARRAY_MAX)
    {
        return -1;
    }

    svcrt_spin_lock_irqsave(&svcrt_ptable_lock, &irq_state);

    pt->slot_state[slot]   = state;
    pt->slot_entry[slot]   = entry;
    pt->slot_task_id[slot] = task_id;

    svcrt_spin_unlock_irqrestore(&svcrt_ptable_lock, irq_state);

    return 0;
}

int32 svcrt_ptable_set_driver_slot(uint32 slot, uint32 state, uint32 entry, uint32 task_id)
{
    svcrt_partition_table_t *pt = svcrt_ptable_get();
    uint32 irq_state;

    if(slot >= pt->driver_max_count || slot >= SVCRT_SLOT_ARRAY_MAX)
    {
        return -1;
    }

    svcrt_spin_lock_irqsave(&svcrt_ptable_lock, &irq_state);

    pt->driver_slot_state[slot]   = state;
    pt->driver_slot_entry[slot]   = entry;
    pt->driver_slot_task_id[slot] = task_id;

    svcrt_spin_unlock_irqrestore(&svcrt_ptable_lock, irq_state);

    return 0;
}

int32 svcrt_ptable_get_driver_slot(uint32 slot, uint32 *p_state, uint32 *p_entry, uint32 *p_task_id)
{
    svcrt_partition_table_t *pt = svcrt_ptable_get();
    uint32 irq_state;

    if(slot >= pt->driver_max_count || slot >= SVCRT_SLOT_ARRAY_MAX)
    {
        return -1;
    }

    svcrt_spin_lock_irqsave(&svcrt_ptable_lock, &irq_state);

    if(p_state != 0)
    {
        *p_state = pt->driver_slot_state[slot];
    }
    if(p_entry != 0)
    {
        *p_entry = pt->driver_slot_entry[slot];
    }
    if(p_task_id != 0)
    {
        *p_task_id = pt->driver_slot_task_id[slot];
    }

    svcrt_spin_unlock_irqrestore(&svcrt_ptable_lock, irq_state);

    return 0;
}

int32 svcrt_ptable_get_slot(uint32 slot, uint32 *p_state, uint32 *p_entry, uint32 *p_task_id)
{
    svcrt_partition_table_t *pt = svcrt_ptable_get();
    uint32 irq_state;

    if(slot >= pt->app_max_count || slot >= SVCRT_SLOT_ARRAY_MAX)
    {
        return -1;
    }

    svcrt_spin_lock_irqsave(&svcrt_ptable_lock, &irq_state);

    if(p_state != 0)
    {
        *p_state = pt->slot_state[slot];
    }
    if(p_entry != 0)
    {
        *p_entry = pt->slot_entry[slot];
    }
    if(p_task_id != 0)
    {
        *p_task_id = pt->slot_task_id[slot];
    }

    svcrt_spin_unlock_irqrestore(&svcrt_ptable_lock, irq_state);

    return 0;
}
