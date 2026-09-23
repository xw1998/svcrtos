/**
* @file sys_arch.c
* @brief lwIP 的 OS 面实现（SVCrtOS）。见 arch/sys_arch.h 的映射表。
* @details 两条贯穿全文件的口径：
*            - **超时语义要换算**。lwIP 说 timeout==0 是"永久等待"，
*              SVCrtOS 说 0 是"不等待"、负值才是永久。所有入口都过
*              lwip_to_svcrt_timeout()，谁也别自己写死参数。
*            - **失败不装成功**。内核对象创建失败、句柄非法、超时以外的错误码，
*              一律回 lwIP 认的失败值，并且往内核日志里留一条带错误码的记录；
*              不把"参数错了"说成"超时了"还不吭声。
*
*          线程只有一个来源：tcpip 线程（sockets API 必须有它）。栈是这里的
*          静态数组——内核任务只接受"调用者自己的"栈，理由见 svcrt_cfg.h。
*
* @author xw
* @date 2026.09.23
*/
#include "lwip/opt.h"
#include "lwip/sys.h"
#include "lwip/err.h"
#include "lwip/tcpip.h"

#include "svcrt_cfg.h"       /* svcrt_task_register()：线程只能从内核注册 */
#include "svcrt_config.h"    /* SVCRT_MQ_DEPTH：邮箱深度就是它的上限 */
#include "svcrt_task.h"      /* wait_period_internal / kernel_get_time / task_wait_internal */
#include "svcrt_sync.h"      /* *_internal：内核侧没有 SVC 可走，只能用内部原语 */
#include "svcrt_mq.h"
#include "svcrt_log.h"

/* lwIP 的 errno 变量。LWIP_PROVIDE_ERRNO=1 时由端口提供定义
 * （见 lwip/errno.h 末尾的 extern int errno）。
 * 这里只有一个全局量：多任务共享 errno 是 lwIP sockets 层自己的已知限制，
 * 我们没有给它做每任务副本——所以判断 socket 调用是否失败要看返回值，
 * 不要跨任务依赖 errno 的值。 */
int errno;

#include "lwip_port.h"

/* ============================================================
 * 超时换算
 * ============================================================ */

/** @brief lwIP 的毫秒超时 → SVCrtOS 的毫秒超时（0 = 永久）。 */
static int32 lwip_to_svcrt_timeout(u32_t timeout)
{
    return (timeout == 0u) ? -1 : (int32)timeout;
}

/* ============================================================
 * 信号量
 * ============================================================ */

err_t sys_sem_new(sys_sem_t *sem, u8_t count)
{
    int32 h;

    if(sem == NULL)
    {
        return ERR_ARG;
    }
    h = svcrt_sem_create_internal("lwip.sem", (int32)count);
    if(h < 0)
    {
        SVCRT_LOGE("LWIP", "sem_create failed rc=%d", (int)h);
        *sem = SYS_SEM_NULL;
        return ERR_MEM;
    }
    *sem = h;
    return ERR_OK;
}

void sys_sem_free(sys_sem_t *sem)
{
    if(!sys_sem_valid(sem))
    {
        return;
    }
    (void)svcrt_sem_delete_internal(*sem);
    sys_sem_set_invalid(sem);
}

void sys_sem_signal(sys_sem_t *sem)
{
    if(!sys_sem_valid(sem))
    {
        SVCRT_LOGE("LWIP", "sem_signal on invalid handle");
        return;
    }
    if(svcrt_sem_post_internal(*sem) != 0)
    {
        SVCRT_LOGE("LWIP", "sem_post failed h=%d", (int)*sem);
    }
}

u32_t sys_arch_sem_wait(sys_sem_t *sem, u32_t timeout)
{
    u32_t start;
    int32 rc;

    if(!sys_sem_valid(sem))
    {
        SVCRT_LOGE("LWIP", "sem_wait on invalid handle");
        return SYS_ARCH_TIMEOUT;
    }
    start = (u32_t)svcrt_kernel_get_time();
    rc = svcrt_sem_wait_internal(*sem, lwip_to_svcrt_timeout(timeout));
    if(rc != 0)
    {
        /* -2 是超时（正常路径）；别的负值是参数/句柄错误，也按超时回给 lwIP，
         * 但必须留痕，不能让"配置写错了"伪装成"等超时了"。 */
        if(rc != SVCRT_SYNC_ERR_TIMEOUT)
        {
            SVCRT_LOGE("LWIP", "sem_wait error rc=%d h=%d", (int)rc, (int)*sem);
        }
        return SYS_ARCH_TIMEOUT;
    }
    return (u32_t)svcrt_kernel_get_time() - start;
}

/* ============================================================
 * 互斥量
 * ============================================================ */

