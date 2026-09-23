/**
* @file svcrt_kernel_check.h
* @brief Kernel side checks on what a caller handed over through an SVC.
* @details These four helpers are the only place that decides whether a
*          pointer that arrived from user mode may be dereferenced by the
*          kernel. They used to be file static inside svcrt_task.c; a second
*          kernel service module (svcrt_net.c) needs the same answers, and a
*          second copy of this logic is exactly the kind of thing that drifts
*          apart and stops guarding.
*
*          Rules of use:
*            - svcrt_kernel_svc_args_ok   the p[] argument block itself
*            - svcrt_kernel_dev_buf_ok    memory the kernel WRITES into
*            - svcrt_kernel_user_ro_ok    memory the kernel only READS
*            - svcrt_kernel_user_name_ok  a NUL terminated name / path
*          Every check answers 1 = usable, 0 = refuse. A refusal is not an
*          error to be worked around: the caller gets a negative return.
*
* @note  Kernel internal. Apps and drivers must not include this file.
* @author xw
*/
#ifndef __SVCRT_KERNEL_CHECK_H__
#define __SVCRT_KERNEL_CHECK_H__

#include "svcrt_types.h"

/* The p[] block of a multi argument SVC: sub command plus its arguments. */
uint8 svcrt_kernel_svc_args_ok(const void *p, uint32 len);

/* A buffer the kernel writes into (dev_read, socket recv, ...). */
uint8 svcrt_kernel_dev_buf_ok(const void *p, int32 len);

/* A buffer the kernel only reads (log text, socket send, ...). */
uint8 svcrt_kernel_user_ro_ok(const void *p, uint32 len);

/* A name / path: read up to len bytes including the NUL, may live in the
 * caller's own firmware window (string literals). */
uint8 svcrt_kernel_user_name_ok(const void *p, uint32 len);

#endif /* __SVCRT_KERNEL_CHECK_H__ */
