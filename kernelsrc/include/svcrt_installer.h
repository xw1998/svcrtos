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
*          开启内核 Shell（SHELL_ENABLE == 1）时不要调用本函数：常驻安装任务
*          会与控制台抢同一个串口 FIFO，此时改用 svcrt_installer_run_once()
*          由 `install` 命令触发一次性窗口，串行占用串口。
*/
int32 svcrt_installer_init(void);

/**
* @brief 在指定设备上接收并安装一个镜像（阻塞式，带超时）
* @param dev        已打开的镜像接收设备句柄（控制台复用同一个句柄）
* @param timeout_ms 等待镜像头的超时（ms）；超时后清空接收状态并返回
* @return 成功返回槽位号（>=0），失败返回 SVCRT_LOADER_ERR_x
* @details 与常驻任务共用同一套同步/校验/落盘逻辑，区别只在调用方式：
*          本函数在一个任务上下文内同步跑完「等头 → 落盘 → 按自启标志启动」，
*          期间调用方不应再去读同一个设备。
*          是否自启由镜像头 flags（SVCRT_APP_FLAG_AUTOSTART）决定。
*/
int32 svcrt_installer_run_once(int32 dev, uint32 timeout_ms);

/**
* @brief Install an .svcapp that is already sitting in the mounted volume
* @param path absolute path inside the mounted volume, e.g. "/APP_DEMO.svcapp"
* @return slot id (>=0) on success, SVCRT_LOADER_ERR_x on failure
* @details This is the "install it from the device's own storage" entrance:
*          the image is read in chunks from littlefs and handed to the same
*          streaming loader the serial window uses, so no image ever has to
*          fit in RAM. A mounted volume is required; with none mounted it
*          returns SVCRT_LOADER_ERR_PARAM rather than guessing.
*          Whether the image starts is decided by the header flags, exactly as
*          on the serial path and at boot - one rule, three entrances.
*/
int32 svcrt_installer_from_file(const char *path);

#if (defined(__cplusplus))
}
#endif

#endif /* __SVCRT_INSTALLER_H__ */
