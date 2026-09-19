/**
* @file mqueue.h
* @brief POSIX message queues for SVCrtOS Apps.
* @details Backed by the kernel message queue service (SVC 0x16). Queues are
*          named and shared, so mq_open of an existing name simply returns the
*          handle; there is no filesystem to hang the name off.
*          Message length is in WORDS, matching the kernel contract, because
*          a word is the unit the kernel copies - saying "bytes" here would
*          be a quieter, and wrong, promise.
* @author xw
*/
#ifndef __SVCRT_MQUEUE_H__
#define __SVCRT_MQUEUE_H__

#include "svcrt_posix_types.h"

#define MQ_FAILED  (-1)

typedef int32 svcrt_mqd_t;

svcrt_mqd_t svcrt_posix_mq_open(const char *name, int oflag);
int  svcrt_posix_mq_close(svcrt_mqd_t mqdes);
int  svcrt_posix_mq_unlink(const char *name);
int  svcrt_posix_mq_send(svcrt_mqd_t mqdes, const void *msg, uint32 len_words);
int  svcrt_posix_mq_receive(svcrt_mqd_t mqdes, void *msg, uint32 len_words, uint32 timeout_ms);

#define mq_open(n, f)              svcrt_posix_mq_open((n), (f))
#define mq_close(q)                svcrt_posix_mq_close(q)
#define mq_unlink(n)               svcrt_posix_mq_unlink(n)
#define mq_send(q, m, l)           svcrt_posix_mq_send((q), (m), (l))
#define mq_receive(q, m, l, t)     svcrt_posix_mq_receive((q), (m), (l), (t))

#endif /* __SVCRT_MQUEUE_H__ */
