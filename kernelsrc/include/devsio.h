/**
* @brief SVCrtOS 设备IO框架
* @details 提供统一的设备驱动接口和动态驱动注册机制
* @author chenjl
* @date 2019.07.05
*/

#ifndef __DEVIO_H__
#define __DEVIO_H__

#include "kerOs.h"
#include "svcrt_config.h"

typedef struct {
    uint32 blocksize;
}DEV_HDR;

typedef DEV_HDR* (*DrvOpenFunc)(uint32 devid,uint32 p);
typedef int32 (*DrvRWFunc)(DEV_HDR* obj,uint8* pdata,int32 len);
typedef int32 (*DrvIOCtrl)(DEV_HDR* obj,uint32 code,uint32 value);

typedef struct {
    DrvOpenFunc DrvOpen;
    DrvRWFunc   DrvRead;
    DrvRWFunc   DrvWrite;
    DrvIOCtrl   DrvCtrl;
}DRV_INTERFACE;

typedef struct {
    char dev_name[8];
    DRV_INTERFACE *drv;
    uint32  dev_num;
}DEV_DESCRIPT;

int32 kerDevOpen(char *name,uint32 param);
int32 kerDevRead(int32 handle,uint8 *pData,int32 len);
int32 kerDevWrite(int32 handle,uint8 *pData,int32 len);
int32 kerDevCtrl(int32 handle,uint32 code,uint32 value);

int32 kerDevRegister(const char *name, DRV_INTERFACE *drv, uint32 dev_num);
int32 kerDevUnregister(const char *name);
int32 kerDevGetCount(void);

#endif
