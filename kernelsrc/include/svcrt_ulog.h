/**
* @file svcrt_ulog.h
* @brief SVCrtOS 用户态服务接口：日志（SVC 0x19）与控制台 Shell（SVC 0x1A）
* @details 这是 App / 驱动固件侧唯一需要包含的两个「内核外设」头文件之一
*          （另一个是 svcrt.h / svcrt_driver_sdk.h）。它同时解决两件事：
*
*          1) 日志
*             用户态代码不允许直接碰串口，也不能调用内核的日志实现。
*             这里给出的做法是：在用户侧把格式串渲染成完整字符串，
*             再通过一条 SVC 交给内核输出。内核统一做运行期级别过滤，
*             于是 shell 里 `log 3` 一次调整，内核与所有 App/驱动的
*             日志级别同时生效，不需要各模块各自维护开关。
*
*             为什么不直接用 C 库的 vsnprintf：
*             - 会把整套 printf 机器（数 KB）链进只有几 KB 的 App 镜像；
*             - 库内部若含有地址相关的初始化/跳转表，还会给动态装载的
*               重定位环节引入额外的、难以穷举的差异点。
*             所以这里自带一个只覆盖常用子集的小格式化器：%d %i %u %x %X
*             %s %c %%，支持 '0' 补零与十进制宽度（如 %08X）。够用、可预期、
*             体积恒定。
*
*          2) Shell 控制台
*             用户态程序可以在内核控制台上注册自己的命令、打印输出，
*             于是「驱动自检」「App 状态查询」这类命令可以跟着镜像一起
*             安装/卸载，而不必改内核。
*             处理器函数由内核 Shell 任务以特权态调用，因此注册时内核会
*             校验它是否落在注册者自己的固件区内（与定时器回调同一策略）；
*             命令名与帮助文本会被复制进内核 RAM，镜像卸载后不会留下
*             指向已擦除 Flash 的悬垂指针。
*
* @note 本头文件不含任何内核内部结构，可安全编入用户态固件。
*/

#ifndef __SVCRT_ULOG_H__
#define __SVCRT_ULOG_H__

#include "svcrt_types.h"
#include "svcrt_log_defs.h"
#include "svcrt_ushell.h"        /* 描述符与接口（与内核分发层共用同一份定义） */
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 用户侧格式化缓冲（栈上，无动态内存）。内核把渲染好的整串直接输出，
 * 不再重排格式，因此这个大小不必与内核的 SVCRT_LOG_BUF_SIZE 相等。 */
#ifndef SVCRT_UFMT_BUF_SIZE
#define SVCRT_UFMT_BUF_SIZE (128)
#endif

/* 内联限定：AC5 只认 __inline；两者都接受 unused 属性，避免未使用告警 */
#define SVCRT_UFMT_INLINE  static __inline __attribute__((unused))

/* ============================================================
 * 迷你格式化器（常用子集）
 * ============================================================ */

SVCRT_UFMT_INLINE uint32 svcrt_ufmt_put(char *out, uint32 size, uint32 pos, char c)
{
    if(pos < (size - 1u))
    {
        out[pos] = c;           /* 始终预留一个字节给结尾的 '\0' */
    }
    return pos + 1u;
}

SVCRT_UFMT_INLINE uint32 svcrt_ufmt_num(char *out, uint32 size, uint32 pos,
                                        uint32 v, uint32 base, uint32 upper,
                                        uint32 width, uint32 zero)
{
    char tmp[12];
    const char *dig = (upper != 0u) ? "0123456789ABCDEF" : "0123456789abcdef";
    uint32 n = 0u;
    uint32 pad = 0u;
    uint32 i;

    do
    {
        tmp[n] = dig[v % base];
        n++;
        v /= base;
    } while((v != 0u) && (n < (uint32)sizeof(tmp)));

    if(zero != 0u)
    {
        while((n < width) && (n < (uint32)sizeof(tmp)))
        {
            tmp[n] = '0';
            n++;
        }
    }
    else if(width > n)
    {
        pad = width - n;
    }

    for(i = 0u; i < pad; i++)
    {
        pos = svcrt_ufmt_put(out, size, pos, ' ');
    }
    for(i = n; i > 0u; i--)
    {
        pos = svcrt_ufmt_put(out, size, pos, tmp[i - 1u]);
    }

    return pos;
}

