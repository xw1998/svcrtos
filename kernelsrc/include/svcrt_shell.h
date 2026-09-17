/**
* @file svcrt_shell.h
* @brief SVCrtOS 内核 Shell 控制台（ark-shell 移植）对外接口
* @details 控制台任务独占 SHELL_DEV_NAME 串口，提供 App/驱动/任务的启停与
*          查询、故障读数、串口安装等人机命令。
*
*          串口归属约定：开启 shell 后不再注册常驻安装任务（见
*          svcrt_installer.c 的说明），安装改由 `install` 命令触发一次性窗口，
*          在同一任务内串行执行，避免两个读者抢同一个串口 FIFO。
*
* @note 本文件属于内核内部接口。
*/

#ifndef __SVCRT_SHELL_H__
#define __SVCRT_SHELL_H__

#include "svcrt_types.h"
#include "svcrt_ushell.h"

#if (defined(__cplusplus))
extern "C" {
#endif

/**
* @brief 初始化并注册内核 Shell 控制台任务
* @return 成功返回任务号（>0）；SHELL_ENABLE 为 0 时返回 0
* @details 需在调度启动之前调用（紧跟 svcrt_loader_start_autostart*() 之后）。
*/
int32 svcrt_shell_init(void);

/**
* @brief 取控制台已打开的串口句柄
* @return 设备句柄；尚未打开时返回 -1
* @details 安装窗口复用同一个句柄收镜像，语义上比再 open 一次更清楚。
*/
int32 svcrt_shell_uart_handle(void);

/**
* @brief 打开控制台串口（已打开则直接返回既有句柄）
* @return 设备句柄；打开失败返回 -1
*/
int32 svcrt_shell_uart_open(void);

/**
* @brief 往内核控制台注册一条用户态（App / 驱动）命令
* @param cmd 用户态描述符；name / help 会被复制进内核 RAM，
*            所以镜像卸载后不会留下指向已擦除 Flash 的悬垂指针
* @return SVCRT_USHELL_OK 或负错误码（见 svcrt_ushell.h）
* @details 供内核分发层（SVC 0x1A 子命令 1）调用；用户态请用
*          svcrt_shell_cmd_register()，不要直接调用本函数。
*/
int32 svcrt_shell_ext_register(const svcrt_ushell_cmd_t *cmd);

/**
* @brief 注销一条用户态命令（按名字，不区分大小写）
* @return SVCRT_USHELL_OK 或负错误码
*/
int32 svcrt_shell_ext_unregister(const char *name);

/**
* @brief 向控制台输出一段有界长度的文本（不追加换行）
* @param msg 文本首地址
* @param len 输出长度；由分发层先探测上限，避免在内核侧做无界 strlen
* @return SVCRT_USHELL_OK 或负错误码
* @details SVC 处理运行在用户任务的 PSP 上（App 栈 4K、驱动栈 1K），
*          因此这里用内核静态缓冲分段输出，不在调用者栈上开临时数组。
*/
int32 svcrt_shell_print_n(const char *msg, uint32 len);


#if (defined(__cplusplus))
}
#endif

#endif /* __SVCRT_SHELL_H__ */
