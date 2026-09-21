/**
* @brief SVCrtOS 任务与调度相关定义（内核内部头文件）
* @details 定义任务控制块、任务状态、SVC 上下文结构，以及任务管理和调度器函数。
*          这些是内核内部接口，应用/驱动一般不直接包含本文件，而是通过 SDK 或 svcrt.h 使用。
*          其中 stack_ptr 字段用于保存/恢复任务的栈指针，是上下文切换的关键。
*/

#ifndef __SVCRT_TASK_H__
#define __SVCRT_TASK_H__

#include "svcrt_def.h"
#include "svcrt_hal.h"
#include "svcrt_config.h"

typedef enum {
    SVCRT_TASK_INVALID,
    SVCRT_TASK_READY,
    SVCRT_TASK_WAIT,
    SVCRT_TASK_RUNNING
} svcrt_task_status_t;

/* ============================================================
 * 系统调用上下文（架构无关）
 * @brief SVC_Server 只接收不透明的上下文指针，参数与返回值统一通过
 *        SVCRT_SVC_ARG / SVCRT_SVC_RET 宏访问（见 svcrt_hal.h）。
 *        各架构的栈帧布局定义在 kernelsrc/port/<族>/<核心>/，
 *        内核不再包含任何寄存器排布，移植新架构无需改动本文件。
 * ============================================================ */
void SVC_Server(void *p_svc_ctx);


/**
* @brief 任务控制块（TCB）
* @details 保存一个任务的全部运行信息：内存区域、优先级、栈、状态、调度计时等。
*          所有任务的 TCB 排成 svcrt_task_table 数组。其中 stack_ptr 在任务被切走时
*          保存其 PSP，切回时据此恢复现场；mpu 是该任务的 MPU 区域快照。
*/
typedef struct {
    uint32 ram_start;
    uint32 ram_size;
    uint32 stack_size;
    uint32 rom_start;
    uint32 rom_size;
    int32  period;
    uint8  priority;                                /* 当前有效优先级（可被继承临时提升） */
    uint8  base_priority;                           /* 基准优先级：创建后不变，撤销继承用 */
    uint8  shm_attri;

    uint32 stack_top;
    uint32 *stack_bottom;
    #if (SVCRT_USE_MPU == 1)
    svcrt_arch_mpu_t mpu;           /* 架构无关的 MPU 区域上下文（见 svcrt_arch.h） */
    #endif
    svcrt_task_status_t status;
    int32  period_time;
    int32  wait_time;
    uint32 tim_tick;
    uint32 touch_tick;
    uint32 stack_ptr;
    uint8  ready_next;                              /* 就绪链表后继（任务下标；SVCRT_TASK_NIL=无） */
    uint8  ready_prev;                              /* 就绪链表前驱（任务下标；SVCRT_TASK_NIL=无） */
    uint8  delay_next;                              /* 延时链表后继（任务下标；SVCRT_TASK_NIL=无） */
    uint8  delay_prev;                              /* 延时链表前驱（任务下标；SVCRT_TASK_NIL=无） */
    uint32 delay_tick;                              /* 延时链上的绝对到期节拍 */
    int32  slice_tick;                              /* 时间片剩余节拍，<=0 时让出同优先级队首 */
    uint8  recover_pending;                         /* 故障恢复待处理标记（两阶段恢复） */
    uint8  delay_queued;                            /* 1=当前挂在延时链上（显式标记，避免单节点自环歧义） */
    void (*entry)(void);                            /* 任务入口（恢复重建用） */
    uint8  wake_reason;                             /* 0=被唤醒 1=超时唤醒 */
    #if (SVCRT_USE_STACK_USAGE == 1)
    uint32 stack_peak_low;                          /* 历史最低栈指针，用于峰值栈用量统计 */
    #endif
    uint8  is_priv;                                 /* 1 = Thread mode privileged, 0 = unprivileged.
                                                     * Honoured only when SVCRT_USE_PRIV == 1;
                                                     * the switch path writes CONTROL.nPRIV from
                                                     * it, so no task can change it for itself. */
} svcrt_task_t;

#if (SVCRT_USE_STACK_CHECK == 1)
#define SVCRT_STACK_END_FLAG_VAL  SVCRT_STACK_END_FLAG
#else
#define SVCRT_STACK_END_FLAG_VAL  (0xed01)
#endif

#if (SVCRT_USE_CPU_LOAD == 1)
extern uint16 svcrt_cpu_busy_ticks;
#endif

extern volatile uint32 svcrt_interrupt_nest;
extern uint32 svcrt_kernel_tick;
extern int32  svcrt_current_task_id;
#if (SVCRT_USE_SCHED_LOCK == 1)
/* 调度器锁：>0 表示当前禁止任务切换（用户任务经 SVC 调用 svcrt_sched_lock） */
extern volatile uint32 svcrt_sched_lock_nest;

void   svcrt_sched_lock_internal(void);
uint32 svcrt_sched_unlock_internal(void);
int32  svcrt_sched_lock_count_internal(void);
#endif

#if (SVCRT_USE_STACK_USAGE == 1)
/* 查询任务栈信息：out3[0]=总字节，out3[1]=峰值已用字节，out3[2]=剩余字节 */
int32  svcrt_task_stack_info_internal(int32 task_id, uint32 *out3);
#endif


int32  svcrt_sched_next(void);

/* ============================================================
 * 就绪队列与延时链（O(1) 调度核心）
 *
 * 就绪队列：按优先级分桶的双向循环链表 + 256 位优先级位图。
 *   选任务 = 位图取最低置位得优先级，取该桶链表头 -> O(1)；
 *   同优先级轮转 = 把队首移到队尾 -> O(1)。
 * 就绪集 = status 为 READY 或 RUNNING 的任务（RUNNING 也是可运行）。
 *
 * 延时链：按绝对到期节拍升序的双向链表，只放“有到期时间的非就绪任务”。
 *   每拍只处理链头到期的那几个 -> 均摊 O(1)，不再遍历全表。
 * ============================================================ */
