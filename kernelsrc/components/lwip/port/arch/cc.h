/**
* @file cc.h
* @brief lwIP 的"编译器/平台"面：类型、字节序、打包、诊断出口。
* @details 只写这套工具链会踩到的东西，其余留给 lwIP 的默认值。
*          两条与 AC5 相关的判断都不要删：
*            - 打包结构用 `__packed` 前缀（AC5 的原生写法），而不是 GCC 那个
*              后缀 attribute —— AC5 不认后者，认了也是空宏，协议结构会静默
*              按对齐排布；
*            - 诊断出口不允许落到 printf：内核里没有半主机重定向，printf 会去
*              敲一个不存在的调试器。LWIP_PLATFORM_ASSERT / DIAG 在
*              lwipopts.h 里已经改成内核日志与钩子。
*
* @author xw
* @date 2026.09.23
*/
#ifndef LWIP_ARCH_CC_H
#define LWIP_ARCH_CC_H

#include <stdint.h>

/* 工具链的 <unistd.h> 是半主机那一套，lwIP 不需要，也不该把内核拖进去。 */
#define LWIP_NO_UNISTD_H   1

/* sockets 层要 errno。用 lwIP 自带那份（它自己定义 errno 变量），
 * 免得链进工具链的 <errno.h>——那份没有 ENOENT 这类 POSIX 码。 */
#define LWIP_PROVIDE_ERRNO 1

/* ---- 字节序 ---- */
#define LITTLE_ENDIAN       1234
#define BIG_ENDIAN          4321
#define BYTE_ORDER          LITTLE_ENDIAN

/* ---- 打包 ---- */
#if defined(__ARMCC_VERSION) && (__ARMCC_VERSION < 6000000)
/* AC5：打包限定符写在 struct 关键字之前。 */
#define PACK_STRUCT_BEGIN   __packed
#define PACK_STRUCT_STRUCT
#else
/* AC6 / GCC / clang：写在大括号之后。 */
#define PACK_STRUCT_BEGIN
#define PACK_STRUCT_STRUCT  __attribute__((packed))
#endif
#define PACK_STRUCT_FIELD(x)  x

#define LWIP_PLATFORM_BYTESWAP  0

#endif /* LWIP_ARCH_CC_H */
