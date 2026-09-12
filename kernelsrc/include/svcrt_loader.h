/**
* @file svcrt_loader.h
* @brief SVCrtOS App 镜像加载器（分区内加载 / 校验 / 启动）
* @details 负责把 App 镜像写入空闲槽位，并按镜像头信息校验后拉起为任务。
*          镜像格式见 svcrt_app_image.h；分区布局运行期从共享内存分区表获取
*          （见 svcrt_ptable.h），本模块不硬编码任何地址。
*
*          支持两种来源：
*          1) 内存缓冲区（svcrt_loader_load_buffer）——调试 / 自测最快路径；
*          2) 设备流式读取（svcrt_loader_load_dev）——从 UART / SD 等设备接收。
*
* @note 本文件属于内核内部接口；对外经 SVC 0x18（APP_MGR）暴露给用户态。
*/

#ifndef __SVCRT_LOADER_H__
#define __SVCRT_LOADER_H__

#include "svcrt_types.h"
#include "svcrt_app_image.h"

#if (defined(__cplusplus))
extern "C" {
#endif

/* ---- 错误码（成功返回槽位号，>=0；失败返回负值） ---- */
#define SVCRT_LOADER_ERR_PARAM    (-1)   /* 参数非法 */
#define SVCRT_LOADER_ERR_MAGIC    (-2)   /* 镜像头魔数错误 */
#define SVCRT_LOADER_ERR_COMPAT   (-3)   /* 硬件兼容签名不匹配 */
#define SVCRT_LOADER_ERR_SIZE     (-4)   /* 镜像长度非法或超出槽位 */
#define SVCRT_LOADER_ERR_CRC      (-5)   /* CRC32 校验失败 */
#define SVCRT_LOADER_ERR_FLASH    (-6)   /* Flash 擦写失败 */
#define SVCRT_LOADER_ERR_NO_SLOT  (-7)   /* 无空闲槽位 */
#define SVCRT_LOADER_ERR_TASK     (-8)   /* 任务注册失败 */
#define SVCRT_LOADER_ERR_STATE    (-9)   /* 槽位状态不允许该操作 */

/**
* @brief 从内存缓冲区加载完整 App 镜像
* @param image     指向镜像起始（含 256 字节头）的缓冲区
* @param image_len 缓冲区长度（字节），必须 >= 头长 + image_size
* @return 成功返回槽位号（>=0），失败返回 SVCRT_LOADER_ERR_x
*/
int32 svcrt_loader_load_buffer(const uint8 *image, uint32 image_len);

/**
* @brief 从设备流式加载 App 镜像
* @param dev       已打开的设备句柄（数据从镜像头开始）
* @param image_len 期望的镜像总长（含头）；传 0 表示由镜像头中的 image_size 决定
* @return 成功返回槽位号（>=0），失败返回 SVCRT_LOADER_ERR_x
* @note 边读边写 Flash，仅使用固定大小分块缓冲，不要求整镜像驻留 RAM。
*/
int32 svcrt_loader_load_dev(int32 dev, uint32 image_len);

/**
* @brief 扫描全部槽位，把 Flash 中已存在且校验通过的镜像标记为 LOADED
* @return 有效（可启动）的槽位数量
* @details 槽位状态原本只存在于共享 RAM，重启后会丢失；本函数在启动时按
*          镜像头魔数 + 硬件兼容签名 + CRC32 重新认定槽位，使“先烧录镜像、
*          再上电运行”的最小闭环成立。
*/
uint32 svcrt_loader_scan(void);

/**
* @brief 扫描驱动区（DRIVER_POOL），认定其中的驱动镜像
* @return 1=驱动镜像有效且可启动，0=无有效驱动
* @details 与 App 槽位共用同一套认定规则：带头的 .svcapp 或开发期裸镜像。
*/
uint32 svcrt_loader_scan_driver(void);

/**
* @brief 启动驱动区的驱动（单驱动：DRIVER_POOL 内一个入口）
* @return 成功返回任务号（>0），失败返回 SVCRT_LOADER_ERR_x
* @details 栈从 DRIVER_RAM 区顶部切出（与 App 同一套路），
*          参数取 DRIVER_TASK_PRIORITY / STACK_SIZE / PERIOD_MS。
*/
int32 svcrt_loader_start_driver(void);

/**
* @brief 从设备流式加载（镜像头已由调用方读出）
* @param dev       已打开的设备句柄，位置正好在镜像头之后
* @param p_hdr     已读出的镜像头（调用方已完成魔数同步）
* @param image_len 期望镜像总长（含头），传 0 表示由镜像头决定
* @return 成功返回槽位号（>=0），失败返回 SVCRT_LOADER_ERR_x
* @details 供安装任务使用：先逐字节同步到镜像头魔数，再把头交给本函数继续
*          流式写入负载，避免整镜像驻留 RAM。
*/
int32 svcrt_loader_load_dev_hdr(int32 dev, const svcrt_app_header_t *p_hdr, uint32 image_len);

/**
* @brief 按“崩溃重启上限”策略处理 App 任务故障
* @param task_id 发生故障的任务号
* @return 1=已达上限并已禁用该 App，0=未达上限（调用方应恢复/重启该 App），
*         -1=不属于任何 App 槽位（内核任务，调用方沿用默认处理）
* @details 计数按槽位累计：故障一次加一，达到 APP_CRASH_RESTART_MAX 后
*          将该槽位置为 INVALID 并让任务脱离调度（不再重启）。
*          重新安装镜像时计数清零。
* @note 计数保存在共享 RAM，掉电即清零，因此当前可挡住“App 反复崩溃重启”，
*       但擋不住“崩溃导致整机复位”的启动环——那需要把计数持久化（如备份寄存器）。
*/
int32 svcrt_loader_on_fault(int32 task_id);

/**
* @brief 把已加载的槽位拉起为任务
* @param slot 槽位号
* @return 成功返回任务号（>0），失败返回 SVCRT_LOADER_ERR_x
*/
int32 svcrt_loader_start(uint32 slot);

/**
* @brief 停止槽位对应的任务（镜像仍保留在 Flash）
* @param slot 槽位号
* @return 0=成功，负值为 SVCRT_LOADER_ERR_x
*/
int32 svcrt_loader_stop(uint32 slot);

/**
* @brief 查询槽位状态
* @param slot 槽位号
* @return SVCRT_APP_SLOT_x；槽位非法返回 0xffffffff
*/
uint32 svcrt_loader_state(uint32 slot);

#if (defined(__cplusplus))
}
#endif

#endif /* __SVCRT_LOADER_H__ */
