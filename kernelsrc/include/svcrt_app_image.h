/**
* @file svcrt_app_image.h
* @brief SVCrtOS App 镜像文件格式（打包工具与 Loader 共用）
* @details App 镜像 = 固定 256 字节头 + 代码/数据段原始内容。
*          镜像由 App 工程编译产物转换而来（见 tools/pack_app.py），
*          Loader 按本结构解析、校验并写入 App 分区。
* @note 本文件不含任何地址信息，App 与内核均可包含。
*/

#ifndef __SVCRT_APP_IMAGE_H__
#define __SVCRT_APP_IMAGE_H__

#include "svcrt_types.h"

/** @brief App 镜像头魔数 "SVCA" */
#define SVCRT_APP_MAGIC           (0x53564341u)

/** @brief App 镜像头固定长度（字节） */
#define SVCRT_APP_HEADER_SIZE     (256u)

/** @brief 镜像类型 */
#define SVCRT_APP_TYPE_APP        (1u)
#define SVCRT_APP_TYPE_DRIVER     (2u)

/**
* @brief App 镜像头（固定 256 字节）
* @note crc32 覆盖范围：本头结构体（crc32 字段自身置 0）+ 头之后的镜像数据。
*/
typedef struct {
    uint32 magic;               /* SVCRT_APP_MAGIC */
    uint32 type;                /* SVCRT_APP_TYPE_x */
    uint32 hw_compat_id;        /* 与内核的 SVCRT_HW_COMPAT_ID 比对 */
    uint32 version;             /* 镜像版本号（供后续升级/回滚使用） */

    uint32 image_size;          /* 头之后的有效镜像长度（字节） */
    uint32 entry_offset;        /* 入口函数相对镜像起始的偏移（通常为 0x00000008） */
    uint32 load_addr;           /* 期望加载地址（最小闭环下等于槽位基址） */
    uint32 crc32;               /* 镜像校验值 */

    uint8  signature[64];       /* 预留：数字签名 */
    uint8  reserved[192];       /* 预留：填充至 256 字节 */
} svcrt_app_header_t;

#if (defined(__cplusplus))
extern "C" {
#endif

/**
* @brief 计算 CRC32（与打包工具使用同一算法：IEEE 802.3 反射多项式）
* @param data 数据指针
* @param len  数据长度（字节）
* @param crc  前一段的 CRC 值（首段传 0）
* @return 累积的 CRC32 值
*/
uint32 svcrt_crc32(const void *data, uint32 len, uint32 crc);

#if (defined(__cplusplus))
}
#endif

#endif /* __SVCRT_APP_IMAGE_H__ */
