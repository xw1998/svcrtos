/**
* @file svcrt_installer.h
* @brief SVCrtOS 内核内安装任务（方案A）
* @details 常驻任务从设备（默认 COM1）接收 SVCrtOS 镜像流并安装到空闲槽位，
*          使 App 的落位方式从「烧录器刷固件」变为「设备自己安装」。
*
*          接收协议（与 tools/pack_app.py 的输出直接对接）：
*          主机把 .svcapp（256 字节头 + 负载）原样连续发送到串口即可，
*          帧头无需额外封装——安装任务会在字节流中搜索镜像头魔数 "SVCA"
*          （小端字节序 41 43 56 53）完成同步，因此中间夹杂的杂散字节会被自动跳过。
*
*          安全性：安装内容必须通过镜像头校验（魔数 / 硬件兼容签名 / 长度 / CRC32）
*          才会被置为 LOADED；写入途中掉电或中断，槽位会停留在 INSTALLING，
*          由 svcrt_loader_scan() 依 CRC 判定为 INVALID，不会被启动。
*
* @note 本文件属于内核内部接口。
*/

#ifndef __SVCRT_INSTALLER_H__
#define __SVCRT_INSTALLER_H__

#include "svcrt_types.h"

#if (defined(__cplusplus))
extern "C" {
#endif

/**
* @brief 初始化并注册安装任务
* @return 成功返回任务号（>0）；INSTALLER_ENABLE 为 0 时返回 0
* @details 需在调度启动之前调用（建议跟在 svcrt_loader_scan() 之后）。
*/
int32 svcrt_installer_init(void);

#if (defined(__cplusplus))
}
#endif

#endif /* __SVCRT_INSTALLER_H__ */
