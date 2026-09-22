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
    int32  creator_id;                                  /* creator task id; 0 = kernel. Dropped together with its creator's image */
    svcrt_task_t *waiters[SVCRT_MAX_SYNC_WAITERS];      /* 等待者列表 */
} svcrt_sem_obj_t;

typedef struct {
    char   name[8];
    svcrt_task_t *owner;                                /* 当前持有者，0 表示未上锁 */
    uint8  used;                                        /* 该对象是否已被占用 */
    uint8  orig_priority;                               /* 持有者原始优先级，用于优先级继承后恢复 */
    int32  creator_id;                                  /* creator task id; 0 = kernel. Dropped together with its creator's image */
    svcrt_task_t *waiters[SVCRT_MAX_SYNC_WAITERS];      /* 等待者列表 */
} svcrt_mtx_obj_t;

typedef struct {
    char   name[8];
    uint8  used;                                        /* 该对象是否已被占用 */
    int32  creator_id;                                  /* creator task id; 0 = kernel */
    svcrt_task_t *waiters[SVCRT_MAX_SYNC_WAITERS];      /* 等待者列表（signal 按优先级挑一个） */
} svcrt_cond_obj_t;

void  svcrt_sync_module_init(void);

int32 svcrt_sem_create_internal(char *name, int32 init_count);
int32 svcrt_sem_wait_internal(int32 handle, int32 timeout_ms);
int32 svcrt_sem_post_internal(int32 handle);
int32 svcrt_sem_delete_internal(int32 handle);

int32 svcrt_mtx_create_internal(char *name);
int32 svcrt_mtx_lock_internal(int32 handle, int32 timeout_ms);
int32 svcrt_mtx_unlock_internal(int32 handle);
int32 svcrt_mtx_delete_internal(int32 handle);

int32 svcrt_cond_create_internal(char *name);
/* timeout_ms == (uint32)-1 表示无限等待；返回值见 SVCRT_SYNC_ERR_x。
 * 约定：调用者必须持有 mtx；函数返回时（含超时）一定重新持有它。 */
int32 svcrt_cond_wait_internal(int32 cond_handle, int32 mtx_handle, int32 timeout_ms);
int32 svcrt_cond_signal_internal(int32 handle);
int32 svcrt_cond_broadcast_internal(int32 handle);
int32 svcrt_cond_delete_internal(int32 handle);
/* User-mode condition variable, three steps: enqueue (idempotent) -> poll
 * ("am I still queued?") -> abort.  Never blocks on the kernel side. */
int32 svcrt_cond_enqueue_internal(int32 cond_handle);
int32 svcrt_cond_poll_internal(int32 cond_handle);
int32 svcrt_cond_abort_internal(int32 cond_handle);

/* 任务下线收尸：把任务从所有信号量/互斥锁的等待队列摘除，
 * 并把它持有的互斥锁移交给优先级最高的等待者（无等待者则释放）。
 * 调用方需自行保证临界区；一般经 svcrt_task_release_resources() 调用。 */
void  svcrt_sync_release_task(int32 task_id);

/* Diagnostics for the token-conservation rules (defined in svcrt_sync.c). */
extern volatile uint32 svcrt_diag_sync_ghost;
extern volatile uint32 svcrt_diag_sync_dup;
extern volatile uint32 dbg_sem_ghost_cnt;
extern volatile uint32 dbg_sem_ghost_idx;
extern volatile uint32 dbg_sem_ghost_tid;
extern volatile uint32 dbg_sem_to_cnt;
extern volatile uint32 dbg_sem_appq_hit;   /* user-mode registrations */
/* Read-only debug view of one table row.  Filled by the getters below; the
 * shell does the formatting so this header stays free of printing.  waiter_id
 * is the 1 based task id. */
typedef struct
{
    uint8  used;
    int32  creator_id;
    int32  owner_id;                     /* mutex only, 0 = free            */
    int32  count;                        /* semaphore tokens, 0 otherwise   */
    int32  nwait;                        /* registered waiters              */
    uint32 waiter_id[SVCRT_MAX_SYNC_WAITERS];
} svcrt_sync_dbg_row_t;

/* Copies row idx into out.  Returns 0 on success, -1 when idx is out of
 * range for that table.  Read only, so it never disturbs the objects. */
int32 svcrt_sync_sem_debug(int32 idx, svcrt_sync_dbg_row_t *out);
/* Event ledger for one semaphore: posts actually delivered to it, tokens
 * taken by the count>0 fast path, and registrations in its waiter queue
 * with the last registering task id.  Read only; the shell reports it
 * next to the object table. */
void svcrt_sync_sem_trace(int32 idx, uint32 *posts, uint32 *takes,
                          uint32 *queued, uint32 *last_waiter);
/* Hand off details of the last post that woke somebody on this object:
 * the task id it woke and the reason bits it wrote.  A joiner that never
 * comes back needs the receiver side proved too, not just the sender. */
void svcrt_sync_sem_handoff(int32 idx, uint32 *woke, uint32 *reason,
                            uint32 *pendsv_also);
/* How many waiters were sitting in this object's queue at the moment of
 * the last post, sampled just before it popped one.  Together with posts
 * and woke it separates 'the token went to the count because nobody was
 * queued' from 'somebody was queued and the pop still lost him'. */
uint32 svcrt_sync_sem_qbefore(int32 idx);
int32 svcrt_sync_mtx_debug(int32 idx, svcrt_sync_dbg_row_t *out);
int32 svcrt_sync_cond_debug(int32 idx, svcrt_sync_dbg_row_t *out);
uint32 svcrt_sync_sem_num(void);
uint32 svcrt_sync_mtx_num(void);
uint32 svcrt_sync_cond_num(void);


#endif
