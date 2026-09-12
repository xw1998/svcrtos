/**
* @brief SVCrtOS Driver SDK - 驱动开发接口
* @details 驱动有两种部署形态，注册/注销/计数接口相同，
*          驱动代码不需要区分自己是哪种形态：
*          由宏 SVCRT_DRV_USER_MODE 选择：
*          - 未定义（默认）：内核态模式，随内核一起编译，直接调用 svcrt_dev_register 等
*          - 已定义：用户态模式，编译为独立固件，经 SVC 0x14 与内核交互
*          用户态模式下任务/事件接口由 svcrt_drv_oslib.c 提供 SVC 封装。
*          本头文件不依赖任何 MCU 头文件，SDK 可独立使用。
*/

#ifndef __SVCRT_DRIVER_SDK_H__
#define __SVCRT_DRIVER_SDK_H__

#include "svcrt_types.h"
#include "svcrt_svc_call.h"

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

SVCRT_SVC_DECL_1(int32, SVCRT_SVC_DRV_MGR, svcrt_call_drv_mgr, uint32 *);

/* 以下任务/事件接口由 svcrt_drv_oslib.c 提供 SVC 封装 */
void   svcrt_task_wait(uint32 ms);
void   svcrt_task_wait_period(void);
void   svcrt_task_delay(uint32 us);
uint32 svcrt_get_time_ms(void);
int32  svcrt_event_create(char *name);
void   svcrt_event_wait(int32 handle, int32 timeout);
void   svcrt_event_set(int32 handle);

#else

extern int32 svcrt_dev_register(const char *name, svcrt_dev_drv_t *drv, uint32 dev_num);
extern int32 svcrt_dev_unregister(const char *name);
extern int32 svcrt_dev_get_count(void);

#endif

#endif
