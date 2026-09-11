/**
* @brief SVCrtOS 应用层基础类型定义
* @details 此文件属于App SDK，仅包含基础类型定义，
*          不依赖任何MCU头文件或内核内部头文件。
*          应用程序只需包含 svcrt.h 即可使用所有OS接口。
*/

#ifndef __SVCRT_TYPES_H__
#define __SVCRT_TYPES_H__

typedef unsigned long  uint32;
typedef signed   long  int32;
typedef unsigned short uint16;
typedef signed   short int16;
typedef unsigned char  uint8;
typedef signed   char  int8;

#endif
