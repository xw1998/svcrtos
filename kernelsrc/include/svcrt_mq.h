/**
* @brief SVCrtOS 消息队列模块（内核内部头文件）
* @details 提供任务间数据传递原语：变长消息队列（拷贝语义）。
*          队列深度由 SVCRT_MQ_DEPTH 约定，单条消息长度上限为
*          SVCRT_MQ_MSG_WORDS 个字；每条消息实际占几个字由发送方声明并随消息记录，
*          接收方按自己的缓冲容量取，recv 返回实际取到的字数。
*          支持阻塞 send/recv（带超时）与中断上下文安全的 send_from_isr。
*          应用程序应通过 svcrt.h 使用，不要直接包含本文件。
*/

#ifndef __SVCRT_MQ_H__
#define __SVCRT_MQ_H__

#include "svcrt_def.h"
#include "svcrt_task.h"
#include "svcrt_config.h"

#if (SVCRT_USE_MQ == 1)

typedef struct {
    char   name[8];
    uint8  used;                                        /* 槽位是否占用 */
    int32  creator_id;                                  /* creator task id; 0 = kernel. Dropped together with its creator's image */
    uint32 head;                                        /* 读位置 */
    uint32 tail;                                        /* 写位置 */
    uint32 count;                                       /* 当前消息条数 */
    svcrt_task_t *send_waiters[SVCRT_MAX_SYNC_WAITERS]; /* 队列满时阻塞的发送者 */
    svcrt_task_t *recv_waiters[SVCRT_MAX_SYNC_WAITERS]; /* 队列空时阻塞的接收者 */
    uint32 buf[SVCRT_MQ_DEPTH * SVCRT_MQ_MSG_WORDS];    /* 消息存储区（定长槽） */
    uint8  msglen[SVCRT_MQ_DEPTH];                      /* 每个槽里消息实际占的字数 */
} svcrt_mq_obj_t;

void  svcrt_mq_module_init(void);

int32 svcrt_mq_create_internal(char *name);

/* Kernel owned queue: the creator id is pinned to 0, so
 * svcrt_mq_release_task() never collects it when some task goes away.
 * Use it for queues the kernel itself owns and that must outlive every
 * task (the socket request queue is one). */
int32 svcrt_mq_create_kernel_internal(char *name);

/* 1 = the handle still names a live queue object, not merely an in-range
 * index. A stored handle keeps passing the plain range checks after the
 * object behind it is gone, so this is the probe that tells the truth. */
int32 svcrt_mq_is_alive(int32 handle);
int32 svcrt_mq_send_internal(int32 handle, uint32 *buf, int32 len_words, int32 timeout_ms);
int32 svcrt_mq_recv_internal(int32 handle, uint32 *buf, int32 len_words, int32 timeout_ms);
int32 svcrt_mq_delete_internal(int32 handle);

int32 svcrt_mq_send_from_isr_internal(int32 handle, void *buf, int32 len_words);

/* 任务下线收尸：把任务从所有消息队列的收发等待队列摘除。
 * 调用方需自行保证临界区；一般经 svcrt_task_release_resources() 调用。 */
void  svcrt_mq_release_task(int32 task_id);

#endif /* SVCRT_USE_MQ */

#endif
