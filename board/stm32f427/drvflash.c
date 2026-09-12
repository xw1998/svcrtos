/**
* @file drvflash.c
* @brief STM32F427 片内 Flash 擦写驱动实现（HAL 库）
* @details 支持 1MB / 2MB 两种扇区划分：
*          - 扇区 0~3  ：16KB
*          - 扇区 4    ：64KB
*          - 扇区 5~11 ：128KB（2MB 器件继续延伸到扇区 23）
*
*          擦除以扇区为单位（自动扩展到覆盖整个区间），编程按字节进行。
*          地址不写死在本文件：调用方（Loader）的地址来自 config/svcrt_partition.h。
*/

#include "drvflash.h"
#include "svcrt_config.h"
#include "svcrt_partition.h"    /* 全工程唯一地址源头：CHIP_FLASH_BASE */

#ifdef SVCRT_BOARD_CONFIG
#include SVCRT_BOARD_CONFIG      /* 引入 stm32f4xx.h → HAL 头文件 */
#endif

#define SVCRT_FLASH_BASE_ADDR     ((uint32)CHIP_FLASH_BASE)
#define SVCRT_FLASH_SMALL_SECTOR  (16u * 1024u)     /* 扇区 0~3       */
#define SVCRT_FLASH_MID_SECTOR    (64u * 1024u)     /* 扇区 4         */
#define SVCRT_FLASH_BIG_SECTOR    (128u * 1024u)    /* 扇区 5~11/23   */
#define SVCRT_FLASH_TOTAL_SIZE    ((uint32)CHIP_FLASH_SIZE)  /* 片内 Flash 总容量 */

/**
* @brief 计算地址所在的扇区号
* @param addr 片内 Flash 地址
* @return 扇区号（0~23），地址非法返回 0xFFFFFFFF
*/
static uint32 svcrt_flash_sector_index(uint32 addr)
{
    uint32 offset;

    if(addr < SVCRT_FLASH_BASE_ADDR)
    {
        return 0xFFFFFFFFu;
    }

    offset = addr - SVCRT_FLASH_BASE_ADDR;

    /* 上界：超出芯片 Flash 容量的地址一律判非法。
     * 不挡的话下面按公式会算出 5~23 的“合法”扇区号（1MB 芯片只到 11），
     * 越界扇区被交给 HAL 去擦。 */
    if(offset >= SVCRT_FLASH_TOTAL_SIZE)
    {
        return 0xFFFFFFFFu;
    }

    if(offset < (4u * SVCRT_FLASH_SMALL_SECTOR))
    {
        return offset / SVCRT_FLASH_SMALL_SECTOR;               /* 0~3  */
    }
    if(offset < (4u * SVCRT_FLASH_SMALL_SECTOR + SVCRT_FLASH_MID_SECTOR))
    {
        return 4u;                                             /* 4    */
    }

    return 5u + (offset - 4u * SVCRT_FLASH_SMALL_SECTOR - SVCRT_FLASH_MID_SECTOR)
                / SVCRT_FLASH_BIG_SECTOR;                      /* 5~23 */
}

uint32 svcrt_port_flash_sector_size(uint32 addr)
{
    uint32 idx = svcrt_flash_sector_index(addr);

    switch(idx)
    {
    case 0xFFFFFFFFu:
        return 0u;
    case 0u:
    case 1u:
    case 2u:
    case 3u:
        return SVCRT_FLASH_SMALL_SECTOR;
    case 4u:
        return SVCRT_FLASH_MID_SECTOR;
    default:
        return SVCRT_FLASH_BIG_SECTOR;
    }
}

int32 svcrt_port_flash_erase(uint32 addr, uint32 size)
{
    FLASH_EraseInitTypeDef erase_init;
    uint32 first_sector;
    uint32 last_sector;
    uint32_t sector_error = 0;   /* HAL 接口要求 uint32_t（与内核 uint32 是不同类型） */
    uint32 end;

    if(size == 0u)
    {
        return 0;
    }

    end = addr + size - 1u;

    first_sector = svcrt_flash_sector_index(addr);
    last_sector  = svcrt_flash_sector_index(end);

    if((first_sector == 0xFFFFFFFFu) || (last_sector == 0xFFFFFFFFu))
    {
        return -1;
    }

    HAL_FLASH_Unlock();

    erase_init.TypeErase    = FLASH_TYPEERASE_SECTORS;
    erase_init.VoltageRange = FLASH_VOLTAGE_RANGE_3;    /* 2.7V ~ 3.6V */
    erase_init.Sector       = first_sector;
    erase_init.NbSectors    = (last_sector - first_sector) + 1u;

    if(HAL_FLASHEx_Erase(&erase_init, &sector_error) != HAL_OK)
    {
        HAL_FLASH_Lock();
        return -1;
    }

    HAL_FLASH_Lock();
    return 0;
}

int32 svcrt_port_flash_write(uint32 addr, const uint8 *data, uint32 len)
{
    uint32 i;
    int32  ret = 0;

    if((data == 0) || (len == 0u))
    {
        return 0;
    }

    HAL_FLASH_Unlock();

    for(i = 0u; i < len; i++)
    {
        /* 按字节编程：写入前必须已擦除，否则 HAL 返回错误 */
        if(HAL_FLASH_Program(FLASH_TYPEPROGRAM_BYTE, addr + i,
                             (uint64_t)data[i]) != HAL_OK)
        {
            ret = -1;
            break;
        }
    }

    HAL_FLASH_Lock();
    return ret;
}
