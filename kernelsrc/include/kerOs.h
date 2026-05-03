/**
* @brief SVCrtOS 操作系统基础类型定义
* @author chenjl
* @date 2019.07.01
*/

#ifndef __KEROS_H__
#define __KEROS_H__

#include "svcrt_port.h"

#define EVENT_HANDLE_FLAG       (0x01100000)
#define DEV_HANDLE_FLAG         (0x01200000)
#define HANDLE_MASK             (0xfff00000)
#define HANDLE_RELMASK          (0x000fffff)

#endif
