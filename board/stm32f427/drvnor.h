/**
* @file drvnor.h
* @brief STM32F427 onboard SPI NOR flash (W25Q128, 16 MiB) - board level
* @details Wiring taken from the board schematic
*          (example/stm32f427/开发板资料/原理图.pdf, U4 = W25Q128, SPI FLASH block):
*
*              U4 pin 1  CS        -> PE15   (plain GPIO, no external pull-up)
*              U4 pin 2  DO (D1)   -> PB14   SPI2_MISO
*              U4 pin 3  WP# (D2)  -> 3V3    (write protect disabled)
*              U4 pin 4  GND       -> GND
*              U4 pin 5  DI (D0)   -> PB15   SPI2_MOSI
*              U4 pin 6  CLK       -> PB10   SPI2_SCK
*              U4 pin 7  HOLD# (D3)-> 3V3    (hold disabled)
*              U4 pin 8  VCC       -> 3V3    (C23 = 100 nF decoupling)
*
*          SPI2 sits on APB1. This board runs SYSCLK = 96 MHz from the HSI PLL,
*          APB1 = HCLK/4 = 24 MHz, so SCK = 24 MHz / prescaler.
*
* @note The STM32F427 has no QUADSPI and Cortex-M4 cannot execute code from an
*       external SPI device, so this chip is a *storage* backend only: images and
*       file system blocks live here and are copied into internal flash / RAM to
*       run. It is never used as XIP memory.
*
* @note All calls are blocking and must run in task context (they rely on the
*       HAL tick for timeouts, which does not advance inside a critical section).
*
* @author xw
* @date 2026.09.21
*/

#ifndef __DRVNOR_H__
#define __DRVNOR_H__

#include "svcrt_types.h"

/** @brief Erase granularity of this part, in bytes (4 KiB sectors) */
#define SVCRT_NOR_SECTOR_BYTES    (4096u)

/** @brief Winbond JEDEC id read back on a healthy W25Q128 (0xEF / 0x40 / 0x18) */
#define SVCRT_NOR_JEDEC_W25Q128   (0xEF4018u)

/**
* @brief Bring up SPI2 + CS GPIO and identify the chip
* @return 0 = chip answered with a recognized JEDEC id, -1 = no/bogus chip
* @note Safe to call more than once; the second call only re-reads the id.
*/
int32 svcrt_nor_init(void);

/**
* @brief Was the chip identified successfully by svcrt_nor_init()?
* @return 1 = present, 0 = not present (all other calls then fail fast)
*/
uint8 svcrt_nor_present(void);

/** @brief Last JEDEC id read (0 when absent) */
uint32 svcrt_nor_jedec_id(void);

/** @brief Capacity in bytes derived from the JEDEC id (0 when absent) */
uint32 svcrt_nor_capacity(void);

/** @brief Erase granularity in bytes (always 4096 for this part) */
uint32 svcrt_nor_sector_size(void);

/**
* @brief Read len bytes (no alignment requirement)
* @return 0 = success, -1 = not present / len == 0 / range out of device
*/
int32 svcrt_nor_read(uint32 addr, uint8 *buf, uint32 len);

/**
* @brief Write len bytes, splitting on 256-byte pages as needed
* @details The target range must already be erased: NOR programming can only
*          clear bits, so writing over a non-0xFF byte silently ANDs the data.
*          This is the caller's contract, same as any raw flash API.
* @return 0 = success, -1 = failure (not present, out of range, timeout)
*/
int32 svcrt_nor_write(uint32 addr, const uint8 *data, uint32 len);

/** @brief Erase the 4 KiB sector containing addr. @return 0 = success */
int32 svcrt_nor_erase_sector(uint32 addr);

/**
* @brief Erase every 4 KiB sector overlapping [addr, addr + size)
* @return 0 = success, -1 = failure
*/
int32 svcrt_nor_erase(uint32 addr, uint32 size);

/** @brief Busy-wait until WIP clears. @return 0 = idle, -1 = timeout */
int32 svcrt_nor_wait_ready(uint32 timeout_ms);

#endif /* __DRVNOR_H__ */
