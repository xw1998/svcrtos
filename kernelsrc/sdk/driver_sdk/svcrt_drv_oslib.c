/**
* @brief SVCrtOS Driver SDK - 用户态驱动OS接口封装
* @details 用户态驱动通过SVC指令调用内核服务
*          提供任务等待、事件操作等API
*          仅在 SVCRT_DRV_USER_MODE 定义时使用
*/

#include "svcrt_driver_sdk.h"
#include "svcrt_ulog.h"
#include "svcrt_svc_call.h"

SVCRT_SVC_DECL_1(int32, 0x10, svcrt_call_dev_io, uint32 *);
SVCRT_SVC_DECL_V2(0x11, svcrt_call_task_ctrl, uint32, uint32);
SVCRT_SVC_DECL_1(uint32, 0x12, svcrt_call_sys_info, uint32);
SVCRT_SVC_DECL_1(int32, 0x13, svcrt_call_event_ctrl, uint32 *);

void svcrt_task_wait(uint32 ms)
{
    svcrt_call_task_ctrl(1, ms);
}

void svcrt_task_wait_period(void)
{
    svcrt_call_task_ctrl(2, 0);
}

void svcrt_task_delay(uint32 us)
{
    svcrt_call_task_ctrl(3, us);
}

uint32 svcrt_get_time_ms(void)
{
    return svcrt_call_sys_info(1);
}

int32 svcrt_event_create(char *name)
{
    uint32 p[3];
    p[0] = 1;
    p[1] = (uint32)name;
    return svcrt_call_event_ctrl(p);
}

/* A driver runs unprivileged too, so the kernel cannot yield while it is
 * inside the SVC handler: the wait only registers and answers
 * SVCRT_SYNC_ERR_WOULDBLOCK.  The yielding happens here, in Thread mode.
 * timeout <= 0 means "wait forever". */
#define SVCRT_DRV_POLL_MS   (1u)

static int32 svcrt_drv_wait_expired(uint32 start_ms, int32 timeout)
{
    if(timeout <= 0)
    {
        return 0;                           /* unbounded wait */
    }
    return ((svcrt_get_time_ms() - start_ms) >= (uint32)timeout) ? 1 : 0;
}

static int32 svcrt_event_wait_raw(int32 handle, int32 timeout)
{
    uint32 p[3];
    p[0] = 2;
    p[1] = handle;
    p[2] = timeout;
    return (int32)svcrt_call_event_ctrl(p);
}

int32 svcrt_event_wait(int32 handle, int32 timeout)
{
    uint32 start = svcrt_get_time_ms();

    for(;;)
    {
        int32 r = svcrt_event_wait_raw(handle, timeout);

        if(r != SVCRT_SYNC_ERR_WOULDBLOCK)
        {
            return r;
        }
        if(svcrt_drv_wait_expired(start, timeout) != 0)
        {
            /* Close it out with timeout 0: the kernel then drops our own
             * registration and answers TIMEOUT. */
            (void)svcrt_event_wait_raw(handle, 0);
            return SVCRT_SYNC_ERR_TIMEOUT;
        }
        svcrt_task_wait(SVCRT_DRV_POLL_MS);
    }
}

void svcrt_event_set(int32 handle)
{
    uint32 p[3];
    p[0] = 3;
    p[1] = handle;
    svcrt_call_event_ctrl(p);
}

/* ============================================================
 * Log and console services (SVC 0x19 / 0x1A)
 *
 * The formatting itself is done on this side (see svcrt_ulog.h), so the
 * kernel only has to filter by level and push the bytes out. One level
 * setting therefore applies to the kernel and to every App / driver.
 * ============================================================ */
SVCRT_SVC_DECL_3(int32, 0x19, svcrt_call_log, uint32, uint32, uint32);
SVCRT_SVC_DECL_1(int32, 0x1A, svcrt_call_shell_svc, uint32 *);

int32 svcrt_log_print(uint32 level, const char *tag, const char *msg)
{
    return svcrt_call_log(level, (uint32)tag, (uint32)msg);
}

int32 svcrt_shell_cmd_register(const svcrt_ushell_cmd_t *cmd)
{
    uint32 p[3];
    p[0] = 1; p[1] = (uint32)cmd; p[2] = 0;
    return svcrt_call_shell_svc(p);
}

int32 svcrt_shell_cmd_unregister(const char *name)
{
    uint32 p[3];
    p[0] = 2; p[1] = (uint32)name; p[2] = 0;
    return svcrt_call_shell_svc(p);
}

int32 svcrt_shell_print(const char *msg)
{
    uint32 p[3];
    p[0] = 3; p[1] = (uint32)msg; p[2] = 0;
    return svcrt_call_shell_svc(p);
}

/* ============================================================
 * User thread service (SVC 0x1B)
 *
 * The kernel owns the TCB and the scheduler, so a user mode thread has to be
 * asked for across the SVC boundary. The entry point and the stack window are
 * validated kernel side against the caller's own regions.
 * ============================================================ */
SVCRT_SVC_DECL_1(int32, 0x1B, svcrt_call_thread_ctrl, uint32 *);

int32 svcrt_thread_create(void (*entry)(void), void *stack, uint32 stack_size,
                          uint32 priority, uint32 period_ms)
{
    uint32 p[6];
    p[0] = 1; p[1] = (uint32)entry; p[2] = (uint32)stack;
    p[3] = stack_size; p[4] = priority; p[5] = period_ms;
    return svcrt_call_thread_ctrl(p);
}

int32 svcrt_thread_self(void)
{
    uint32 p[6];
    p[0] = 2; p[1] = 0; p[2] = 0; p[3] = 0; p[4] = 0; p[5] = 0;
    return svcrt_call_thread_ctrl(p);
}

void svcrt_thread_exit(void)
{
    uint32 p[6];
    p[0] = 3; p[1] = 0; p[2] = 0; p[3] = 0; p[4] = 0; p[5] = 0;
    (void)svcrt_call_thread_ctrl(p);
}
