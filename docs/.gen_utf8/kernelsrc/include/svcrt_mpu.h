/**
* @brief SVCrtOS 内存保护单元管理接口（内核内部）
* @details 提供MPU区域配置和管理的内核侧接口。
*          实际的MPU寄存器操作由port层的 svcrt_port_mpu_* 系列函数实现，
*          内核不直接操作任何MPU寄存器。
*          此文件仅供内核内部使用。
*/

#ifndef __SVCRT_MPU_H__
#define __SVCRT_MPU_H__

#include "svcrt_def.h"
#include "svcrt_hal.h"
#include "svcrt_task.h"
#include "svcrt_config.h"

#if (SVCRT_USE_MPU == 1)

void svcrt_mpu_module_init(void);
void svcrt_mpu_set(uint32 rom_addr, uint32 rom_size, uint32 ram_addr, uint32 ram_size);
void svcrt_mpu_set_app(svcrt_task_t *p_app);
void svcrt_mpu_reset(void);

#else

#define svcrt_mpu_module_init()
#define svcrt_mpu_set(rom_addr, rom_size, ram_addr, ram_size)
#define svcrt_mpu_set_app(p_app)
#define svcrt_mpu_reset()

#endif

#endif