/**
* @brief 格式化核心：参数已取成 va_list（供带 ... 的包装函数复用）
* @return 实际写入的字符数（不含结尾 '\0'）
*/
SVCRT_UFMT_INLINE int svcrt_ufmt_v(char *out, uint32 size, const char *fmt, va_list ap)
{
    uint32 pos = 0u;

    if((out == 0) || (size == 0u))
    {
        return 0;
    }
    if(fmt == 0)
    {
        out[0] = '\0';
        return 0;
    }

    while((*fmt != '\0') && (pos < (size - 1u)))
    {
        uint32 zero = 0u;
        uint32 width = 0u;

        if(*fmt != '%')
        {
            pos = svcrt_ufmt_put(out, size, pos, *fmt);
            fmt++;
            continue;
        }

        fmt++;

        while((*fmt == '0') || (*fmt == '-'))
        {
            if(*fmt == '0')
            {
                zero = 1u;
            }
            fmt++;
        }
        while((*fmt >= '0') && (*fmt <= '9'))
        {
            width = width * 10u + (uint32)(*fmt - '0');
            fmt++;
        }
        if((*fmt == 'l') || (*fmt == 'h'))      /* 32 位平台上按 32 位处理 */
        {
            fmt++;
        }

        switch(*fmt)
        {
        case 'd':
        case 'i':
        {
            int32 v = (int32)va_arg(ap, int);
            uint32 uv;

            if(v < 0)
            {
                pos = svcrt_ufmt_put(out, size, pos, '-');
                /* 取绝对值时先 +1，避免 INT32_MIN 取反溢出 */
                uv = (uint32)(-(v + 1)) + 1u;
                if(width > 0u)
                {
                    width--;
                }
            }
            else
            {
                uv = (uint32)v;
            }
            pos = svcrt_ufmt_num(out, size, pos, uv, 10u, 0u, width, zero);
            break;
        }
        case 'u':
            pos = svcrt_ufmt_num(out, size, pos, (uint32)va_arg(ap, unsigned int),
                                 10u, 0u, width, zero);
            break;
        case 'x':
            pos = svcrt_ufmt_num(out, size, pos, (uint32)va_arg(ap, unsigned int),
                                 16u, 0u, width, zero);
            break;
        case 'X':
            pos = svcrt_ufmt_num(out, size, pos, (uint32)va_arg(ap, unsigned int),
                                 16u, 1u, width, zero);
            break;
        case 'p':
            pos = svcrt_ufmt_put(out, size, pos, '0');
            pos = svcrt_ufmt_put(out, size, pos, 'x');
            pos = svcrt_ufmt_num(out, size, pos, (uint32)va_arg(ap, void *),
                                 16u, 0u, 8u, 1u);
            break;
        case 'c':
            pos = svcrt_ufmt_put(out, size, pos, (char)va_arg(ap, int));
            break;
        case 's':
        {
            const char *s = va_arg(ap, const char *);
            uint32 pad = 0u;
            uint32 len = 0u;

            if(s == 0)
            {
                s = "(null)";
            }
            while(s[len] != '\0')
            {
                len++;
            }
            if(width > len)
            {
                pad = width - len;
            }
            while((pad > 0u) && (pos < (size - 1u)))
            {
                pos = svcrt_ufmt_put(out, size, pos, ' ');
                pad--;
            }
            while((*s != '\0') && (pos < (size - 1u)))
            {
                pos = svcrt_ufmt_put(out, size, pos, *s);
                s++;
            }
            break;
        }
        case '%':
            pos = svcrt_ufmt_put(out, size, pos, '%');
            break;
        default:
            /* 不认识的转换符：原样输出，便于发现用了不支持的格式 */
            pos = svcrt_ufmt_put(out, size, pos, '%');
            if(*fmt != '\0')
            {
                pos = svcrt_ufmt_put(out, size, pos, *fmt);
            }
            break;
        }

        if(*fmt != '\0')
        {
            fmt++;
        }
    }

    out[(pos < size) ? pos : (size - 1u)] = '\0';
    return (int)((pos < size) ? pos : (size - 1u));
}

/**
* @brief 把格式化结果写入 out（最多 size-1 个字符 + '\0'）
* @return 实际写入的字符数（不含结尾 '\0'）
*/
SVCRT_UFMT_INLINE int svcrt_ufmt(char *out, uint32 size, const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = svcrt_ufmt_v(out, size, fmt, ap);
    va_end(ap);

    return n;
}

