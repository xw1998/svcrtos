/**
* @brief SVCrtOS memory protection manager (kernel internal interface)
* @details Kernel code calls only this layer; all MPU register access lives in
*          the port layer (svcrt_port_mpu_*). Porting to a new architecture
*          therefore only needs a new port implementation.
*          The whole interface collapses to empty macros when SVCRT_USE_MPU == 0.
*/

#ifndef __SVCRT_MPU_H__
#define __SVCRT_MPU_H__

#include "svcrt_def.h"
#include "svcrt_hal.h"
#include "svcrt_task.h"
#include "svcrt_config.h"

/* Region index layout used by every task context built in svcrt_mpu.c. */
#define SVCRT_MPU_RGN_CODE       (0u)   /* task code window (read-only, executable) */
#define SVCRT_MPU_RGN_DATA       (1u)   /* task data/stack window (read/write, XN) */
#define SVCRT_MPU_RGN_SHARE      (2u)   /* kernel shared RAM (partition table) */
#define SVCRT_MPU_RGN_PERIPH1    (3u)   /* peripheral window 1 (board configured) */
#define SVCRT_MPU_RGN_PERIPH2    (4u)   /* peripheral window 2 (board configured) */

#if (SVCRT_USE_MPU == 1)

void svcrt_mpu_module_init(void);
void svcrt_mpu_set_app(svcrt_task_t *p_app);
void svcrt_mpu_set_idle(void);
void svcrt_mpu_build_task(svcrt_task_t *p_task);
void svcrt_mpu_reset(void);

#else

#define svcrt_mpu_module_init()
#define svcrt_mpu_set_app(p_app)
#define svcrt_mpu_set_idle()
#define svcrt_mpu_build_task(p_task)
#define svcrt_mpu_reset()

#endif

#endif
