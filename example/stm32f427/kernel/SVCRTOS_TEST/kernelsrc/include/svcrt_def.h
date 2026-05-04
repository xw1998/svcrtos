/**
* @brief SVCrtOS 内核公共定义
* @details 内核内部使用的公共宏和常量定义
*          此文件仅供内核内部使用，应用程序不应包含此文件
*/

#ifndef __SVCRT_DEF_H__
#define __SVCRT_DEF_H__

#include "svcrt_port.h"

#define SVCRT_EVENT_HANDLE_FLAG     (0x01100000)
#define SVCRT_DEV_HANDLE_FLAG       (0x01200000)
#define SVCRT_HANDLE_MASK           (0xfff00000)
#define SVCRT_HANDLE_RELMASK        (0x000fffff)

#define SVCRT_SVC_DEV_IO            (0x10)
#define SVCRT_SVC_TASK_CTRL         (0x11)
#define SVCRT_SVC_SYS_INFO          (0x12)
#define SVCRT_SVC_EVENT_CTRL        (0x13)

#endif
