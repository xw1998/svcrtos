/**
* @brief SVCrtOS Driver SDK 基础类型定义
* @details 为 Driver SDK 提供与内核一致的基础整型别名，
*          不含任何 MCU 头文件与 OS 依赖，驱动可独立编译。
*          定义与 kernelsrc/include/svcrt_types.h 保持一致。
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
