/**
* @brief SVCrtOS ?????????????????
* @details ????ó???(????)????OS??????????SVC???????????
* @author xw
* @date 2026.05.03
*/

#include "svcrt.h"

int32  __svc(0x10)  svcrt_call_dev_io(uint32 *p);
void   __svc(0x11)  svcrt_call_task_ctrl(uint32 fn, uint32 p);
uint32 __svc(0x12)  svcrt_call_sys_info(uint32 fn);
int32  __svc(0x13)  svcrt_call_event_ctrl(uint32 *p);
int32  __svc(0x15)  svcrt_call_sync_ctrl(uint32 *p);

int32 svcrt_dev_open(char *name, uint32 param)
{
    uint32 parameters[4];

    parameters[0] = 1;
    parameters[1] = (uint32)name;
    parameters[2] = param;

    return svcrt_call_dev_io(parameters);
}

int32 svcrt_dev_read(int32 handle, void *pdata, int32 len)
{
    uint32 parameters[4];

    parameters[0] = 2;
    parameters[1] = handle;
    parameters[2] = (uint32)pdata;
    parameters[3] = len;

    return svcrt_call_dev_io(parameters);
}

int32 svcrt_dev_write(int32 handle, void *pdata, int32 len)
{
    uint32 parameters[4];

    parameters[0] = 3;
    parameters[1] = handle;
    parameters[2] = (uint32)pdata;
    parameters[3] = len;

    return svcrt_call_dev_io(parameters);
}

int32 svcrt_dev_close(int32 handle)
{
    uint32 parameters[4];

    parameters[0] = 5;
    parameters[1] = handle;

    return svcrt_call_dev_io(parameters);
}

int32 svcrt_dev_ctrl(int32 handle, uint32 code, uint32 value)
{
    uint32 parameters[4];

    parameters[0] = 4;
    parameters[1] = handle;
    parameters[2] = code;
    parameters[3] = value;

    return svcrt_call_dev_io(parameters);
}

__weak void AppMain(void)
{
}

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

void svcrt_task_kill(void)
{
    svcrt_call_task_ctrl(4, 0);
}

uint32 svcrt_get_time_ms(void)
{
    return svcrt_call_sys_info(1);
}

uint32 svcrt_get_cpu_usage(void)
{
    return svcrt_call_sys_info(2);
}

int32 svcrt_event_create(char *name)
{
    uint32 p[3];
    p[0] = 1;
    p[1] = (uint32)name;
    return svcrt_call_event_ctrl(p);
}

void svcrt_event_wait(int32 handle, int32 timeout)
{
    uint32 p[3];
    p[0] = 2;
    p[1] = handle;
    p[2] = timeout;
    svcrt_call_event_ctrl(p);
}

void svcrt_event_set(int32 handle)
{
    uint32 p[3];
    p[0] = 3;
    p[1] = handle;
    svcrt_call_event_ctrl(p);
}

int32 svcrt_sem_create(char *name, int32 init_count)
{
    uint32 p[3];
    p[0] = 1;
    p[1] = (uint32)name;
    p[2] = (uint32)init_count;
    return svcrt_call_sync_ctrl(p);
}

int32 svcrt_sem_wait(int32 handle, int32 timeout)
{
    uint32 p[3];
    p[0] = 2;
    p[1] = handle;
    p[2] = (uint32)timeout;
    return svcrt_call_sync_ctrl(p);
}

int32 svcrt_sem_post(int32 handle)
{
    uint32 p[3];
    p[0] = 3;
    p[1] = handle;
    return svcrt_call_sync_ctrl(p);
}

int32 svcrt_sem_delete(int32 handle)
{
    uint32 p[3];
    p[0] = 4;
    p[1] = handle;
    return svcrt_call_sync_ctrl(p);
}

int32 svcrt_mutex_create(char *name)
{
    uint32 p[3];
    p[0] = 5;
    p[1] = (uint32)name;
    return svcrt_call_sync_ctrl(p);
}

int32 svcrt_mutex_lock(int32 handle, int32 timeout)
{
    uint32 p[3];
    p[0] = 6;
    p[1] = handle;
    p[2] = (uint32)timeout;
    return svcrt_call_sync_ctrl(p);
}

int32 svcrt_mutex_unlock(int32 handle)
{
    uint32 p[3];
    p[0] = 7;
    p[1] = handle;
    return svcrt_call_sync_ctrl(p);
}

int32 svcrt_mutex_delete(int32 handle)
{
    uint32 p[3];
    p[0] = 8;
    p[1] = handle;
    return svcrt_call_sync_ctrl(p);
}

/* ============================================================
 * 消息队列 / 软定时器 / 任务与故障查询（P0 扩展）
 * ============================================================ */
int32  __svc(0x16)  svcrt_call_mq_ctrl(uint32 *p);
int32  __svc(0x17)  svcrt_call_timer_ctrl(uint32 *p);
int32  __svc(0x11)  svcrt_call_task_ctrl_ret(uint32 fn, uint32 p);
uint32 __svc(0x12)  svcrt_call_sys_info_ext(uint32 fn, uint32 a1, uint32 a2);

