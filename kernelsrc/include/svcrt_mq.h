/**
* @brief SVCrtOS 消息队列模块（内核内部头文件）
* @details 提供任务间数据传递原语：定长消息队列（拷贝语义）。
*          每个队列容量与单条消息长度由 SVCRT_MQ_DEPTH / SVCRT_MQ_MSG_WORDS 统一约定。
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
    uint32 head;                                        /* 读位置 */
    uint32 tail;                                        /* 写位置 */
    uint32 count;                                       /* 当前消息条数 */
    svcrt_task_t *send_waiters[SVCRT_MAX_SYNC_WAITERS]; /* 队列满时阻塞的发送者 */
    svcrt_task_t *recv_waiters[SVCRT_MAX_SYNC_WAITERS]; /* 队列空时阻塞的接收者 */
    uint32 buf[SVCRT_MQ_DEPTH * SVCRT_MQ_MSG_WORDS];    /* 消息存储区 */
} svcrt_mq_obj_t;

void  svcrt_mq_module_init(void);

int32 svcrt_mq_create_internal(char *name);
int32 svcrt_mq_send_internal(int32 handle, uint32 *buf, int32 len_words, int32 timeout_ms);
int32 svcrt_mq_recv_internal(int32 handle, uint32 *buf, int32 len_words, int32 timeout_ms);
int32 svcrt_mq_delete_internal(int32 handle);

int32 svcrt_mq_send_from_isr_internal(int32 handle, void *buf, int32 len_words);

/* ����������ʬ���������������Ϣ���е��շ��ȴ�����ժ����
 * ���÷������б�֤�ٽ�����һ�㾭 svcrt_task_release_resources() ���á� */
void  svcrt_mq_release_task(int32 task_id);

#endif /* SVCRT_USE_MQ */

#endif
