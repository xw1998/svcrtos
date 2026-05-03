/**
* @brief SVCrtOS Driver SDK - 驱动开发接口
* @details 此文件是驱动开发者需要包含的唯一头文件。
*          提供驱动接口定义、驱动注册API和设备操作码定义。
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
    uint32 blocksize;
} DEV_HDR;

typedef DEV_HDR* (*DrvOpenFunc)(uint32 devid, uint32 param);
typedef int32    (*DrvCloseFunc)(DEV_HDR *obj);
typedef int32    (*DrvReadFunc)(DEV_HDR *obj, uint8 *pdata, int32 len);
typedef int32    (*DrvWriteFunc)(DEV_HDR *obj, uint8 *pdata, int32 len);
typedef int32    (*DrvIOCtrlFunc)(DEV_HDR *obj, uint32 code, uint32 value);

typedef struct {
    DrvOpenFunc    DrvOpen;
    DrvCloseFunc   DrvClose;
    DrvReadFunc    DrvRead;
    DrvWriteFunc   DrvWrite;
    DrvIOCtrlFunc  DrvCtrl;
} SVCRT_DRV_INTERFACE;

int32 svcrtDrvRegister(const char *name, SVCRT_DRV_INTERFACE *drv, uint32 dev_num);
int32 svcrtDrvUnregister(const char *name);
int32 svcrtDrvGetCount(void);

#endif
