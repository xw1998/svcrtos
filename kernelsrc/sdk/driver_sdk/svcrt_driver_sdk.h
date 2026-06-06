/**
* @brief SVCrtOS Driver SDK - 驱动开发接口
* @details 此文件是驱动开发者需要包含的唯一头文件。
*          提供驱动接口定义、驱动注册API和设备操作码定义。
*          支持两种编译模式：
*          - SVCRT_DRV_MODE_KERNEL: 内核态模式，驱动编译为静态库与内核链接
*          - SVCRT_DRV_MODE_USER:   用户态模式，驱动独立编译为固件，通过SVC注册
*          默认为内核态模式，用户态模式需定义 SVCRT_DRV_USER_MODE 宏。
*          此文件不依赖任何MCU头文件或内核内部头文件，驱动SDK可独立编译。
*/

#ifndef __SVCRT_DRIVER_SDK_H__
#define __SVCRT_DRIVER_SDK_H__

#include "svcrt_types.h"

#define SVCRT_DEV_CTRL_OPEN          (0x0001)
#define SVCRT_DEV_CTRL_CLOSE         (0x0002)
#define SVCRT_DEV_CTRL_SET_BAUD      (0x0010)
#define SVCRT_DEV_CTRL_SET_MODE      (0x0011)
#define SVCRT_DEV_CTRL_GET_STATUS    (0x0020)
#define SVCRT_DEV_CTRL_RESET         (0x0030)

#define SVCRT_DRV_OK                 (0)
#define SVCRT_DRV_ERROR              (-1)
#define SVCRT_DRV_BUSY               (-2)
#define SVCRT_DRV_TIMEOUT            (-3)
#define SVCRT_DRV_INVALID_PARAM      (-4)

typedef struct {
    uint32 block_size;
} svcrt_dev_hdr_t;

typedef svcrt_dev_hdr_t *(*svcrt_drv_open_func)(uint32 dev_id, uint32 param);
typedef int32            (*svcrt_drv_close_func)(svcrt_dev_hdr_t *obj);
typedef int32            (*svcrt_drv_read_func)(svcrt_dev_hdr_t *obj, uint8 *pdata, int32 len);
typedef int32            (*svcrt_drv_write_func)(svcrt_dev_hdr_t *obj, uint8 *pdata, int32 len);
typedef int32            (*svcrt_drv_ioctl_func)(svcrt_dev_hdr_t *obj, uint32 code, uint32 value);

typedef struct {
    svcrt_drv_open_func    drv_open;
    svcrt_drv_close_func   drv_close;
    svcrt_drv_read_func    drv_read;
    svcrt_drv_write_func   drv_write;
    svcrt_drv_ioctl_func   drv_ctrl;
} svcrt_dev_drv_t;

int32 svcrt_drv_register(const char *name, svcrt_dev_drv_t *drv, uint32 dev_num);
int32 svcrt_drv_unregister(const char *name);
int32 svcrt_drv_get_count(void);

#ifdef SVCRT_DRV_USER_MODE

#define SVCRT_SVC_DRV_MGR           (0x14)

int32 __svc(SVCRT_SVC_DRV_MGR) svcrt_call_drv_mgr(uint32 *p);

#else

extern int32 svcrt_dev_register(const char *name, svcrt_dev_drv_t *drv, uint32 dev_num);
extern int32 svcrt_dev_unregister(const char *name);
extern int32 svcrt_dev_get_count(void);

#endif

#endif
