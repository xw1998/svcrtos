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
typedef char svcrt_loader_driver_stack_check[
    (DRIVER_TASK_STACK_SIZE <= DRIVER_RAM_SIZE) ? 1 : -1];

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

/* 计算镜像入口地址（ARM 需置 Thumb 位）
 * 槽位内布局为 [镜像头 256 字节][负载]，负载的链接基址 = 槽位基址 + SVCRT_APP_HEADER_SIZE，
 * entry_offset 是入口相对“负载起始”的偏移，所以入口 = 分区基址 + 头长 + entry_offset。
 * （裸镜像调试路径不经过本函数：它没有镜像头，入口直接取分区基址，
 *   见 svcrt_loader_identify 里的 APP_ALLOW_RAW_IMAGE 分支） */
static uint32 svcrt_loader_entry_addr(uint32 slot_base, uint32 entry_offset)
{
    uint32 entry = slot_base + SVCRT_APP_HEADER_SIZE + entry_offset;

    #if (SVCRT_ARCH_IS_ARM == 1)
    entry |= 1u;                /* Cortex-M：Thumb 指令集标志位 */
    #endif

    return entry;
}

/* 查找可复用槽位，返回槽位号或 -1
 * 可复用 = 空槽 / 内容无效（含被禁用）/ 已停止（LOADED）。
 * RUNNING 不在其中：正在运行的任务不允许被覆盖擦除，必须先 svcrt_app_stop()。 */
static int32 svcrt_loader_find_slot(void)
{
    svcrt_partition_table_t *pt = svcrt_ptable_get();
    uint32 i;

    for(i = 0u; i < pt->app_max_count && i < 8u; i++)
    {
        uint32 st = pt->slot_state[i];

        if((st == SVCRT_APP_SLOT_EMPTY) || (st == SVCRT_APP_SLOT_INVALID) ||
           (st == SVCRT_APP_SLOT_LOADED))
        {
            return (int32)i;
        }
    }

    return -1;
}

/* 硬停一个正在运行的外部任务（App 槽位或驱动区）。
 * 用于覆盖安装前把旧任务踢出调度，避免“擦除正在执行的代码区”导致取指崩溃。 */
