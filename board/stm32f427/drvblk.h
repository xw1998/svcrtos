/**
* @file drvblk.h
* @brief STM32F427 board level block device registration
* @details Registers the two non-volatile backends this board has:
*
*            int0 - the internal flash image pool region (see
*                   config/svcrt_partition.h). Memory mapped, erased in
*                   128 KiB sectors.
*            nor0 - the external SPI NOR, U4 = W25Q128 (16 MiB). Not memory
*                   mapped, erased in 4 KiB sectors.
*
*          Neither descriptor touches hardware at registration time: bring-up
*          is deferred to the first svcrt_blk_init() call, so a build that
*          never opens a device never pays for it and a board whose NOR is
*          absent does not dead-start the SPI bus.
*
* @author xw
* @date 2026.09.21
*/

#ifndef __DRVBLK_H__
#define __DRVBLK_H__

#include "svcrt_types.h"

/**
* @brief Register the board backends with the kernel block device registry
* @return 0 = both registered, -1 = at least one registration was refused
* @note  Called once from svcrt_dev_board_init(); no hardware is touched.
*/
int32 svcrt_board_blk_init(void);

#endif /* __DRVBLK_H__ */
