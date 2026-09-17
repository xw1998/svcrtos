/**
* @file svcrt_ushell.h
* @brief 用户态 Shell 控制台服务：描述符与接口（SVC 0x1A）
* @details 内核分发层需要按同样的内存布局读取用户传来的命令描述符，
*          用户侧要按同样的布局填写它，所以结构体与长度上限定义在这一个
*          头文件里，两侧共同包含，避免「各写一份、改一处忘一处」。
*
*          「用户态命令」是怎么跑起来的：
*            1) App / 驱动调用 svcrt_shell_cmd_register()，把描述符交给内核；
*            2) 内核把 name / help 复制进自己的 RAM（镜像卸载后不留悬垂指针），
*               并在 Shell 命令表里挂一条指向内核跳板的命令；
*            3) 用户在控制台敲这条命令时，内核跳板先确认处理器函数仍落在
*               某个已装载镜像的范围内，再调用它——镜像被卸载后命令会
*               自行失效并提示，而不是跳到已擦除的 Flash 上。
*
* @note 本头文件不含内核内部结构，App / 驱动固件可安全包含。
*/

#ifndef __SVCRT_USHELL_H__
#define __SVCRT_USHELL_H__

#include "svcrt_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 长度上限：内核按这些值复制/截断，超长不会越界，只是被截短 */
#define SVCRT_USHELL_NAME_MAX   (15)    /**< 命令名字符数上限（不含结尾 '\0'） */
#define SVCRT_USHELL_HELP_MAX   (47)    /**< 帮助文本字符数上限（不含结尾 '\0'） */

/* 错误码：与 SVC 分发层返回给用户的值一致 */
#define SVCRT_USHELL_OK         (0)
#define SVCRT_USHELL_ERR_PARAM  (-1)    /**< 描述符或字符串非法 */
#define SVCRT_USHELL_ERR_DUP    (-2)    /**< 命令名已存在 */
#define SVCRT_USHELL_ERR_FULL   (-3)    /**< 用户命令表已满 */
#define SVCRT_USHELL_ERR_NOTFND (-4)    /**< 未找到要注销的命令 */
#define SVCRT_USHELL_ERR_OFF    (-5)    /**< 内核未启用 Shell（SHELL_ENABLE=0） */

/**
* @brief 用户态 Shell 命令描述符
* @note 注册时内核会复制 name / help 指向的字符串，所以用户侧可以用
*       字符串常量，也可以在注册完成后释放自己的缓冲。
*       func 必须是注册者自身固件区内的函数：内核会按「调用者自己的
*       ROM 区间」校验，防止借 Shell 的特权态去执行别人的代码。
*/
typedef struct
{
    const char *name;                        /**< 命令名（区分大小写） */
    int       (*func)(int argc, char **argv);/**< 处理器：argc/argv 与内置命令同构 */
    const char *help;                        /**< 帮助文本，会出现在 help 列表里 */
    uint32      max_args;                    /**< 参数上限（含命令名），0 表示不限制 */
} svcrt_ushell_cmd_t;

/**
* @brief 在内核 Shell 控制台上注册一条命令
* @param cmd 描述符（必须位于用户可见 RAM）
* @return SVCRT_USHELL_OK 或负错误码
*/
int32 svcrt_shell_cmd_register(const svcrt_ushell_cmd_t *cmd);

/**
* @brief 注销一条命令（按名字，不区分大小写）
* @return SVCRT_USHELL_OK 或负错误码
*/
int32 svcrt_shell_cmd_unregister(const char *name);

/**
* @brief 向内核 Shell 控制台输出一段文本
* @param msg NUL 结尾字符串；是否换行由调用者决定（要换行就带 "\r\n"）
* @return SVCRT_USHELL_OK 或负错误码
*/
int32 svcrt_shell_print(const char *msg);

#ifdef __cplusplus
}
#endif

#endif /* __SVCRT_USHELL_H__ */
