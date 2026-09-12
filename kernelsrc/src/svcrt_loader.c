/**
* @file svcrt_loader.c
* @brief SVCrtOS App 镜像加载器实现
* @details 加载流程（对应指导文档 6.3 节）：
*          1) 校验镜像头（魔数 / 硬件兼容签名 / 长度）；
*          2) 计算整镜像 CRC32 并与头中记录比对；
*          3) 查找空闲槽位，擦除槽位 Flash 区间；
*          4) 写入镜像（头 + 负载）；
*          5) 从 Flash 回读复算 CRC 确认；
*          6) 更新槽位状态为 LOADED，记录入口地址。
*          启动时把入口地址注册为内核任务（优先级 / 栈 / 周期取自分区配置）。
*
* @note 本模块只能由内核特权态调用（经 SVC 0x18 分发）。
*/

#include "svcrt_loader.h"
#include "svcrt_ptable.h"
#include "svcrt_share.h"
#include "svcrt_app_image.h"
#include "svcrt_hal.h"
#include "svcrt_config.h"
#include "svcrt_dev.h"
#include "svcrt_cfg.h"
#include "svcrt_task.h"
#include "svcrt_fault.h"
#include "svcrt_partition.h"    /* 内核专属：读取槽位 / 任务运行参数 */

/* 设备流式加载的分块缓冲（避免整镜像驻留 RAM） */
#define SVCRT_LOADER_CHUNK_SIZE   (512u)

/* 分区容量必须装得下任务栈：编译期就把配置错误暴露出来 */
typedef char svcrt_loader_app_stack_check[
    (APP_TASK_STACK_SIZE <= APP_RAM_SIZE) ? 1 : -1];

/* 设备流式加载的分块缓冲 */
static uint8  svcrt_loader_chunk[SVCRT_LOADER_CHUNK_SIZE];

/* ============================================================
 * CRC32（IEEE 802.3 反射多项式 0xEDB88320）
 * @note 与打包工具 tools/pack_app.py 使用同一算法，支持分段累积：
 *       crc = svcrt_crc32(seg1, len1, 0); crc = svcrt_crc32(seg2, len2, crc);
 * ============================================================ */
uint32 svcrt_crc32(const void *data, uint32 len, uint32 crc)
{
    const uint8 *p = (const uint8 *)data;
    uint32 i;
    uint32 j;

    crc = ~crc;
    for(i = 0u; i < len; i++)
    {
        crc ^= (uint32)p[i];
        for(j = 0u; j < 8u; j++)
        {
            if((crc & 1u) != 0u)
            {
                crc = (crc >> 1) ^ 0xEDB88320u;
            }
            else
            {
                crc >>= 1;
            }
        }
    }

    return ~crc;
}

/* 计算整镜像 CRC：头 256 字节（crc32 字段按 0 参与）+ 负载 */
static uint32 svcrt_loader_image_crc(const uint8 *image, uint32 total_len)
{
    const svcrt_app_header_t *p_hdr = (const svcrt_app_header_t *)image;
    uint32 crc_off = (uint32)((const uint8 *)&p_hdr->crc32 - image);
    uint32 zero = 0u;
    uint32 crc;

    crc = svcrt_crc32(image, crc_off, 0u);
    crc = svcrt_crc32(&zero, 4u, crc);
    crc = svcrt_crc32(image + crc_off + 4u, total_len - crc_off - 4u, crc);
    return crc;
}

/* 计算镜像入口地址（ARM 需置 Thumb 位） */
static uint32 svcrt_loader_entry_addr(uint32 slot_base, uint32 entry_offset)
{
    uint32 entry = slot_base + entry_offset;

    #if (SVCRT_ARCH_IS_ARM == 1)
    entry |= 1u;                /* Cortex-M：Thumb 指令集标志位 */
    #endif

    return entry;
}

