/**
* @brief SVCrtOS ???????????
* @details ????RT-Thread??rtconfig.h?????ß€???????????????????
*          ???????????????????????
*/

#ifndef __SVCRT_CONFIG_H__
#define __SVCRT_CONFIG_H__

/* ============================================================
 * CPU???????????????
 * ============================================================ */
#define SVCRT_ARCH_CORTEX_M3      0
#define SVCRT_ARCH_CORTEX_M4      1
#define SVCRT_ARCH_CORTEX_M7      2

#define SVCRT_CPU_ARCH            SVCRT_ARCH_CORTEX_M4

/* ============================================================
 * FPU ?????????
 * ============================================================ */
#if (SVCRT_CPU_ARCH == SVCRT_ARCH_CORTEX_M3)
#define SVCRT_USE_FPU             0
#else
#define SVCRT_USE_FPU             1
#endif

/* ============================================================
 * MPU ??õC?????????
 * ============================================================ */
#if (SVCRT_CPU_ARCH == SVCRT_ARCH_CORTEX_M3)
#define SVCRT_USE_MPU             0
#else
#define SVCRT_USE_MPU             1
#endif

/* ============================================================
 * ?????????????
 * @note ????MPU????MPU??????
 * ============================================================ */
#if (SVCRT_USE_MPU == 1)
#define SVCRT_USE_PRIV            1
#else
#define SVCRT_USE_PRIV            0
#endif

/* ============================================================
 * ??????????
 * ============================================================ */
#define SVCRT_TASK_MAX_NUM        (7)
#define SVCRT_TICK_PERIOD_US      (500)
#define SVCRT_EVENT_NUM           (10)
#define SVCRT_MAX_EVENT_WAITERS   (4)

#define SVCRT_MS_TO_TICK(ms)      ((ms) * 1000 / SVCRT_TICK_PERIOD_US)

/* ============================================================
 * ?ıÙ???????????
 * ============================================================ */
#define SVCRT_DEV_MAX_NUM         (8)

/* ============================================================
 * CPU???????
 * ============================================================ */
#define SVCRT_USE_CPU_LOAD        1

/* ============================================================
 * ???????
 * ============================================================ */
#define SVCRT_USE_STACK_CHECK     1
#define SVCRT_STACK_END_FLAG      (0xed01)

/* ============================================================
 * ???????????
 * @brief ????????????????????? board ???
 *        svcrt_board_config.h ?????????????????
 * ============================================================ */
#ifndef SVCRT_SHARE_MEM_ADDR
#define SVCRT_SHARE_MEM_ADDR      (0x20028000)
#endif

#ifndef SVCRT_SHARE_MEM_SIZE
#define SVCRT_SHARE_MEM_SIZE      (0x8000)
#endif

/* ============================================================
 * ?????
 * @brief ????????? SystemCoreClock ????????????????
 *        ?????? board/svcrt_board_config.h ?ßÿ?????????
 * ============================================================ */
#ifndef SVCRT_SYSTEM_CLOCK_HZ
#define SVCRT_SYSTEM_CLOCK_HZ     (168000000)
#endif

/* ============================================================
 * ?èâ???????
 * @brief ??????????????? SVCRT_BOARD_CONFIG????????èâ???????
 *        ?èâ???????????????????????????
 * ============================================================ */
#ifdef SVCRT_BOARD_CONFIG
#include SVCRT_BOARD_CONFIG
#endif

#endif
