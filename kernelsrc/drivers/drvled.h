/**
* @brief 系统指示灯模块
* @author chenjl
* @date 2019.07.05
*/

#ifndef __DRV_SYSLED_H__
#define __DRV_SYSLED_H__

#include "kerOs.h"
#include "devsio.h"

typedef struct {
    DEV_HDR hdr;
    uint8 opened;
    uint8 ledon;
}LED_DEV;

extern DRV_INTERFACE led_drv;

#endif