/* 查找空闲槽位，返回槽位号或 -1 */
static int32 svcrt_loader_find_slot(void)
{
    svcrt_partition_table_t *pt = svcrt_ptable_get();
    uint32 i;

    for(i = 0u; i < pt->app_max_count && i < 8u; i++)
    {
        if(pt->slot_state[i] == SVCRT_APP_SLOT_EMPTY)
        {
            return (int32)i;
        }
    }

    return -1;
}

/* 校验镜像头公共字段；返回 0 或 SVCRT_LOADER_ERR_x */
static int32 svcrt_loader_check_header(const svcrt_app_header_t *p_hdr)
{
    if(p_hdr->magic != SVCRT_APP_MAGIC)
    {
        return SVCRT_LOADER_ERR_MAGIC;
    }

    if(p_hdr->hw_compat_id != SVCRT_HW_COMPAT_ID)
    {
        return SVCRT_LOADER_ERR_COMPAT;
    }

    if(p_hdr->image_size == 0u)
    {
        return SVCRT_LOADER_ERR_SIZE;
    }

    return 0;
}

/* 从设备读取指定长度（重试有限次，避免非阻塞读返回 0 时空转） */
static int32 svcrt_loader_read_dev(int32 dev, uint8 *buf, uint32 len)
{
    uint32 got = 0u;
    uint32 idle = 0u;

    while(got < len)
    {
        int32 r = svcrt_dev_read_internal(dev, buf + got, (int32)(len - got));

        if(r > 0)
        {
            got += (uint32)r;
            idle = 0u;
        }
        else
        {
            idle++;
            if(idle > 100000u)      /* 约百万次空转后放弃，防止死等 */
            {
                return -1;
            }
        }
    }

    return (int32)got;
}

int32 svcrt_loader_load_buffer(const uint8 *image, uint32 image_len)
{
    const svcrt_app_header_t *p_hdr;
    svcrt_partition_table_t *pt;
    uint32 total;
    uint32 slot;
    uint32 slot_base;
    int32 ret;

    if(image == 0 || image_len < SVCRT_APP_HEADER_SIZE)
    {
        return SVCRT_LOADER_ERR_PARAM;
    }

    p_hdr = (const svcrt_app_header_t *)image;

    ret = svcrt_loader_check_header(p_hdr);
    if(ret != 0)
    {
        return ret;
    }

    total = SVCRT_APP_HEADER_SIZE + p_hdr->image_size;
    if(total > image_len)
    {
        return SVCRT_LOADER_ERR_SIZE;
    }

    ret = svcrt_loader_find_slot();
    if(ret < 0)
    {
        return SVCRT_LOADER_ERR_NO_SLOT;
    }
    slot = (uint32)ret;

    pt = svcrt_ptable_get();
    if(total > pt->app_slot_size)
    {
        return SVCRT_LOADER_ERR_SIZE;
    }

    if(svcrt_loader_image_crc(image, total) != p_hdr->crc32)
    {
        return SVCRT_LOADER_ERR_CRC;
    }

    slot_base = pt->app_user_base + slot * pt->app_slot_size;

    /* 进入安装态：写入未完成前不允许被启动；掉电中断则由 CRC 校验判为 INVALID */
    svcrt_ptable_set_slot(slot, SVCRT_APP_SLOT_INSTALLING, 0u, 0u);

    if(svcrt_port_flash_erase(slot_base, total) != 0)
    {
        svcrt_ptable_set_slot(slot, SVCRT_APP_SLOT_EMPTY, 0u, 0u);
        return SVCRT_LOADER_ERR_FLASH;
    }

    if(svcrt_port_flash_write(slot_base, image, total) != 0)
    {
        return SVCRT_LOADER_ERR_FLASH;
    }

    /* 回读校验：Flash 已映射，可直接按地址复算 */
    if(svcrt_loader_image_crc((const uint8 *)slot_base, total) != p_hdr->crc32)
    {
        svcrt_ptable_set_slot(slot, SVCRT_APP_SLOT_EMPTY, 0u, 0u);
        return SVCRT_LOADER_ERR_CRC;
    }

    /* 新镜像写入成功：清零故障计数（重新安装 = 重新开始） */
    pt->slot_crash_cnt[slot] = 0u;

    svcrt_ptable_set_slot(slot, SVCRT_APP_SLOT_LOADED,
                          svcrt_loader_entry_addr(slot_base, p_hdr->entry_offset), 0u);

    return (int32)slot;
}

