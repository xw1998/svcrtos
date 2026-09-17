/**
* @file svcrt_log_defs.h
* @brief 日志级别与颜色定义（内核侧与用户态共用）
* @details 内核日志（svcrt_log.h）与用户态日志（svcrt_ulog.h）必须对「级别
*          数值」有一致的理解，否则 shell 里 `log 2` 收紧到 WARNING 之后，
*          某一侧可能把 WARNING 当成别的语义。所以级别、编译期开关、
*          颜色这些「契约」放在同一个头文件里，两边各自 include。
*
*          本文件不声明任何函数、不依赖任何内核结构，App / 驱动固件可以
*          安全地把它编进去。
*/

#ifndef __SVCRT_LOG_DEFS_H__
#define __SVCRT_LOG_DEFS_H__

/* ============================================================
 * 日志级别
 * ============================================================ */

#define SVCRT_LOG_NONE      (0u)    /**< 关闭全部日志 */
#define SVCRT_LOG_ERROR     (1u)    /**< 错误：功能已经失败 */
#define SVCRT_LOG_WARNING   (2u)    /**< 警告：可疑但还能跑 */
#define SVCRT_LOG_INFO      (3u)    /**< 一般信息：关键状态变化 */
#define SVCRT_LOG_DEBUG     (4u)    /**< 调试：高频细节 */

/* 编译期级别：config/svcrt_partition.h 可覆盖；默认 INFO。
 * 用「级别数值」比较而非开关：设为 WARNING 时 ERROR 仍然保留。 */
#ifndef SVCRT_LOG_LEVEL
#define SVCRT_LOG_LEVEL     (SVCRT_LOG_INFO)
#endif

/* ============================================================
 * ANSI 颜色（终端不支持时只是显示转义字符，不影响日志内容）
 * ============================================================ */

#ifndef SVCRT_LOG_COLOR_ENABLE
#define SVCRT_LOG_COLOR_ENABLE  (1)
#endif

#if (SVCRT_LOG_COLOR_ENABLE == 1)
#define SVCRT_LOG_COLOR_ERROR   "\033[0;31m"
#define SVCRT_LOG_COLOR_WARNING "\033[0;33m"
#define SVCRT_LOG_COLOR_INFO    "\033[0;32m"
#define SVCRT_LOG_COLOR_DEBUG   "\033[0;34m"
#define SVCRT_LOG_COLOR_RESET   "\033[0m"
#else
#define SVCRT_LOG_COLOR_ERROR   ""
#define SVCRT_LOG_COLOR_WARNING ""
#define SVCRT_LOG_COLOR_INFO    ""
#define SVCRT_LOG_COLOR_DEBUG   ""
#define SVCRT_LOG_COLOR_RESET   ""
#endif

#endif /* __SVCRT_LOG_DEFS_H__ */
