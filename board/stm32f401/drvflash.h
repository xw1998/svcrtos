/**
* @file drvflash.h
* @brief STM32F401 片内 Flash 擦写驱动（板级）
* @details 为内核 Loader 提供 Flash 擦除/编程能力。接口契约定义在
*          kernelsrc/include/svcrt_hal.h 中（svcrt_port_flash_*），
*          本文件仅声明板级实现，供板级代码直接调用。
*
* @note 擦写期间 CPU 访问 Flash 会被硬件阻塞，系统 tick 可能延迟若干毫秒，
*       属于正常现象；擦除/编程过程中不得断电。
*/

#ifndef __DRVFLASH_H__
#define __DRVFLASH_H__

#include "svcrt_hal.h"

/**
* @brief 擦除 Flash 区间（按扇区擦除，自动对齐到扇区边界）
* @param addr 起始地址（必须位于片内 Flash）
* @param size 字节长度
* @return 0=成功，-1=失败
*/
int32 svcrt_port_flash_erase(uint32 addr, uint32 size);

/**
* @brief 写入 Flash（按字节编程，写入前该区间必须已擦除）
* @param addr 起始地址
* @param data 数据指针
* @param len  字节长度
* @return 0=成功，-1=失败
*/
int32 svcrt_port_flash_write(uint32 addr, const uint8 *data, uint32 len);

/**
* @brief 获取指定地址所在扇区的大小（字节）
* @param addr Flash 地址
* @return 扇区大小；地址非法返回 0
*/
uint32 svcrt_port_flash_sector_size(uint32 addr);

#endif /* __DRVFLASH_H__ */