int32 svcrt_loader_load_dev(int32 dev, uint32 image_len)
{
    svcrt_app_header_t hdr;

    if(dev < 0)
    {
        return SVCRT_LOADER_ERR_PARAM;
    }

    if(svcrt_loader_read_dev(dev, (uint8 *)&hdr, SVCRT_APP_HEADER_SIZE) != (int32)SVCRT_APP_HEADER_SIZE)
    {
        return SVCRT_LOADER_ERR_SIZE;
    }

    return svcrt_loader_load_dev_hdr(dev, &hdr, image_len);
}

int32 svcrt_loader_load_dev_hdr(int32 dev, const svcrt_app_header_t *p_hdr, uint32 image_len)
{
    /* 头已由调用方（如安装任务）读出并已完成魔数同步，这里只需继续流式写入负载 */
    svcrt_app_header_t hdr = *p_hdr;
    svcrt_partition_table_t *pt;
    uint32 total;
    uint32 slot;
    uint32 slot_base;
    uint32 written;
    int32 ret;

    if(dev < 0)
    {
        return SVCRT_LOADER_ERR_PARAM;
    }

    ret = svcrt_loader_check_header(&hdr);
    if(ret != 0)
    {
        return ret;
    }

    total = SVCRT_APP_HEADER_SIZE + hdr.image_size;
    if(image_len != 0u && total > image_len)
    {
        return SVCRT_LOADER_ERR_SIZE;
    }

    ret = svcrt_loader_find_slot();
    if(ret < 0)
    {
        return SVCRT_LOADER_ERR_NO_SLOT;
    }
    slot = (uint32)ret;

    pt = svcrt_ptable_get();
    if(total > pt->app_slot_size)
    {
        return SVCRT_LOADER_ERR_SIZE;
    }

    slot_base = pt->app_user_base + slot * pt->app_slot_size;

    /* 进入安装态：写入未完成前不允许被启动；掉电中断则由 CRC 校验判为 INVALID */
    svcrt_ptable_set_slot(slot, SVCRT_APP_SLOT_INSTALLING, 0u, 0u);

    if(svcrt_port_flash_erase(slot_base, total) != 0)
    {
        svcrt_ptable_set_slot(slot, SVCRT_APP_SLOT_EMPTY, 0u, 0u);
        return SVCRT_LOADER_ERR_FLASH;
    }

    if(svcrt_port_flash_write(slot_base, (const uint8 *)&hdr, SVCRT_APP_HEADER_SIZE) != 0)
    {
        return SVCRT_LOADER_ERR_FLASH;
    }

    written = 0u;
    while(written < hdr.image_size)
    {
        uint32 want = hdr.image_size - written;

        if(want > SVCRT_LOADER_CHUNK_SIZE)
        {
            want = SVCRT_LOADER_CHUNK_SIZE;
        }

        if(svcrt_loader_read_dev(dev, svcrt_loader_chunk, want) != (int32)want)
        {
            svcrt_ptable_set_slot(slot, SVCRT_APP_SLOT_EMPTY, 0u, 0u);
            return SVCRT_LOADER_ERR_SIZE;
        }

        if(svcrt_port_flash_write(slot_base + SVCRT_APP_HEADER_SIZE + written,
                                  svcrt_loader_chunk, want) != 0)
        {
            svcrt_ptable_set_slot(slot, SVCRT_APP_SLOT_EMPTY, 0u, 0u);
            return SVCRT_LOADER_ERR_FLASH;
        }

        written += want;
    }

    if(svcrt_loader_image_crc((const uint8 *)slot_base, total) != hdr.crc32)
    {
        svcrt_ptable_set_slot(slot, SVCRT_APP_SLOT_EMPTY, 0u, 0u);
        return SVCRT_LOADER_ERR_CRC;
    }

    /* 新镜像写入成功：清零故障计数（重新安装 = 重新开始） */
    pt->slot_crash_cnt[slot] = 0u;

    svcrt_ptable_set_slot(slot, SVCRT_APP_SLOT_LOADED,
                          svcrt_loader_entry_addr(slot_base, hdr.entry_offset), 0u);

    return (int32)slot;
}

