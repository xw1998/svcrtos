/**
* @brief SVCrtOS 设备驱动框架（内核内部）
* @details 提供统一的设备驱动接口和动态驱动注册机制
*          此文件仅供内核内部使用，应用程序应使用 svcrt.h
*/

#ifndef __SVCRT_DEV_H__
#define __SVCRT_DEV_H__

#include "svcrt_def.h"
#include "svcrt_config.h"

typedef struct {
    uint32 block_size;
} svcrt_dev_hdr_t;

typedef svcrt_dev_hdr_t *(*svcrt_drv_open_func)(uint32 dev_id, uint32 param);
typedef int32            (*svcrt_drv_close_func)(svcrt_dev_hdr_t *obj);
typedef int32            (*svcrt_drv_rw_func)(svcrt_dev_hdr_t *obj, uint8 *pdata, int32 len);
typedef int32            (*svcrt_drv_ioctl_func)(svcrt_dev_hdr_t *obj, uint32 code, uint32 value);

typedef struct {
    svcrt_drv_open_func    drv_open;
    svcrt_drv_close_func   drv_close;
    svcrt_drv_rw_func      drv_read;
    svcrt_drv_rw_func      drv_write;
    svcrt_drv_ioctl_func   drv_ctrl;
} svcrt_dev_drv_t;

typedef struct {
    char dev_name[8];
    svcrt_dev_drv_t *drv;
    uint32 dev_num;
} svcrt_dev_desc_t;

void   svcrt_dev_module_init(void);
int32  svcrt_dev_register(const char *name, svcrt_dev_drv_t *drv, uint32 dev_num);
int32  svcrt_dev_unregister(const char *name);
int32  svcrt_dev_get_count(void);

/* 按设备表槽位索引取设备名，供 devfs 枚举用。
 * idx  为槽位索引(0 ~ SVCRT_DEV_MAX_NUM-1)，与 open/close 的 handle 同一套编号。
 * 空槽位或参数非法返回 -1，且 out[0]=0（不填空值）。
 * 成功写出返回 0。 */
int32  svcrt_dev_name_at(uint32 idx, char *out, uint32 max);

int32  svcrt_dev_open_internal(char *name, uint32 param);
int32  svcrt_dev_close_internal(int32 handle);
int32  svcrt_dev_read_internal(int32 handle, uint8 *pdata, int32 len);
int32  svcrt_dev_write_internal(int32 handle, uint8 *pdata, int32 len);
int32  svcrt_dev_ctrl_internal(int32 handle, uint32 code, uint32 value);

void   svcrt_dev_board_init(void);

#endif
