/**
* @brief SVCrtOS 同步原语定义（内核内部头文件）
* @details 定义计数信号量与互斥锁的对象结构及内核内部接口。
*          信号量用于控制对有限资源的访问，互斥锁用于互斥保护并支持优先级继承。
*          二者都维护一个等待者列表，释放时优先唤醒优先级最高的等待任务。
*          应用一般通过 SDK 或 svcrt.h 使用，不直接包含本文件。
* @author xw
* @date 2026.05.30
*/

#ifndef __SVCRT_SYNC_H__
#define __SVCRT_SYNC_H__

#include "svcrt_def.h"
#include "svcrt_task.h"
#include "svcrt_config.h"

typedef struct {
    char   name[8];
    int32  count;                                       /* 计数值：>0 表示可用资源数，0 表示需排队等待 */
    uint8  used;                                        /* 该对象是否已被占用 */
    svcrt_task_t *waiters[SVCRT_MAX_SYNC_WAITERS];      /* 等待者列表 */
} svcrt_sem_obj_t;

typedef struct {
    char   name[8];
    svcrt_task_t *owner;                                /* 当前持有者，0 表示未上锁 */
    uint8  used;                                        /* 该对象是否已被占用 */
    uint8  orig_priority;                               /* 持有者原始优先级，用于优先级继承后恢复 */
    svcrt_task_t *waiters[SVCRT_MAX_SYNC_WAITERS];      /* 等待者列表 */
} svcrt_mtx_obj_t;

void  svcrt_sync_module_init(void);

int32 svcrt_sem_create_internal(char *name, int32 init_count);
int32 svcrt_sem_wait_internal(int32 handle, int32 timeout_ms);
int32 svcrt_sem_post_internal(int32 handle);
int32 svcrt_sem_delete_internal(int32 handle);

int32 svcrt_mtx_create_internal(char *name);
int32 svcrt_mtx_lock_internal(int32 handle, int32 timeout_ms);
int32 svcrt_mtx_unlock_internal(int32 handle);
int32 svcrt_mtx_delete_internal(int32 handle);

/* 任务下线收尸：把任务从所有信号量/互斥锁的等待队列摘除，
 * 并把它持有的互斥锁移交给优先级最高的等待者（无等待者则释放）。
 * 调用方需自行保证临界区；一般经 svcrt_task_release_resources() 调用。 */
void  svcrt_sync_release_task(int32 task_id);

#endif