err_t sys_mutex_new(sys_mutex_t *mutex)
{
    int32 h;

    if(mutex == NULL)
    {
        return ERR_ARG;
    }
    h = svcrt_mtx_create_internal("lwip.mtx");
    if(h < 0)
    {
        SVCRT_LOGE("LWIP", "mutex_create failed rc=%d", (int)h);
        *mutex = SYS_MUTEX_NULL;
        return ERR_MEM;
    }
    *mutex = h;
    return ERR_OK;
}

void sys_mutex_free(sys_mutex_t *mutex)
{
    if((mutex == NULL) || (*mutex < 0))
    {
        return;
    }
    (void)svcrt_mtx_delete_internal(*mutex);
    *mutex = SYS_MUTEX_NULL;
}

void sys_mutex_lock(sys_mutex_t *mutex)
{
    /* 负值 = 永久等待：lwIP 的互斥量没有超时语义。 */
    if((mutex == NULL) || (svcrt_mtx_lock_internal(*mutex, -1) != 0))
    {
        SVCRT_LOGE("LWIP", "mutex_lock failed");
    }
}

void sys_mutex_unlock(sys_mutex_t *mutex)
{
    if(mutex == NULL)
    {
        return;
    }
    if(svcrt_mtx_unlock_internal(*mutex) != 0)
    {
        SVCRT_LOGE("LWIP", "mutex_unlock failed h=%d", (int)*mutex);
    }
}

/* ============================================================
 * 消息队列（mbox：一条消息 = 一个指针）
 * ============================================================ */

/* lwIP 的邮箱装的是 void*，SVCrtOS 的消息队列按 32 位字搬运：这里固定搬
 * 一个字（指针本身），所以下面全部写成 (uint32 *)&ptr。队列的"深度"是
 * 消息条数（<= SVCRT_MQ_DEPTH），不是字节数。 */
err_t sys_mbox_new(sys_mbox_t *mbox, int size)
{
    int32 h;

    if(mbox == NULL)
    {
        return ERR_ARG;
    }
    /* 内核队列的深度是编译期定值（SVCRT_MQ_DEPTH）。要得比它大就直说，
     * 不截短——截短的后果是丢包，而且丢在看不见的地方。 */
    if((size <= 0) || (size > (int)SVCRT_MQ_DEPTH))
    {
        SVCRT_LOGE("LWIP", "mbox size %d outside 1..%d", size, (int)SVCRT_MQ_DEPTH);
        *mbox = SYS_MBOX_NULL;
        return ERR_MEM;
    }
    h = svcrt_mq_create_internal("lwip.mbx");
    if(h < 0)
    {
        SVCRT_LOGE("LWIP", "mq_create failed rc=%d", (int)h);
        *mbox = SYS_MBOX_NULL;
        return ERR_MEM;
    }
    *mbox = h;
    return ERR_OK;
}

void sys_mbox_free(sys_mbox_t *mbox)
{
    if(!sys_mbox_valid(mbox))
    {
        return;
    }
    (void)svcrt_mq_delete_internal(*mbox);
    sys_mbox_set_invalid(mbox);
}

void sys_mbox_post(sys_mbox_t *mbox, void *msg)
{
    if(!sys_mbox_valid(mbox))
    {
        SVCRT_LOGE("LWIP", "mbox_post on invalid handle");
        return;
    }
    /* 负值 = 等到有位为止：lwIP 认为 post 不该丢消息。 */
    if(svcrt_mq_send_internal(*mbox, (uint32 *)&msg, 1, -1) != 0)
    {
        SVCRT_LOGE("LWIP", "mbox_post failed h=%d", (int)*mbox);
    }
}

err_t sys_mbox_trypost(sys_mbox_t *mbox, void *msg)
{
    if(!sys_mbox_valid(mbox))
    {
        return ERR_ARG;
    }
    if(svcrt_mq_send_internal(*mbox, (uint32 *)&msg, 1, 0) != 0)
    {
        return ERR_MEM;     /* 满：lwIP 会按"这次没投进去"处理 */
    }
    return ERR_OK;
}

err_t sys_mbox_trypost_fromisr(sys_mbox_t *mbox, void *msg)
{
    if(!sys_mbox_valid(mbox))
    {
        return ERR_ARG;
    }
    if(svcrt_mq_send_from_isr_internal(*mbox, (uint32 *)&msg, 1) != 0)
    {
        return ERR_MEM;
    }
    return ERR_OK;
}

u32_t sys_arch_mbox_fetch(sys_mbox_t *mbox, void **msg, u32_t timeout)
{
    u32_t start;
    void *got = NULL;
    int32 rc;

    if(!sys_mbox_valid(mbox))
    {
        SVCRT_LOGE("LWIP", "mbox_fetch on invalid handle");
        return SYS_ARCH_TIMEOUT;
    }
    start = (u32_t)svcrt_kernel_get_time();
    /* 收进本地变量再交出去：超时那一轮 mbox 里什么都没有，绝不能让
     * 调用者拿到一个没被写过的指针。 */
    rc = svcrt_mq_recv_internal(*mbox, (uint32 *)&got, 1, lwip_to_svcrt_timeout(timeout));
    if(rc < 0)
    {
        if(rc != SVCRT_SYNC_ERR_TIMEOUT)
        {
            SVCRT_LOGE("LWIP", "mq_recv error rc=%d h=%d", (int)rc, (int)*mbox);
        }
        return SYS_ARCH_TIMEOUT;
    }
    if(msg != NULL)
    {
        *msg = got;
    }
    return (u32_t)svcrt_kernel_get_time() - start;
}

