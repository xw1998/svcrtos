/**
* @file svcrt_log.c
* @brief SVCrtOS 内核日志：分级输出与运行期过滤的实现
* @details 输出目标是内核控制台串口（SHELL_DEV_NAME）。这里不复用 shell 的
*          平台层句柄，而是自己惰性打开一次：日志允许在 shell 之前使用
*          （比如启动横幅），也允许在 SHELL_ENABLE=0 的裁剪配置里单独存在。
*          两次 open 拿到的是同一个设备对象，写接口串行调用，互不干扰。
*
*          级别过滤分两道：
*            - 编译期：SVCRT_LOG_LEVEL 决定哪些宏展开成空语句（见 svcrt_log.h）；
*            - 运行期：本文件的 g_log_level，可由 shell 的 `log <level>` 或
*              SVC 0x19 之外的内部调用改变，全局生效（内核 + 所有 App/驱动）。
*
*          行内不做重入保护：日志只能在任务上下文调用，这是设备框架的
*          写接口本身的前提（见 svcrt_dev.h）。
*
* @note 本模块由内核特权态调用；用户态经 SVC 0x19 进入 svcrt_log_svc()。
*/

#include "svcrt_config.h"
#include "svcrt_log.h"
#include "svcrt_dev.h"
#include "svcrt_hal.h"

/* Per-byte budget while the transmit pipe is full. */
/* The console device queues into a 128 byte pipe that the TXE interrupt */
/* drains, so a byte waits about 87 us at 115200 - and longer when the */
/* producers stay ahead of the line, which is the normal case for a burst. */
/* Measured on board: with a 4000 iteration budget the counters showed */
/* tx_drop=420 B alongside 1.69M full-pipe retries, i.e. the budget was */
/* still shorter than the time one byte needed.  Budget for a complete pipe */
/* drain (127 x 87 us ~= 11 ms) so a burst is never cut; the value stays */
/* bounded, so a dead port still cannot hang the caller forever.  Lower it */
/* with the `log` counters in hand, never on a guess.  Same value as the */
/* shell platform layer (SHELL_TX_SPIN_LIMIT): the shell never lost a */
/* byte while the console path used 64, and that gap was the whole */
/* difference between the two. */
#define SVCRT_CONSOLE_TX_WAIT     (200000u)

/* 前缀缓冲：颜色 + [级别] + [tag:行号] + 空格 */
#define SVCRT_LOG_HEAD_SIZE       (64)

/* 控制台句柄（-1 = 尚未打开） */
static int32  g_log_dev   = -1;

/* 运行期级别：初值取编译期配置，保证「配置里关掉的级别」不会因为
 * 忘了调用 svcrt_log_set_level() 而在运行期被打开 */
static uint32 g_log_level = (uint32)SVCRT_LOG_LEVEL;

/* Console sink lock.  Held across one complete output unit so that no two
 * writers can interleave half a line.  Deliberately does NOT mask
 * interrupts: the console device is interrupt driven, so masking them
 * stops the transmit FIFO from draining and the byte loop then drops the
 * line.  A plain CAS busy flag is also the right shape here - the
 * re-entrant spinlock would silently hand the lock to a handler that
 * preempted the holder, which is exactly the case this lock exists for.
 * See svcrt_log.h. */
static volatile uint32 g_console_busy    = 0u;
static volatile uint32 g_console_busy_ev = 0u;

/* Bytes the writer gave up on after the whole wait budget.  This is real
 * output loss, not a retry: the transmit pipe stayed full for the entire
 * budget and the byte never left the MCU. */
static volatile uint32 g_console_tx_drop = 0u;

/* Bounded spin: long enough to cover a console line, short enough that a
 * handler never looks hung. */
#define SVCRT_CONSOLE_SPIN_LIMIT  (400000u)


/* ============================================================
 * 输出通道
 * ============================================================ */

static int32 svcrt_log_dev(void)
{
    if(g_log_dev < 0)
    {
        char name[] = SHELL_DEV_NAME;

        g_log_dev = svcrt_dev_open_internal(name, (uint32)SHELL_DEV_ARG);
    }

    return g_log_dev;
}

/* Console API ------------------------------------------------------- */

int32 svcrt_console_handle(void)
{
    return svcrt_log_dev();
}

int32 svcrt_console_is_handle(int32 handle)
{
    return (handle == svcrt_log_dev()) ? 1 : 0;
}

int32 svcrt_console_lock(void)
{
    uint32 spin = 0u;

    while(svcrt_port_atomic_cas(&g_console_busy, 0u, 1u) == 0u)
    {
        if(++spin > SVCRT_CONSOLE_SPIN_LIMIT)
        {
            /* Console stayed busy for the whole budget.  Write the unit
             * anyway: output is never dropped because of contention, and
             * no context can deadlock waiting for a task that cannot run. */
            g_console_busy_ev++;
            return 0;
        }
    }

    SVCRT_DMB();
    return 1;
}

void svcrt_console_unlock(void)
{
    SVCRT_DMB();
    g_console_busy = 0u;
}

uint32 svcrt_console_busy_count(void)
{
    return g_console_busy_ev;
}

uint32 svcrt_console_tx_drop_count(void)
{
    return g_console_tx_drop;
}

