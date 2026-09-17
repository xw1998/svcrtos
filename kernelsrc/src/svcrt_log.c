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

/* 发送忙等上限：串口彻底卡死时不死循环（与 shell 平台层同一策略） */
#define SVCRT_LOG_TX_SPIN_LIMIT   (64u)
/* Per-byte budget. svcrt_log_puts runs inside the SVC handler, so it has to */
/* return fast: when the board TX FIFO stays full the byte is dropped instead */
/* of stalling the whole system (no other task can be scheduled meanwhile). */

/* 前缀缓冲：颜色 + [级别] + [tag:行号] + 空格 */
#define SVCRT_LOG_HEAD_SIZE       (64)

/* 控制台句柄（-1 = 尚未打开） */
static int32  g_log_dev   = -1;

/* 运行期级别：初值取编译期配置，保证「配置里关掉的级别」不会因为
 * 忘了调用 svcrt_log_set_level() 而在运行期被打开 */
static uint32 g_log_level = (uint32)SVCRT_LOG_LEVEL;

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

/* 逐字节写：板级发送 FIFO 只有 128 字节，整串一次写会写不进去 */
static void svcrt_log_puts(const char *s)
{
    int32 dev = svcrt_log_dev();

    if((s == 0) || (dev < 0))
    {
        return;
    }

    while(*s != '\0')
    {
        uint32 spin = 0u;

        while((svcrt_dev_write_internal(dev, (uint8 *)s, 1) != 1) &&
              (spin < SVCRT_LOG_TX_SPIN_LIMIT))
        {
            spin++;
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

    svcrt_log_puts(head);
    svcrt_log_puts(msg);
    svcrt_log_puts("\r\n");
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
