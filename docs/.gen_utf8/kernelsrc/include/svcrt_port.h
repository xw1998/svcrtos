/**
* @brief SVCrtOS 硬件移植接口（兼容层）
* @details 此文件为兼容层头文件，所有硬件抽象接口
*          已迁移至 svcrt_hal.h，内核代码统一使用 svcrt_hal.h。
*          保留此文件仅为向后兼容，新代码请直接包含 svcrt_hal.h。
*
*          移植到新MCU时，只需在 port/ 目录下实现 svcrt_hal.h 中
*          声明的所有函数，内核代码无需任何修改。
*
* @note 此文件不包含任何芯片头文件，不操作任何寄存器
*/

#ifndef __SVCRT_PORT_H__
#define __SVCRT_PORT_H__

#include "svcrt_hal.h"

#endif