int32 svcrt_console_write(const uint8 *p_data, uint32 len)
{
    int32  dev = svcrt_log_dev();
    int32  held;
    uint32 i;

    if((p_data == 0) || (len == 0u))
    {
        return 0;
    }

    if(dev < 0)
    {
        return -1;
    }

    held = svcrt_console_lock();
    for(i = 0u; i < len; i++)
    {
        uint32 spin = 0u;
        int32  queued = 0;

        do
        {
            if(svcrt_dev_write_internal(dev, (uint8 *)&p_data[i], 1) == 1)
            {
                queued = 1;
                break;
            }
            spin++;
        } while(spin < SVCRT_CONSOLE_TX_WAIT);

        if(queued == 0)
        {
            g_console_tx_drop++;
        }
    }
    if(held != 0)
    {
        svcrt_console_unlock();
    }

    return (int32)len;
}

/* 逐字节写：板级发送管道按字节计账，写满即短写，整串一次写会写不进去
 * （管道已放大到 1024 B，但逐字节 + 自旋才是这里的契约） */
/* Raw byte loop.  Callers must hold the console lock; svcrt_log_emit()
 * takes it once for the whole line. */
static void svcrt_log_puts_raw(const char *s)
{
    int32 dev = svcrt_log_dev();

    if((s == 0) || (dev < 0))
    {
        return;
    }

    while(*s != '\0')
    {
        uint32 spin = 0u;
        int32  queued = 0;

        do
        {
            if(svcrt_dev_write_internal(dev, (uint8 *)s, 1) == 1)
            {
                queued = 1;
                break;
            }
            spin++;
        } while(spin < SVCRT_CONSOLE_TX_WAIT);

        if(queued == 0)
        {
            g_console_tx_drop++;
        }
        s++;
    }
}

/* ============================================================
 * 级别辅助
 * ============================================================ */

static const char *svcrt_log_level_name(uint32 level)
{
    switch(level)
    {
        case SVCRT_LOG_ERROR:   return "E";
        case SVCRT_LOG_WARNING: return "W";
        case SVCRT_LOG_INFO:    return "I";
        case SVCRT_LOG_DEBUG:   return "D";
        default:                return "?";
    }
}

static const char *svcrt_log_level_color(uint32 level)
{
    switch(level)
    {
        case SVCRT_LOG_ERROR:   return SVCRT_LOG_COLOR_ERROR;
        case SVCRT_LOG_WARNING: return SVCRT_LOG_COLOR_WARNING;
        case SVCRT_LOG_INFO:    return SVCRT_LOG_COLOR_INFO;
        case SVCRT_LOG_DEBUG:   return SVCRT_LOG_COLOR_DEBUG;
        default:                return SVCRT_LOG_COLOR_RESET;
    }
}

/* ============================================================
 * 对外接口
 * ============================================================ */

void svcrt_log_init(void)
{
    g_log_level = (uint32)SVCRT_LOG_LEVEL;

    (void)svcrt_log_dev();
}

void svcrt_log_set_level(uint32 level)
{
    if(level > (uint32)SVCRT_LOG_DEBUG)
    {
        return;                 /* 非法级别不改变现状，避免误配后全体静音 */
    }

    g_log_level = level;
}

uint32 svcrt_log_get_level(void)
{
    return g_log_level;
}

void svcrt_log_emit(uint32 level, const char *tag, uint32 line, const char *msg)
{
    char head[SVCRT_LOG_HEAD_SIZE];
    int32  held;

    if(msg == 0)
    {
        return;
    }

    /* 级别过滤：NONE 与高于当前级别的日志直接丢弃 */
    if((level == SVCRT_LOG_NONE) || (level > g_log_level) ||
       (level > (uint32)SVCRT_LOG_DEBUG))
    {
        return;
    }

    if(tag == 0)
    {
        tag = "?";
    }

    if(line != 0u)
    {
        (void)snprintf(head, sizeof(head), "%s[%s]%s[%s:%u] ",
                       svcrt_log_level_color(level),
                       svcrt_log_level_name(level),
                       SVCRT_LOG_COLOR_RESET,
                       tag, (unsigned)line);
    }
    else
    {
        (void)snprintf(head, sizeof(head), "%s[%s]%s[%s] ",
                       svcrt_log_level_color(level),
                       svcrt_log_level_name(level),
                       SVCRT_LOG_COLOR_RESET,
                       tag);
    }

    /* One output unit = one lock.  Nothing can be spliced into the
     * middle of a log line. */
    held = svcrt_console_lock();
    svcrt_log_puts_raw(head);
    svcrt_log_puts_raw(msg);
    svcrt_log_puts_raw("\r\n");
    if(held != 0)
    {
        svcrt_console_unlock();
    }
}

int32 svcrt_log_svc(uint32 level, const char *tag, const char *msg)
{
    /* 用户态只能调高到 DEBUG 为止；越界按 DEBUG 处理而不是拒绝：
     * 日志接口不值得让调用者去处理错误码。 */
    if(level > (uint32)SVCRT_LOG_DEBUG)
    {
        level = (uint32)SVCRT_LOG_DEBUG;
    }

    if(msg == 0)
    {
        return -1;
    }

    svcrt_log_emit(level, tag, 0u, msg);
    return 0;
}
