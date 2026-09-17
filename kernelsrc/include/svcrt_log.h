/**
* @file svcrt_log.h
* @brief SVCrtOS 内核日志：分级输出 + 运行期过滤（内核侧接口）
* @details 设计沿用 mcu_framework 的 log 组件思路，并补上 SVCrtOS 需要的一环：
*
*            - 级别：NONE(0) < ERROR(1) < WARNING(2) < INFO(3) < DEBUG(4)，
*              数值越大越啰嗦（定义见 svcrt_log_defs.h，与用户态共用）。
*            - 编译期裁剪：SVCRT_LOG_LEVEL 低于某条日志的级别时，该宏整体展开为
*              空语句，格式化代码根本不进二进制（省 Flash 也省栈）。
*            - 运行期过滤：svcrt_log_set_level() 可随时收紧/放宽，配合 shell 的
*              `log <level>` 命令，现场不用重新烧写就能看细节。
*            - 输出格式：[级别字母][tag:行号] 正文，级别字母带 ANSI 颜色，
*              与终端里的其它彩色输出（shell 提示符、错误信息）风格一致。
*
*          与用户态（App / 驱动）的关系：
*            内核侧用本文件的 SVCRT_LOGE/W/I/D 宏，直接写串口，不做 SVC 切换；
*            用户态的对应接口在 svcrt_ulog.h，格式串在用户侧格式化后经
*            SVC 0x19 交内核，由内核统一做级别过滤与输出——
*            这样「运行期级别」是全局一致的，shell 改一次，内核与所有 App
*            的日志同时变。
*
* @note 输出走控制台串口（SHELL_DEV_NAME），首次使用时惰性打开。日志只能在
*       任务上下文调用：设备框架的写接口不保证可重入，中断里请用调试器变量。
*/

#ifndef __SVCRT_LOG_H__
#define __SVCRT_LOG_H__

#include "svcrt_types.h"
#include "svcrt_log_defs.h"
#include <stdio.h>              /* snprintf：日志宏在栈缓冲里完成格式化 */

#ifdef __cplusplus
extern "C" {
#endif

/* 单条日志正文缓冲（栈上分配，函数返回即释放，无动态内存） */
#ifndef SVCRT_LOG_BUF_SIZE
#define SVCRT_LOG_BUF_SIZE  (128)
#endif

/* ============================================================
 * 内核侧 API
 * ============================================================ */

/**
* @brief 日志模块初始化（打开控制台串口，装载配置里的初始级别）
* @details 需在设备框架初始化之后、调度启动之前调用；重复调用无害。
*/
void   svcrt_log_init(void);

/**
* @brief 设置运行期日志级别
* @param level SVCRT_LOG_NONE..SVCRT_LOG_DEBUG
* @note 超出范围的值不改变当前级别（避免误配导致全体静音）。
*/
void   svcrt_log_set_level(uint32 level);

/**
* @brief 取当前运行期日志级别
*/
uint32 svcrt_log_get_level(void);

/**
* @brief 输出一条已完成格式化的日志（内核内部与 SVC 0x19 共用此出口）
* @param level 级别；高于当前运行期级别时直接丢弃
* @param tag   标签（短字符串，通常是大写模块名）
* @param line  源码行号；用户态传 0 时不打印行号
* @param msg   正文（已格式化，为空时直接返回）
*/
void   svcrt_log_emit(uint32 level, const char *tag, uint32 line, const char *msg);

/**
* @brief SVC 0x19 的服务入口（用户态 App / 驱动调用）
* @param level 级别
* @param tag   标签字符串地址（已由分发层校验）
* @param msg   正文地址（已由分发层校验）
* @return 0=已处理（含被级别过滤），-1=参数非法
*/
int32  svcrt_log_svc(uint32 level, const char *tag, const char *msg);

#ifdef __cplusplus
}
#endif

/* ============================================================
 * 内核侧日志宏
 *
 * 用法：
 *     SVCRT_LOGI("LOADER", "slot %u installed, %u bytes", slot, size);
 *
 * 展开后先做编译期判定（级别不够则整条消失），再做运行期判定，
 * 最后格式化到栈缓冲再交给 svcrt_log_emit()。
 * ============================================================ */

#define SVCRT_LOG_EMIT_(lvl, tag, ...)                                    \
    do {                                                                  \
        char svcrt_log_buf_[SVCRT_LOG_BUF_SIZE];                          \
        (void)snprintf(svcrt_log_buf_, sizeof(svcrt_log_buf_), __VA_ARGS__); \
        svcrt_log_emit((lvl), (tag), (uint32)__LINE__, svcrt_log_buf_);    \
    } while(0)

#if (SVCRT_LOG_LEVEL >= SVCRT_LOG_ERROR)
#define SVCRT_LOGE(tag, ...)                                              \
    do {                                                                  \
        if(svcrt_log_get_level() >= SVCRT_LOG_ERROR)                      \
        {                                                                 \
            SVCRT_LOG_EMIT_(SVCRT_LOG_ERROR, tag, __VA_ARGS__);            \
        }                                                                 \
    } while(0)
#else
#define SVCRT_LOGE(tag, ...)    do { } while(0)
#endif

#if (SVCRT_LOG_LEVEL >= SVCRT_LOG_WARNING)
#define SVCRT_LOGW(tag, ...)                                              \
    do {                                                                  \
        if(svcrt_log_get_level() >= SVCRT_LOG_WARNING)                    \
        {                                                                 \
            SVCRT_LOG_EMIT_(SVCRT_LOG_WARNING, tag, __VA_ARGS__);          \
        }                                                                 \
    } while(0)
#else
#define SVCRT_LOGW(tag, ...)    do { } while(0)
#endif

#if (SVCRT_LOG_LEVEL >= SVCRT_LOG_INFO)
#define SVCRT_LOGI(tag, ...)                                              \
    do {                                                                  \
        if(svcrt_log_get_level() >= SVCRT_LOG_INFO)                       \
        {                                                                 \
            SVCRT_LOG_EMIT_(SVCRT_LOG_INFO, tag, __VA_ARGS__);             \
        }                                                                 \
    } while(0)
#else
#define SVCRT_LOGI(tag, ...)    do { } while(0)
#endif

#if (SVCRT_LOG_LEVEL >= SVCRT_LOG_DEBUG)
#define SVCRT_LOGD(tag, ...)                                              \
    do {                                                                  \
        if(svcrt_log_get_level() >= SVCRT_LOG_DEBUG)                      \
        {                                                                 \
            SVCRT_LOG_EMIT_(SVCRT_LOG_DEBUG, tag, __VA_ARGS__);            \
        }                                                                 \
    } while(0)
#else
#define SVCRT_LOGD(tag, ...)    do { } while(0)
#endif

#endif /* __SVCRT_LOG_H__ */
