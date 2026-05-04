/**
* @brief 系统指示灯模块
* @author xw
* @date 2026.05.03
*/

#ifndef __DRV_LED_H__
#define __DRV_LED_H__

#include "svcrt_def.h"
#include "svcrt_dev.h"

typedef struct {
    svcrt_dev_hdr_t hdr;
    uint8 opened;
    uint8 led_on;
} led_dev_t;

extern svcrt_dev_drv_t led_drv;

#endif