static void svcrt_loader_halt_task(uint32 task_id)
{
    if(task_id == 0u || task_id > (uint32)svcrt_task_count)
    {
        return;
    }

    SVCRT_DISABLE_IRQ();
    svcrt_task_table[task_id - 1u].recover_pending = 0u;
    svcrt_task_table[task_id - 1u].status          = SVCRT_TASK_INVALID;
    SVCRT_ENABLE_IRQ();
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

    /* image_size 不能让 total = 头长 + image_size 回绕：
     * 一旦回绕（如 image_size = 0xFFFFFFFF 时 total = 255），调用方的
     * “total > 槽位容量”检查会被绕过，随后的擦除/写入/CRC 全部按错误长度进行。 */
    if(p_hdr->image_size > (0xFFFFFFFFu - SVCRT_APP_HEADER_SIZE))
    {
        return SVCRT_LOADER_ERR_SIZE;
    }

    /* entry_offset 是入口相对「负载起始」的偏移，必须落在负载范围内，
     * 否则镜像能把入口指到分区内任意地址（含镜像头或数据区）。 */
    if(p_hdr->entry_offset >= p_hdr->image_size)
    {
        return SVCRT_LOADER_ERR_ENTRY;
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

    /* 缓冲区路径同样只收 App 镜像；驱动镜像走 load_driver* */
    if(p_hdr->type != SVCRT_APP_TYPE_APP)
    {
        return SVCRT_LOADER_ERR_PARAM;
    }

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

    /* 镜像头里的期望加载地址必须等于目标分区基址。
     * 打包工具会用该字段并在 --verify 时校验（tools/pack_app.py），
     * 内核此前不校验 → 按其它基址链接的镜像会被照单收下，启动后跑飞。 */
    if(p_hdr->load_addr != slot_base)
    {
        return SVCRT_LOADER_ERR_ADDR;
    }

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

/* 把负载流式写入已擦除的分区（头 + 分块负载），返回 0 或 SVCRT_LOADER_ERR_x */
static int32 svcrt_loader_stream_payload(int32 dev, uint32 base, const svcrt_app_header_t *p_hdr)
{
    uint32 written = 0u;

    if(svcrt_port_flash_write(base, (const uint8 *)p_hdr, SVCRT_APP_HEADER_SIZE) != 0)
    {
        return SVCRT_LOADER_ERR_FLASH;
    }

    while(written < p_hdr->image_size)
    {
        uint32 want = p_hdr->image_size - written;

        if(want > SVCRT_LOADER_CHUNK_SIZE)
        {
            want = SVCRT_LOADER_CHUNK_SIZE;
        }

        if(svcrt_loader_read_dev(dev, svcrt_loader_chunk, want) != (int32)want)
        {
            return SVCRT_LOADER_ERR_SIZE;
        }

        if(svcrt_port_flash_write(base + SVCRT_APP_HEADER_SIZE + written,
                                  svcrt_loader_chunk, want) != 0)
        {
            return SVCRT_LOADER_ERR_FLASH;
        }

        written += want;
    }

    return 0;
}

int32 svcrt_loader_load_dev_hdr(int32 dev, const svcrt_app_header_t *p_hdr, uint32 image_len)
{
    /* 头已由调用方（如安装任务）读出并已完成魔数同步，这里只需继续流式写入负载 */
    svcrt_partition_table_t *pt = svcrt_ptable_get();
    svcrt_app_header_t hdr = *p_hdr;
    uint32 total;
    uint32 slot_base;
    int32 slot;
    int32 ret;

    if(dev < 0)
    {
        return SVCRT_LOADER_ERR_PARAM;
    }

    /* 镜像类型必须与目标区一致，避免把驱动镜像塞进 App 槽位 */
    if(hdr.type != SVCRT_APP_TYPE_APP)
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

    slot = svcrt_loader_find_slot();
    if(slot < 0)
    {
        return SVCRT_LOADER_ERR_NO_SLOT;
    }

    if(total > pt->app_slot_size)
    {
        return SVCRT_LOADER_ERR_SIZE;
    }

    slot_base = pt->app_user_base + (uint32)slot * pt->app_slot_size;

    /* 镜像头里的期望加载地址必须等于目标分区基址。
     * 打包工具会用该字段并在 --verify 时校验（tools/pack_app.py），
     * 内核此前不校验 → 按其它基址链接的镜像会被照单收下，启动后跑飞。 */
    if(hdr.load_addr != slot_base)
    {
        return SVCRT_LOADER_ERR_ADDR;
    }

    /* 进入安装态：写入未完成前不允许被启动；掉电中断则由 CRC 校验判为 INVALID */
    svcrt_ptable_set_slot((uint32)slot, SVCRT_APP_SLOT_INSTALLING, 0u, 0u);

    if(svcrt_port_flash_erase(slot_base, total) != 0)
    {
        svcrt_ptable_set_slot((uint32)slot, SVCRT_APP_SLOT_EMPTY, 0u, 0u);
        return SVCRT_LOADER_ERR_FLASH;
    }

    ret = svcrt_loader_stream_payload(dev, slot_base, &hdr);
    if(ret != 0)
    {
        svcrt_ptable_set_slot((uint32)slot, SVCRT_APP_SLOT_EMPTY, 0u, 0u);
        return ret;
    }

    if(svcrt_loader_image_crc((const uint8 *)slot_base, total) != hdr.crc32)
    {
        svcrt_ptable_set_slot((uint32)slot, SVCRT_APP_SLOT_EMPTY, 0u, 0u);
        return SVCRT_LOADER_ERR_CRC;
    }

    /* 新镜像写入成功：清零故障计数（重新安装 = 重新开始） */
    pt->slot_crash_cnt[slot] = 0u;

    svcrt_ptable_set_slot((uint32)slot, SVCRT_APP_SLOT_LOADED,
                          svcrt_loader_entry_addr(slot_base, hdr.entry_offset), 0u);

    return slot;
}

int32 svcrt_loader_load_driver_dev(int32 dev, const svcrt_app_header_t *p_hdr, uint32 image_len)
{
    svcrt_partition_table_t *pt = svcrt_ptable_get();
    svcrt_app_header_t hdr = *p_hdr;
    uint32 total;
    int32 ret;

    if(dev < 0)
    {
        return SVCRT_LOADER_ERR_PARAM;
    }

    if(hdr.type != SVCRT_APP_TYPE_DRIVER)
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

    if(hdr.load_addr != pt->driver_pool_base)
    {
        return SVCRT_LOADER_ERR_ADDR;
    }

    /* 驱动区是单入口，不做槽位分配；容量按整个 DRIVER_POOL 计 */
    if(total > pt->driver_pool_size)
    {
        return SVCRT_LOADER_ERR_SIZE;
    }

    /* 旧驱动还在运行：先踢出调度再擦除，否则擦除正在执行的代码区必然取指崩溃。
     * （若本次安装是由旧驱动自身经 SVC 发起，拒绝——SVC 返回后还会跳回已擦除的代码） */
    if(pt->driver_state == SVCRT_APP_SLOT_RUNNING)
    {
        if((int32)pt->driver_task_id == svcrt_current_task_id)
        {
            return SVCRT_LOADER_ERR_STATE;
        }

        svcrt_loader_halt_task(pt->driver_task_id);
    }

    pt->driver_state = SVCRT_APP_SLOT_INSTALLING;
    pt->driver_entry = 0u;

    if(svcrt_port_flash_erase(pt->driver_pool_base, total) != 0)
    {
        pt->driver_state = SVCRT_APP_SLOT_EMPTY;
        return SVCRT_LOADER_ERR_FLASH;
    }

    ret = svcrt_loader_stream_payload(dev, pt->driver_pool_base, &hdr);
    if(ret != 0)
    {
        pt->driver_state = SVCRT_APP_SLOT_EMPTY;
        return ret;
    }

    if(svcrt_loader_image_crc((const uint8 *)pt->driver_pool_base, total) != hdr.crc32)
    {
        pt->driver_state = SVCRT_APP_SLOT_EMPTY;
        return SVCRT_LOADER_ERR_CRC;
    }

    pt->driver_crash_cnt = 0u;
    pt->driver_entry     = svcrt_loader_entry_addr(pt->driver_pool_base, hdr.entry_offset);
    pt->driver_state     = SVCRT_APP_SLOT_LOADED;

    return 0;
}

int32 svcrt_loader_load_driver(int32 dev, uint32 image_len)
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

    return svcrt_loader_load_driver_dev(dev, &hdr, image_len);
}

/* 认定一段分区内的内容，得到一个可用入口。
 * 支持两种共存的格式：
 *   1) 安装路径：带 256 字节头 + CRC32 的 .svcapp，
 *      入口 = 分区基址 + 镜像头长度 + entry_offset（负载链接基址另有 +256，见 gen_scatter）；
 *   2) 开发路径：Keil 直接下载的裸镜像（无镜像头），入口 = 分区基址（ARM 置 Thumb 位）。
 *      开发期靠它保住“固定地址烧录 + MDK 下断点调试”的闭环；
 *      发布固件时应把 APP_ALLOW_RAW_IMAGE 关掉，只接受 .svcapp。
 * 返回 0=有效（*out_entry 为入口），-1=空或无效。 */
static int32 svcrt_loader_identify(uint32 region_base, uint32 region_size,
                                    uint32 expect_type, uint32 *out_entry)
{
    const svcrt_app_header_t *p_hdr = (const svcrt_app_header_t *)region_base;
    uint32 total;

    if(p_hdr->magic == SVCRT_APP_MAGIC)
    {
        if((p_hdr->hw_compat_id != SVCRT_HW_COMPAT_ID) ||
           (p_hdr->image_size == 0u) || (p_hdr->type != expect_type))
        {
            return -1;
        }

        /* 与 svcrt_loader_check_header 同口径：禁止 total 回绕，且入口必须落在负载内 */
        if(p_hdr->image_size > (0xFFFFFFFFu - SVCRT_APP_HEADER_SIZE))
        {
            return -1;
        }

        if(p_hdr->entry_offset >= p_hdr->image_size)
        {
            return -1;
        }

        total = SVCRT_APP_HEADER_SIZE + p_hdr->image_size;
        if(total > region_size)
        {
            return -1;
        }

        /* Flash 已映射，直接按地址复算 CRC */
        if(svcrt_loader_image_crc((const uint8 *)p_hdr, total) != p_hdr->crc32)
        {
            return -1;
        }

        *out_entry = svcrt_loader_entry_addr(region_base, p_hdr->entry_offset);
        return 0;
    }

    /* 非镜像头：擦除态（0xFFFFFFFF）或全零视为空 */
    if((p_hdr->magic == 0xFFFFFFFFu) || (p_hdr->magic == 0x00000000u))
    {
        return -1;
    }

    #if (APP_ALLOW_RAW_IMAGE == 1)
    *out_entry = region_base;
    #if (SVCRT_ARCH_IS_ARM == 1)
    *out_entry |= 1u;               /* Cortex-M：Thumb 指令集标志位 */
    #endif
    return 0;
    #else
    return -1;
    #endif
}

uint32 svcrt_loader_scan(void)
{
    svcrt_partition_table_t *pt = svcrt_ptable_get();
    uint32 found = 0u;
    uint32 i;

    for(i = 0u; i < pt->app_max_count && i < 8u; i++)
    {
        uint32 base = pt->app_user_base + i * pt->app_slot_size;
        uint32 entry;

        if(svcrt_loader_identify(base, pt->app_slot_size, SVCRT_APP_TYPE_APP, &entry) == 0)
        {
            svcrt_ptable_set_slot(i, SVCRT_APP_SLOT_LOADED, entry, 0u);
            found++;
        }
        else if(((const svcrt_app_header_t *)base)->magic != 0xFFFFFFFFu)
        {
            /* 有内容但认定失败：标记为不可用，避免被误启动 */
            svcrt_ptable_set_slot(i, SVCRT_APP_SLOT_INVALID, 0u, 0u);
        }
    }

    return found;
}

uint32 svcrt_loader_scan_driver(void)
{
    svcrt_partition_table_t *pt = svcrt_ptable_get();
    uint32 entry;

    if(svcrt_loader_identify(pt->driver_pool_base, pt->driver_pool_size, SVCRT_APP_TYPE_DRIVER, &entry) == 0)
    {
        pt->driver_entry = entry;
        pt->driver_state = SVCRT_APP_SLOT_LOADED;
        return 1u;
    }

    if(((const svcrt_app_header_t *)pt->driver_pool_base)->magic != 0xFFFFFFFFu)
    {
        pt->driver_entry = 0u;
        pt->driver_state = SVCRT_APP_SLOT_INVALID;
    }

    return 0u;
}

int32 svcrt_loader_start_driver(void)
{
    svcrt_partition_table_t *pt = svcrt_ptable_get();
    uint32 stack_bottom;
    int32 task_id;

    if(pt->driver_state != SVCRT_APP_SLOT_LOADED)
    {
        return SVCRT_LOADER_ERR_STATE;
    }

    if(pt->driver_entry == 0u)
    {
        return SVCRT_LOADER_ERR_STATE;
    }

    /* 与 App 同一套路：栈从驱动 RAM 区顶部切出，App 端 .sct 的 ARM_LIB_STACK
     * 指向同一地址（见 gen_scatter.py 的 driver target）。 */
    stack_bottom = (DRIVER_RAM_BASE + DRIVER_RAM_SIZE) - DRIVER_TASK_STACK_SIZE;

    task_id = svcrt_task_register((void (*)(void))pt->driver_entry,
                                  (uint32 *)stack_bottom,
                                  (uint32)DRIVER_TASK_STACK_SIZE,
                                  (uint8)DRIVER_TASK_PRIORITY,
                                  (uint32)DRIVER_TASK_PERIOD_MS);
    if(task_id <= 0)
    {
        return SVCRT_LOADER_ERR_TASK;
    }

    svcrt_task_table[task_id - 1].ram_start = DRIVER_RAM_BASE;
    svcrt_task_table[task_id - 1].ram_size  = DRIVER_RAM_SIZE;
    svcrt_task_table[task_id - 1].rom_start = pt->driver_pool_base;
    svcrt_task_table[task_id - 1].rom_size  = pt->driver_pool_size;

    pt->driver_task_id = (uint32)task_id;
    pt->driver_state   = SVCRT_APP_SLOT_RUNNING;

    svcrt_sched_activate_higher((uint8)DRIVER_TASK_PRIORITY);

    return task_id;
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

    if(pt->driver_task_id == (uint32)task_id)
    {
        pt->driver_crash_cnt++;

        #if (APP_CRASH_RESTART_MAX > 0)
        if(pt->driver_crash_cnt >= (uint32)APP_CRASH_RESTART_MAX)
        {
            SVCRT_DISABLE_IRQ();
            svcrt_task_table[task_id - 1].recover_pending = 0u;
            svcrt_task_table[task_id - 1].status          = SVCRT_TASK_INVALID;
            SVCRT_ENABLE_IRQ();

            pt->driver_state   = SVCRT_APP_SLOT_INVALID;
            pt->driver_task_id = 0u;
            svcrt_fault_record(SVCRT_FAULT_APPDISABLED, task_id);
            return 1;
        }
        #endif

        return 0;
    }

    return -1;          /* 不属于任何 App 槽位/驱动区（内核任务）：沿用默认处理 */
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
