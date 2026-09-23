/**
* @brief SVCrtOS 小程序基础类型定义
* @details 与 App / 驱动 SDK 的同名文件内容一致：不含任何 MCU 头文件、
*          不含内核内部头文件。小程序只需包含 svcrt.h 即可调用全部 OS 接口。
*/

#ifndef __SVCRT_TYPES_H__
#define __SVCRT_TYPES_H__

typedef unsigned long  uint32;
typedef signed   long  int32;
typedef unsigned short uint16;
typedef signed   short int16;
typedef unsigned char  uint8;
typedef signed   char  int8;

/* Toolchain-independent weak-symbol attribute (AC5 vs AC6/GCC/Clang). */
#if defined(__CC_ARM)
#define SVCRT_WEAK __weak
#else
#define SVCRT_WEAK __attribute__((weak))
#endif

#endif