uint32 svcrt_loader_scan(void)
{
    svcrt_partition_table_t *pt = svcrt_ptable_get();
    uint32 found = 0u;
    uint32 i;

    for(i = 0u; i < pt->app_max_count && i < 8u; i++)
    {
        const svcrt_app_header_t *p_hdr =
            (const svcrt_app_header_t *)(pt->app_user_base + i * pt->app_slot_size);
        uint32 total;

        if(p_hdr->magic != SVCRT_APP_MAGIC)
        {
            continue;                           /* 空槽位：Flash 为擦除值 0xFF */
        }

        if((p_hdr->hw_compat_id != SVCRT_HW_COMPAT_ID) || (p_hdr->image_size == 0u))
        {
            svcrt_ptable_set_slot(i, SVCRT_APP_SLOT_INVALID, 0u, 0u);
            continue;
        }

        total = SVCRT_APP_HEADER_SIZE + p_hdr->image_size;
        if(total > pt->app_slot_size)
        {
            svcrt_ptable_set_slot(i, SVCRT_APP_SLOT_INVALID, 0u, 0u);
            continue;
        }

        /* Flash 已映射，直接按地址复算 CRC */
        if(svcrt_loader_image_crc((const uint8 *)p_hdr, total) != p_hdr->crc32)
        {
            svcrt_ptable_set_slot(i, SVCRT_APP_SLOT_INVALID, 0u, 0u);
            continue;
        }

        svcrt_ptable_set_slot(i, SVCRT_APP_SLOT_LOADED,
                              svcrt_loader_entry_addr(pt->app_user_base + i * pt->app_slot_size,
                                                      p_hdr->entry_offset), 0u);
        found++;
    }

    return found;
}

int32 svcrt_loader_on_fault(int32 task_id)
{
    svcrt_partition_table_t *pt = svcrt_ptable_get();
    uint32 i;

    if(task_id <= 0)
    {
        return -1;
    }

    for(i = 0u; i < pt->app_max_count && i < 8u; i++)
    {
        if(pt->slot_task_id[i] != (uint32)task_id)
        {
            continue;
        }

        pt->slot_crash_cnt[i]++;

        #if (APP_CRASH_RESTART_MAX > 0)
        if(pt->slot_crash_cnt[i] >= (uint32)APP_CRASH_RESTART_MAX)
        {
            /* 连续故障达到上限：禁用该 App —— 不再重启，并让它脱离调度。
             * 槽位置 INVALID（而非 EMPTY），以便宿主/上位机通过 svcrt_app_status()
             * 区分“被禁用的应用”与“空槽位”；重新安装即可清零计数、重新启用。 */
            SVCRT_DISABLE_IRQ();
            svcrt_task_table[task_id - 1].recover_pending = 0u;
            svcrt_task_table[task_id - 1].status          = SVCRT_TASK_INVALID;
            SVCRT_ENABLE_IRQ();

            svcrt_ptable_set_slot(i, SVCRT_APP_SLOT_INVALID, pt->slot_entry[i], 0u);

            /* 记一条可诊断的故障：上位机可用 svcrt_fault_record_read() 看到“被禁用”的原因 */
            svcrt_fault_record(SVCRT_FAULT_APPDISABLED, task_id);
            return 1;
        }
        #endif

        return 0;       /* 未达上限：由调用方安排恢复（重启该 App） */
    }

    return -1;          /* 不属于任何 App 槽位（内核任务）：沿用默认处理 */
}

