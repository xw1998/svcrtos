/* ============================================================
 * svcrt_trace.h - Kernel event trace facade
 *
 * Purpose
 *   Give the kernel one small API to instrument itself with, and
 *   record those events with the mdk_trace component's SWD backend:
 *   events are compressed into a RAM ring and drained by the host
 *   over SWD, incrementally, without ever skipping a run.
 *
 * Why not a private ring
 *   This module used to own a 6-byte-per-record ring and dump it as
 *   hex over the console. That works, but it cannot be seamless: a
 *   host either asks for the whole buffer (and loses whatever the
 *   ring overwrote meanwhile) or polls (and pays a UART transfer per
 *   sample). The SWD backend solves both with compression plus a
 *   drain cursor the target honours, so the ring is never overwritten
 *   where the host has not read yet. Keeping one implementation also
 *   keeps the on-target encoder and the host decoder from drifting.
 *
 * Host side
 *   trace_swd_status  -> ring capacity, cursors, loss, compression
 *   trace_swd_read    -> drain a slice, decode it into a timeline
 *   trace_swd_reset   -> ask the target to start a new recording
 *
 * event ids
 *   0x01..0x7F  system events defined below
 *   0x80..0xEF  spare (app / driver instrumentation)
 *   0xFExx      reserved for the component's fault register dump
 * ============================================================ */
#ifndef SVCRT_TRACE_H
#define SVCRT_TRACE_H

#include "svcrt_types.h"
#include "mdk_trace.h"

/* ---------------- event ids ---------------- */
#define SVCRT_TR_EV_NONE        0x00u
#define SVCRT_TR_EV_MARK        0x01u   /* arg = user value                     */
#define SVCRT_TR_EV_SWITCH      0x10u   /* arg = from | (to << 4)               */
#define SVCRT_TR_EV_WAIT        0x11u   /* arg = 0-based task idx going to sleep */
#define SVCRT_TR_EV_READY       0x12u   /* arg = task id made ready             */
#define SVCRT_TR_EV_CREATE      0x13u   /* arg = task id                        */
#define SVCRT_TR_EV_EXIT        0x14u   /* arg = task id                        */
#define SVCRT_TR_EV_TICK        0x15u   /* periodic heartbeat, off by default   */
#define SVCRT_TR_EV_ISR_ENTER   0x20u   /* arg = IRQ number                     */
#define SVCRT_TR_EV_ISR_EXIT    0x21u   /* arg = IRQ number                     */
#define SVCRT_TR_EV_SVC         0x30u   /* arg = SVC function number            */
#define SVCRT_TR_EV_EXT         0xFFu   /* escape, see header comment           */

/* Task ids are 4 bits wide inside a SWITCH payload.
 * 0x0F marks the idle context; ids 0..14 are task table indices. */
#define SVCRT_TR_ID_MAX         14u
#define SVCRT_TR_ID_IDLE        0x0Fu
#define SVCRT_TR_SW_PACK(from, to)  ((uint8)((((from) & 0x0Fu) << 4) | ((to) & 0x0Fu)))
#define SVCRT_TR_SW_FROM(arg)       ((uint8)(((arg) >> 4) & 0x0Fu))
#define SVCRT_TR_SW_TO(arg)         ((uint8)((arg) & 0x0Fu))

#ifndef SVCRT_TRACE_MAX_TASKS
#define SVCRT_TRACE_MAX_TASKS   16u
#endif

/* IRQ numbers used by the board level instrumentation. They are the negative
 * exception numbers of the Cortex-M core, which is what the host displays. */
#define SVCRT_TR_IRQ_PENDSV     14u
#define SVCRT_TR_IRQ_SYSTICK    15u
#define SVCRT_TR_IRQ_SVC        11u

/* ISR enter/exit kinds, matching MDK_TRACE_KIND_* so the board does not have
 * to include the component header to instrument a handler. */
#define SVCRT_TR_ISR_ENTER      0u
#define SVCRT_TR_ISR_EXIT       1u

/* ---------------- lifecycle ---------------- */
void   svcrt_trace_init(void);                 /* enable the component        */
void   svcrt_trace_reset(void);                /* drop the switch counters    */
void   svcrt_trace_enable(uint8 on);           /* gate the instrumentation    */
uint8  svcrt_trace_enabled(void);
uint32 svcrt_trace_capacity(void);             /* ring bytes                  */
uint32 svcrt_trace_total(void);                /* events recorded since sync  */
uint32 svcrt_trace_last_cycles(void);          /* current DWT cycle count      */
uint32 svcrt_trace_switch_count(uint8 task_id);/* times this task was switched in */
uint32 svcrt_trace_overrun(void);              /* events dropped by back-pressure */

/* ---------------- recording (called from the kernel) ---------------- */
void   svcrt_trace_record(uint8 ev, uint8 arg);
void   svcrt_trace_switch(uint8 from, uint8 to);
void   svcrt_trace_wait(uint8 task_id);
/* Enter/exit of a hardware or core exception. `kind` is SVCRT_TR_ISR_*.
 * Safe from an exception handler: a few stores, no blocking. */
void   svcrt_trace_isr(uint8 irq, uint8 kind);

/* ---------------- fault snapshot ----------------
 * MUST be the first statement of a fault handler: it reads LR (EXC_RETURN),
 * MSP and PSP and walks the exception frame to recover PC / LR / xPSR. Any
 * statement placed before it can destroy the state it is here to record.
 * It is a macro, not a function: a call would push a frame and clobber LR
 * before the snapshot could read the real EXC_RETURN.
 */
#define SVCRT_TRACE_FAULT_CAPTURE()   MDK_TRACE_FAULT_CAPTURE()

/* ---------------- console command ---------------- */
int    svcrt_trace_shell_cmd(int argc, char *argv[]);

#endif /* SVCRT_TRACE_H */