#define SVCRT_TASK_NIL        (0xFFu)

/* 任务控制块指针 -> 任务下标（0 起） */
#define SVCRT_TASK_IDX(p)     ((int32)((p) - svcrt_task_table))

int32  svcrt_ready_top(void);                      /* O(1) 取最高优先级就绪任务下标，-1=无 */
void   svcrt_ready_add(int32 task_idx);            /* 加入就绪队列（幂等）并从延时链摘除 */
void   svcrt_ready_del(int32 task_idx);            /* 移出就绪队列（幂等） */
void   svcrt_ready_reprio(int32 task_idx, uint8 old_prio);  /* 优先级变了，换桶 */
void   svcrt_ready_rotate(int32 task_idx);         /* 同优先级轮转：移到该桶队尾 */
void   svcrt_delay_arm(int32 task_idx, uint32 ticks);   /* 挂入延时链（到期=当前节拍+ticks） */
void   svcrt_delay_disarm(int32 task_idx);         /* 从延时链摘除（幂等） */
void   svcrt_delay_tick(void);                     /* 每拍调用：只处理链头已到期的节点 */
void   svcrt_ready_reset(void);                    /* 清空就绪队列与延时链（装载/重置时调用） */
void   svcrt_ready_rebuild(void);                  /* 从任务表 status 重新派生就绪集（首次调度前兜底） */
int32  svcrt_sched_check(void);                    /* 一致性自检：返回不一致条目数，0=一致 */
uint32 svcrt_sched_ready_count(void);              /* 当前就绪队列上的任务数（仅自检/调试） */
uint8  svcrt_ready_contains(int32 task_idx);       /* 1=该任务当前挂在就绪链上（仅自检/调试） */

svcrt_task_t *svcrt_task_get_current(void);
uint32 svcrt_kernel_get_time(void);
uint32 svcrt_kernel_get_tick(void);
uint16 svcrt_kernel_get_cpu_busy_ticks(void);

void svcrt_task_wait_internal(uint32 ms);
void svcrt_task_wait_period_internal(void);
void svcrt_task_block_internal(void);

/**
* @brief 在调用方已持有的临界区内阻塞当前任务（供信号量/互斥锁/消息队列使用）
* @param timeout_ms >0 定时等待（ms）；<=0 无限等待（只能被显式唤醒）
* @note 这是内核内部原语，不直接对外。调用方（sem/mtx/mq/event）必须先把
*       对外 API 的“0=不等待、负值=永久等待”约定归一化后再调用，
*       不能把对外的 0 直接传进来（那会被本原语解释成无限等待）。
* @return 0=被显式唤醒（已获得资源），1=等待超时，-1=未能进入阻塞（调用方需自行摘除队列）
* @details 调用约定：进入时中断已关，返回时中断仍关（调用方在同一临界区内继续处理等待队列）。
*          与 svcrt_task_wait_internal 的区别：后者会自行开关中断，
*          对“先把自己挂进等待队列、再睡下”的原语来说，那中间存在唤醒丢失窗口
*          （ISR 的唤醒会投给一个还没睡下的任务）。本函数把置状态与切走放在同一临界区内。
*/
int32 svcrt_task_block_in_critical(uint32 timeout_ms);

/**
* @brief 任务下线收尸：清理该任务在同步对象/消息队列中的等待登记与锁持有关系
* @param task_id 目标任务号（从 1 开始）
* @details 用于任务自杀（kill）、故障恢复、覆盖安装前硬停任务等场景。
*          不做收尸的后果：post/unlock 会把一个已经不在等待的任务置为 READY
*          （从旧栈“复活”）；它持有的互斥锁永久锁死，等待者全部饿死。
*          本函数自带保存-恢复语义的临界区，可在已有临界区内安全调用。
*/
void svcrt_task_release_resources(int32 task_id);
void svcrt_task_delay_internal(uint32 us);
void svcrt_task_kill_internal(void);
int32  svcrt_task_status_get_internal(int32 task_id);

/* ============================================================
 * User thread service (SVC 0x1B) - kernel side
 *
 * Every entry point below is reachable from user mode through the SVC
 * boundary. The create wrapper validates the entry point and the stack
 * window against the caller's own firmware / RAM regions before a TCB is
 * allocated, so a thread can never be handed a stack that overlaps the
 * kernel or a neighbour App.
 * ============================================================ */
int32  svcrt_thread_create_internal(void (*entry)(void), uint32 *stack_bottom,
                                    uint32 stack_size, uint8 priority, uint32 period_ms);
int32  svcrt_thread_self_internal(void);
void   svcrt_thread_exit_internal(void);
int32  svcrt_task_recover(int32 task_id);

void svcrt_sched_activate_higher(uint8 ck_pri);

int32 svcrt_sched_is_switching(void);
int32 svcrt_sched_activate(int32 new_task, uint32 old_psp);

void svcrt_kernel_tick_handler(void);
/* 各 CPU 异常共用入口：fault_type 取 svcrt_fault_type_t 中的 HARDFAULT/MEMFAULT/BUSFAULT/USGFAULT
 * 返回值：可用于恢复的任务栈指针（非 0 时由板级层调用 svcrt_port_resume_task 完成恢复）；
 *         0 表示不可恢复（内核/中断上下文故障），板级层应停机等待调试器。 */
uint32 svcrt_cpu_fault_handler(uint32 fault_type);
uint32 svcrt_hardfault_handler(void);

#endif