int32 svcrt_loader_start(uint32 slot)
{
    svcrt_partition_table_t *pt = svcrt_ptable_get();
    svcrt_app_header_t hdr;
    uint32 slot_base;
    uint32 entry;
    uint32 stack_top;
    uint32 stack_bottom;
    int32 task_id;

    if(slot >= pt->app_max_count)
    {
        return SVCRT_LOADER_ERR_PARAM;
    }

    if(pt->slot_state[slot] != SVCRT_APP_SLOT_LOADED)
    {
        return SVCRT_LOADER_ERR_STATE;
    }

    entry = pt->slot_entry[slot];
    if(entry == 0u)
    {
        return SVCRT_LOADER_ERR_STATE;
    }

    /* 栈不是另外分配的缓冲区，而是从 App 自己的 RAM 区顶部切出来的：
     *   stack_top    = APP_RAM_BASE + APP_RAM_SIZE
     *   stack_bottom = stack_top - APP_TASK_STACK_SIZE
     * 该地址与 App 端 .sct 中 ARM_LIB_STACK 指向的栈顶完全一致（见 gen_scatter.py），
     * 因此 __main 设置 SP 后与内核推导值一致，不存在“双份栈”。
     * 地址只在 config/svcrt_partition.h 定义一次，此处全部派生。 */
    stack_top    = APP_RAM_BASE + APP_RAM_SIZE;
    stack_bottom = stack_top - APP_TASK_STACK_SIZE;

    task_id = svcrt_task_register((void (*)(void))entry,
                                  (uint32 *)stack_bottom,
                                  (uint32)APP_TASK_STACK_SIZE,
                                  (uint8)APP_TASK_PRIORITY,
                                  (uint32)APP_TASK_PERIOD_MS);
    if(task_id <= 0)
    {
        return SVCRT_LOADER_ERR_TASK;
    }

    slot_base = pt->app_user_base + slot * pt->app_slot_size;

    /* 修正 TCB 的区域描述：任务的可访问范围是整个 App 分区，而不是只有栈。
     * 这是为后续 MPU 隔离预留的元数据（AnOs 的 kerAppInitMpu 就是按这组值配 MPU）。 */
    svcrt_task_table[task_id - 1].ram_start = APP_RAM_BASE;
    svcrt_task_table[task_id - 1].ram_size  = APP_RAM_SIZE;
    svcrt_task_table[task_id - 1].rom_start = slot_base;
    svcrt_task_table[task_id - 1].rom_size  = (svcrt_ptable_read_header(slot, &hdr) == 0)
                                              ? hdr.image_size : 0u;

    svcrt_ptable_set_slot(slot, SVCRT_APP_SLOT_RUNNING, entry, (uint32)task_id);

    svcrt_sched_activate_higher((uint8)APP_TASK_PRIORITY);

    return task_id;
}

int32 svcrt_loader_stop(uint32 slot)
{
    svcrt_partition_table_t *pt = svcrt_ptable_get();
    uint32 task_id;

    if(slot >= pt->app_max_count)
    {
        return SVCRT_LOADER_ERR_PARAM;
    }

    task_id = pt->slot_task_id[slot];
    if(task_id == 0u || task_id > (uint32)svcrt_task_count)
    {
        return SVCRT_LOADER_ERR_STATE;
    }

    SVCRT_DISABLE_IRQ();
    svcrt_task_table[task_id - 1u].recover_pending = 0u;
    svcrt_task_table[task_id - 1u].status          = SVCRT_TASK_INVALID;
    SVCRT_ENABLE_IRQ();

    svcrt_ptable_set_slot(slot, SVCRT_APP_SLOT_LOADED, pt->slot_entry[slot], 0u);

    SVCRT_SWITCH_TASK();

    return 0;
}

uint32 svcrt_loader_state(uint32 slot)
{
    svcrt_partition_table_t *pt = svcrt_ptable_get();

    if(slot >= pt->app_max_count)
    {
        return 0xffffffffu;
    }

    return pt->slot_state[slot];
}
