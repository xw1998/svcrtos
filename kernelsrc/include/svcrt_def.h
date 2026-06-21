/**
* @brief SVCrtOS 内核公共常量定义
* @details 集中定义句柄标志位、句柄掩码以及各类 SVC 调用号。
*          句柄高位用标志位区分对象类型（事件/设备/信号量/互斥锁），低位为对象索引。
* @note 这些常量在内核与 SDK 间共享，修改时需保证两侧一致。
*/

#ifndef __SVCRT_DEF_H__
#define __SVCRT_DEF_H__

#include "svcrt_types.h"

#define SVCRT_EVENT_HANDLE_FLAG     (0x01100000)
#define SVCRT_DEV_HANDLE_FLAG       (0x01200000)
#define SVCRT_HANDLE_MASK           (0xfff00000)
#define SVCRT_HANDLE_RELMASK        (0x000fffff)

#define SVCRT_SVC_DEV_IO            (0x10)
#define SVCRT_SVC_TASK_CTRL         (0x11)
#define SVCRT_SVC_SYS_INFO          (0x12)
#define SVCRT_SVC_EVENT_CTRL        (0x13)
#define SVCRT_SVC_DRV_MGR           (0x14)
#define SVCRT_SVC_SYNC_CTRL         (0x15)

#define SVCRT_SEM_HANDLE_FLAG       (0x01300000)
#define SVCRT_MTX_HANDLE_FLAG       (0x01400000)

#endif