u32_t sys_arch_mbox_tryfetch(sys_mbox_t *mbox, void **msg)
{
    void *got = NULL;

    if(!sys_mbox_valid(mbox))
    {
        return SYS_MBOX_EMPTY;
    }
    if(svcrt_mq_recv_internal(*mbox, (uint32 *)&got, 1, 0) < 0)
    {
        return SYS_MBOX_EMPTY;
    }
    if(msg != NULL)
    {
        *msg = got;
    }
    return 0;
}

/* ============================================================
 * 线程
 * ============================================================ */

/* lwIP 只会为 tcpip 线程调这里（sockets/netconn 都由它服务，App 不直接建
 * lwIP 线程）。两个槽足够，多出来的请求会拿到 SYS_THREAD_NULL 而不是
 * 悄悄复用别人的栈。 */
#define LWIP_THREAD_MAX      2
#define LWIP_THREAD_STACK_W  512u   /* 2 KB，与 TCPIP_THREAD_STACKSIZE 对齐 */

typedef struct
{
    uint32          used;
    uint32          stack[LWIP_THREAD_STACK_W];
    lwip_thread_fn  entry;
    void           *arg;
} lwip_thread_slot_t;

static lwip_thread_slot_t g_thread_slot[LWIP_THREAD_MAX];

/* 每个槽一个入口：内核任务注册不收参数，靠"槽位专属入口"把 arg 带过去，
 * 免得去猜当前是哪个槽。 */
static void lwip_thread_tramp0(void);
static void lwip_thread_tramp1(void);

static void (* const g_thread_tramp[LWIP_THREAD_MAX])(void) =
{
    lwip_thread_tramp0,
    lwip_thread_tramp1
};

/** @brief 线程体跑完不该返回：内核任务返回意味着任务结束，lwIP 没这个预期。 */
static void lwip_thread_run(uint32 idx)
{
    g_thread_slot[idx].entry(g_thread_slot[idx].arg);
    SVCRT_LOGE("LWIP", "thread %u returned", (unsigned)idx);
    for(;;)
    {
        svcrt_task_wait_internal(1000u);
    }
}

static void lwip_thread_tramp0(void)
{
    lwip_thread_run(0u);
}

static void lwip_thread_tramp1(void)
{
    lwip_thread_run(1u);
}

sys_thread_t sys_thread_new(const char *name, lwip_thread_fn thread, void *arg,
                            int stacksize, int prio)
{
    uint32 i;

    (void)name;
    if((thread == NULL) || (prio <= 0) || (prio > 254))
    {
        SVCRT_LOGE("LWIP", "thread_new bad args prio=%d", prio);
        return SYS_THREAD_NULL;
    }
    if(((uint32)stacksize > (LWIP_THREAD_STACK_W * 4u)) || (stacksize < 128))
    {
        /* 要的栈比这里有的还大：回失败。悄悄给一半的后果是随机踩内存。 */
        SVCRT_LOGE("LWIP", "thread_new stack %d out of range", stacksize);
        return SYS_THREAD_NULL;
    }
    for(i = 0u; i < (uint32)LWIP_THREAD_MAX; i++)
    {
        if(g_thread_slot[i].used == 0u)
        {
            g_thread_slot[i].used  = 1u;
            g_thread_slot[i].entry = thread;
            g_thread_slot[i].arg   = arg;
            return (sys_thread_t)svcrt_task_register(g_thread_tramp[i],
                                                     g_thread_slot[i].stack,
                                                     (uint32)stacksize,
                                                     (uint8)prio, 0u);
        }
    }
    SVCRT_LOGE("LWIP", "thread_new: no slot");
    return SYS_THREAD_NULL;
}

/* ============================================================
 * 时间与初始化
 * ============================================================ */

u32_t sys_now(void)
{
    return (u32_t)svcrt_kernel_get_time();
}

void sys_init(void)
{
    /* SVCrtOS 没有需要在这里初始化的 OS 侧资源：内核对象按需创建。 */
}

/* lwIP 的断言出口（lwipopts.h 里把 LWIP_PLATFORM_ASSERT 指到这里）。
 * 不调用 abort：内核里 abort 只会把整块板子带下去，而调用者想知道的是
 * "哪一条断言、在哪个文件"。 */
void svcrt_lwip_assert_hook(const char *msg, int line, const char *file)
{
    SVCRT_LOGE("LWIP", "assert %s @%s:%d", (msg != NULL) ? msg : "?", file, line);
}