/* ============================================================
 * 日志：SVC 0x19
 * ============================================================ */

/**
* @brief 输出一条日志（级别过滤在核内统一完成）
* @param level SVCRT_LOG_NONE..SVCRT_LOG_DEBUG
* @param tag   标签（短字符串，NUL 结尾，位于调用者自身固件或 RAM 均可）
* @param msg   正文（NUL 结尾）
* @return 0=已提交内核；-1=参数被内核拒绝
*/
int32 svcrt_log_print(uint32 level, const char *tag, const char *msg);

/**
* @brief 格式化并输出一条日志
* @param level 级别
* @param tag   标签
* @param fmt   格式串（子集见文件头说明）
* @note 在用户侧完成格式化，只需一条 SVC；不需要额外的符号实现。
*/
SVCRT_UFMT_INLINE int32 svcrt_log_printf(uint32 level, const char *tag, const char *fmt, ...)
{
    char buf[SVCRT_UFMT_BUF_SIZE];
    va_list ap;

    va_start(ap, fmt);
    (void)svcrt_ufmt_v(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    return svcrt_log_print(level, tag, buf);
}

#define SVCRT_ULOG_DO_(lvl, tag, ...)                                          \
    do {                                                                       \
        char svcrt_ulog_buf_[SVCRT_UFMT_BUF_SIZE];                             \
        (void)svcrt_ufmt(svcrt_ulog_buf_, sizeof(svcrt_ulog_buf_), __VA_ARGS__); \
        (void)svcrt_log_print((lvl), (tag), svcrt_ulog_buf_);                  \
    } while(0)

#if (SVCRT_LOG_LEVEL >= SVCRT_LOG_ERROR)
#define SVCRT_ULOG_E(tag, ...)  SVCRT_ULOG_DO_(SVCRT_LOG_ERROR, tag, __VA_ARGS__)
#else
#define SVCRT_ULOG_E(tag, ...)  do { } while(0)
#endif

#if (SVCRT_LOG_LEVEL >= SVCRT_LOG_WARNING)
#define SVCRT_ULOG_W(tag, ...)  SVCRT_ULOG_DO_(SVCRT_LOG_WARNING, tag, __VA_ARGS__)
#else
#define SVCRT_ULOG_W(tag, ...)  do { } while(0)
#endif

#if (SVCRT_LOG_LEVEL >= SVCRT_LOG_INFO)
#define SVCRT_ULOG_I(tag, ...)  SVCRT_ULOG_DO_(SVCRT_LOG_INFO, tag, __VA_ARGS__)
#else
#define SVCRT_ULOG_I(tag, ...)  do { } while(0)
#endif

#if (SVCRT_LOG_LEVEL >= SVCRT_LOG_DEBUG)
#define SVCRT_ULOG_D(tag, ...)  SVCRT_ULOG_DO_(SVCRT_LOG_DEBUG, tag, __VA_ARGS__)
#else
#define SVCRT_ULOG_D(tag, ...)  do { } while(0)
#endif

/* ============================================================
 * Shell 控制台服务：SVC 0x1A
 *
 * 描述符 svcrt_ushell_cmd_t 与三个接口的声明在 svcrt_ushell.h
 * （内核分发层要按同样的布局读描述符，所以定义只能有一份）。
 * 这里只提供在用户侧完成格式化的方便宏。
 * ============================================================ */

/**
* @brief 格式化并打印到内核 Shell 控制台
* @note 与 svcrt_log_printf 同样在用户侧完成格式化。
*/
SVCRT_UFMT_INLINE int32 svcrt_shell_printf(const char *fmt, ...)
{
    char buf[SVCRT_UFMT_BUF_SIZE];
    va_list ap;

    va_start(ap, fmt);
    (void)svcrt_ufmt_v(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    return svcrt_shell_print(buf);
}

#define SVCRT_USHELL_PRINTF(...)                                               \
    do {                                                                       \
        char svcrt_ulog_buf_[SVCRT_UFMT_BUF_SIZE];                             \
        (void)svcrt_ufmt(svcrt_ulog_buf_, sizeof(svcrt_ulog_buf_), __VA_ARGS__); \
        (void)svcrt_shell_print(svcrt_ulog_buf_);                              \
    } while(0)

#ifdef __cplusplus
}
#endif

#endif /* __SVCRT_ULOG_H__ */
