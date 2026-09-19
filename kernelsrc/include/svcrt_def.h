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
#define SVCRT_SVC_MQ_CTRL           (0x16)
#define SVCRT_SVC_TIMER_CTRL        (0x17)
#define SVCRT_SVC_LOG               (0x19)
#define SVCRT_SVC_SHELL             (0x1A)
#define SVCRT_SVC_APP_MGR           (0x18)
/* User thread service: create / self / exit. Backs the POSIX
 * pthread facade in the App SDK; a thread needs a TCB and a scheduler slot,
 * which only the kernel owns, so this is the one place user code can ask for
 * a new execution context. */
#define SVCRT_SVC_THREAD_CTRL       (0x1B)   /* App 镜像管理与分区查询 */

/* 同步原语（信号量/互斥锁）返回码：0=成功，负值=失败。
 * 超时必须返回负值——调用方据此判断“本次没有拿到资源”；
 * 若与 0（成功）混为一谈，会出现“以为拿到了信号量、计数却没减”的错乱。 */
#define SVCRT_SYNC_OK             (0)
#define SVCRT_SYNC_ERR_PARAM      (-1)
#define SVCRT_SYNC_ERR_TIMEOUT    (-2)
#define SVCRT_SYNC_ERR_DELETED    (-3)   /* 等待的同步对象/消息队列已被删除 */

/* 任务唤醒原因（task->wake_reason）：等待类接口据此判断“是否真的拿到了资源”。
 * 对象被删除时等待者必须收到 2，否则会一直睡下去（永久泄漏）。 */
#define SVCRT_WAKE_NORMAL         (0)    /* 被显式唤醒：已获得资源/事件 */
#define SVCRT_WAKE_TIMEOUT        (1)    /* 等待超时 */
#define SVCRT_WAKE_OBJ_DELETED    (2)    /* 等待的对象已被删除 */

#define SVCRT_SEM_HANDLE_FLAG       (0x01300000)
#define SVCRT_MTX_HANDLE_FLAG       (0x01400000)
#define SVCRT_MQ_HANDLE_FLAG        (0x01500000)
#define SVCRT_TIMER_HANDLE_FLAG     (0x01600000)

#endif
