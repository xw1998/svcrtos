/**
* @brief mdk_trace instrumentation config - STM32F427 / SVCrtOS
* @details Picked up because the project defines MDK_TRACE_USE_CONFIG_FILE;
*          AC5 (armcc V5.06) does not answer __has_include, so without that
*          define this file would be silently skipped and the component would
*          fall back to its ITM/SWO default - it would still compile, but
*          nothing would ever leave the chip.
*
*          This board is wired to a DAPLink over SWD only (no SWO pin), so the
*          backend is swd: events are compressed on target into a RAM ring and
*          the host drains it incrementally with mdkdebug trace_swd_read. The
*          unread part of the ring is never overwritten, so a recording that
*          keeps up loses nothing.
*
*          Time granularity is a run time decision, not a build one: the host
*          writes TS_SHIFT / DT_UNIT / FLAGS.TS_OFF into the control block and
*          asks for a fresh recording. The clock below is only used to convert
*          the target side cycle count into microseconds.
*
*          Everything else keeps the component defaults
*          (kernelsrc/components/mdk_trace/mdk_trace_config_default.h):
*            MDK_TRACE_SWD_BYTES         8192   ring bytes = static RAM cost
*            MDK_TRACE_SWD_TS_SHIFT      0      full DWT resolution
*            MDK_TRACE_SWD_CLEAR_ON_INIT 1      clear the ring on power up
*/

#ifndef MDK_TRACE_CONFIG_H
#define MDK_TRACE_CONFIG_H

/* Master switch. 0 compiles the whole component down to empty functions. */
#define MDK_TRACE_ENABLE              1

/* Backend: compressed RAM ring, drained by the host over SWD. */
#define MDK_TRACE_BACKEND_SWD         1

/* Core clock, derived from SystemClock_Config() in
 * example/stm32f427/kernel/SVCRTOS_TEST/Core/Src/main.c:
 *   HSI 16 MHz / PLLM 16 = 1 MHz, x PLLN 192 = 192 MHz VCO, / PLLP 2 = 96 MHz.
 * FLASH_LATENCY_3 in the same function is the wait state count for a
 * 90..100 MHz SYSCLK on voltage scale 1, which agrees with 96 MHz.
 *
 * Note this is NOT board/stm32f427/svcrt_board_config.h's
 * SVCRT_SYSTEM_CLOCK_HZ (168 MHz). That macro describes the part's nominal
 * maximum rather than this project's HSI-fed PLL, and nothing in the kernel
 * reads it today - but the difference matters here, because this value ends
 * up in the host's microsecond conversion. Do not copy the 168.
 */
#define MDK_TRACE_CPU_HZ              96000000

/* Fault snapshot: extra pc/lr/sp/xpsr/hfsr/mmfar/bfar in the fault path.
 * NOTE: the F427 board file does not call mdk_trace_fault_capture() yet -
 * its four fault vectors still go straight to the kernel handlers - so this
 * switch has no producer on this board and is left at 0 to say so, rather
 * than 1 and producing an empty fault record the host would read as
 * "no fault happened". F401 has the capture wired. */
#define MDK_TRACE_FAULT_FRAME         0

#endif /* MDK_TRACE_CONFIG_H */