int32 svcrt_mq_create(char *name)
{
    uint32 p[6];
    p[0] = 1; p[1] = (uint32)name; p[2] = 0; p[3] = 0; p[4] = 0; p[5] = 0;
    return svcrt_call_mq_ctrl(p);
}

int32 svcrt_mq_send(int32 handle, void *buf, int32 len_words, int32 timeout)
{
    uint32 p[6];
    p[0] = 2; p[1] = (uint32)handle; p[2] = (uint32)buf;
    p[3] = (uint32)len_words; p[4] = (uint32)timeout; p[5] = 0;
    return svcrt_call_mq_ctrl(p);
}

int32 svcrt_mq_recv(int32 handle, void *buf, int32 len_words, int32 timeout)
{
    uint32 p[6];
    p[0] = 3; p[1] = (uint32)handle; p[2] = (uint32)buf;
    p[3] = (uint32)len_words; p[4] = (uint32)timeout; p[5] = 0;
    return svcrt_call_mq_ctrl(p);
}

int32 svcrt_mq_delete(int32 handle)
{
    uint32 p[6];
    p[0] = 4; p[1] = (uint32)handle; p[2] = 0; p[3] = 0; p[4] = 0; p[5] = 0;
    return svcrt_call_mq_ctrl(p);
}

int32 svcrt_timer_create(char *name)
{
    uint32 p[6];
    p[0] = 1; p[1] = (uint32)name; p[2] = 0; p[3] = 0; p[4] = 0; p[5] = 0;
    return svcrt_call_timer_ctrl(p);
}

int32 svcrt_timer_start(int32 handle, uint32 period_ms, uint32 mode,
                        void (*cb)(void *), void *arg)
{
    uint32 p[6];
    p[0] = 2; p[1] = (uint32)handle; p[2] = period_ms; p[3] = mode;
    p[4] = (uint32)cb; p[5] = (uint32)arg;
    return svcrt_call_timer_ctrl(p);
}

int32 svcrt_timer_stop(int32 handle)
{
    uint32 p[6];
    p[0] = 3; p[1] = (uint32)handle; p[2] = 0; p[3] = 0; p[4] = 0; p[5] = 0;
    return svcrt_call_timer_ctrl(p);
}

int32 svcrt_timer_delete(int32 handle)
{
    uint32 p[6];
    p[0] = 4; p[1] = (uint32)handle; p[2] = 0; p[3] = 0; p[4] = 0; p[5] = 0;
    return svcrt_call_timer_ctrl(p);
}

int32 svcrt_task_status_get(int32 task_id)
{
    return svcrt_call_task_ctrl_ret(5, (uint32)task_id);
}

int32 svcrt_task_recover_req(int32 task_id)
{
    return svcrt_call_task_ctrl_ret(6, (uint32)task_id);
}

int32 svcrt_fault_record_count(void)
{
    return (int32)svcrt_call_sys_info_ext(3, 0, 0);
}

int32 svcrt_fault_record_read(int32 index, uint32 *out3)
{
    return (int32)svcrt_call_sys_info_ext(4, (uint32)index, (uint32)out3);
}

/* ============================================================
 * 调度器锁 / 任务栈用量查询（SVC 0x11 子命令 7~10）
 * ============================================================ */

/* 3 参数版本的任务控制调用：r0=子命令，r1/r2=参数 */
int32  __svc(0x11)  svcrt_call_task_ctrl_arg2(uint32 fn, uint32 a1, uint32 a2);

void svcrt_sched_lock(void)
{
    svcrt_call_task_ctrl_ret(7, 0);
}

void svcrt_sched_unlock(void)
{
    svcrt_call_task_ctrl_ret(8, 0);
}

int32 svcrt_sched_lock_count(void)
{
    return svcrt_call_task_ctrl_ret(9, 0);
}

int32 svcrt_task_stack_info(int32 task_id, uint32 *out3)
{
    return svcrt_call_task_ctrl_arg2(10, (uint32)task_id, (uint32)out3);
}

/* ============================================================
 * App 镜像管理与分区查询（SVC 0x18 子命令 2~5）
 * ============================================================ */
int32  __svc(0x18)  svcrt_call_app_mgr(uint32 *p);

int32 svcrt_app_load(int32 dev, uint32 image_len)
{
    uint32 p[6];
    p[0] = 2; p[1] = (uint32)dev; p[2] = image_len; p[3] = 0; p[4] = 0; p[5] = 0;
    return svcrt_call_app_mgr(p);
}

int32 svcrt_app_start(uint32 slot)
{
    uint32 p[6];
    p[0] = 3; p[1] = slot; p[2] = 0; p[3] = 0; p[4] = 0; p[5] = 0;
    return svcrt_call_app_mgr(p);
}

int32 svcrt_app_stop(uint32 slot)
{
    uint32 p[6];
    p[0] = 4; p[1] = slot; p[2] = 0; p[3] = 0; p[4] = 0; p[5] = 0;
    return svcrt_call_app_mgr(p);
}

uint32 svcrt_app_status(uint32 slot)
{
    uint32 p[6];
    p[0] = 5; p[1] = slot; p[2] = 0; p[3] = 0; p[4] = 0; p[5] = 0;
    return (uint32)svcrt_call_app_mgr(p);
}
