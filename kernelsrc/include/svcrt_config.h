/**
* @brief SVCrtOS 内核集中配置文件
* @details 借鉴 RT-Thread 的 rtconfig.h 思路，把内核所有可裁剪、可调参数集中在此管理。
*          板级配置可通过 SVCRT_BOARD_CONFIG 宏指向的头文件覆盖这里的默认值。
*          覆盖规则：本文件里**所有可裁剪开关与可调参数**都必须用 #ifndef 包住，
*          否则板级配置重定义时会报宏重定义，等于覆盖不了；
*          由这些参数派生出来的宏（如 SVCRT_MS_TO_TICK）不包，它不是配置项。
* @note 本文件不包含任何芯片相关头文件，也不直接操作任何硬件寄存器。
*/

#ifndef __SVCRT_CONFIG_H__
#define __SVCRT_CONFIG_H__

/* ============================================================
 * 板级配置优先加载
 * @brief 板级配置可覆盖 SVCRT_CPU_ARCH / SVCRT_ARCH_CORE、主频、
 *        内存地址等参数，因此必须在架构派生与默认值之前加载。
 *        通过编译选项 -DSVCRT_BOARD_CONFIG=<chip_header.h> 指定。
 * ============================================================ */
#ifdef SVCRT_BOARD_CONFIG
#include SVCRT_BOARD_CONFIG
#endif

/* ============================================================
 * Partition / capacity policy: single source of truth
 * @brief Task table size and slot counts live in
 *        config/svcrt_partition.h together with the Flash/RAM layout,
 *        so a capacity change can never disagree with the layout.
 *        Do not define SVCRT_TASK_MAX_NUM here anymore.
 * ============================================================ */
#include "svcrt_partition.h"

/* ============================================================
 * 自旋锁 / 临界区
 * @brief 内核与驱动侧自旋锁（svcrt_spin.h）
 * ============================================================ */
#ifndef SVCRT_USE_SPINLOCK
#define SVCRT_USE_SPINLOCK        1
#endif

/* ============================================================
 * 调度器锁（用户态临界区）
 * @brief 用户任务通过 SVC 调用 svcrt_sched_lock/unlock，
 *        禁止任务切换（不关中断），相当于 RT-Thread 的 rt_enter_critical
 * ============================================================ */
#ifndef SVCRT_USE_SCHED_LOCK
#define SVCRT_USE_SCHED_LOCK      1
#endif

/* ============================================================
 * 任务栈使用量分析
 * @brief 运行时统计每个任务的峰值栈用量（高水位法）：
 *        1) 创建时用 SVCRT_STACK_FILL_PATTERN 填充整段栈；
 *        2) 查询时扫描未被覆盖的图案区得到已用峰值；
 *        3) 上下文切换时记录最低栈指针，二者取大值。
 * ============================================================ */
#ifndef SVCRT_USE_STACK_USAGE
#define SVCRT_USE_STACK_USAGE     1
#endif

#ifndef SVCRT_STACK_FILL_PATTERN
#define SVCRT_STACK_FILL_PATTERN  (0xcdcdcdcd)
#endif

#include "svcrt_arch.h"

/* ============================================================
 * CPU 架构选择（架构抽象层）
 * @brief 架构族 / 具体核心 / 能力宏统一由 svcrt_arch.h 提供，
 *        本文件只负责据此派生内核功能开关的默认值。
 *        选择方式（二选一，板级配置或编译选项均可）：
 *          1) SVCRT_ARCH_CORE = SVCRT_CPU_CORE_xxx（推荐，支持 ARM/RISC-V/LoongArch）
 *          2) SVCRT_CPU_ARCH  = 0/1/2（兼容旧工程，仅 Cortex-M3/M4/M7）
 *        新架构实现放在 kernelsrc/port/<族>/<核心>/，详见 port/README.md
 * ============================================================ */

/* ============================================================
 * FPU 浮点单元使能
 * @brief 是否启用浮点单元，影响上下文切换时是否保存浮点寄存器。
 *        需与目标 CPU 实际硬件一致；M4F/M7 有 FPU，M3 无 FPU。
 * ============================================================ */
#ifndef SVCRT_USE_FPU
#define SVCRT_USE_FPU             SVCRT_ARCH_HAS_FPU
#endif

/* ============================================================
 * MPU 内存保护单元使能
 * @brief 是否启用 MPU，实现任务间 ROM/RAM 空间隔离。
 *        需与目标 CPU 实际硬件一致；启用后每次切换会重配 MPU 区域。
 * ============================================================ */
#ifndef SVCRT_USE_MPU
#define SVCRT_USE_MPU             SVCRT_ARCH_HAS_MPU
#endif

/* ============================================================
 * 特权级分离使能
 * @note 依赖 MPU；未启用 MPU 时特权分离无意义，自动关闭。
 * ============================================================ */
#ifndef SVCRT_USE_PRIV
#if (SVCRT_USE_MPU == 1)
#define SVCRT_USE_PRIV            1
#else
#define SVCRT_USE_PRIV            0
#endif
#endif

/* ============================================================
 * MPU peripheral windows (only used when SVCRT_USE_MPU == 1)
 * @details Unprivileged tasks may reach peripheral registers only through
 *          these two windows. 0 = window disabled by default; the real
 *          ranges are chip characteristics and come from the board config,
 *          the kernel does not know any peripheral address.
 *          SVCRT_MPU_PERIPH_RW: 1 = unprivileged read/write, 0 = readonly.
 * ============================================================ */
#ifndef SVCRT_MPU_PERIPH_BASE
#define SVCRT_MPU_PERIPH_BASE     (0u)
#endif
#ifndef SVCRT_MPU_PERIPH_SIZE
#define SVCRT_MPU_PERIPH_SIZE     (0u)
#endif
#ifndef SVCRT_MPU_PERIPH2_BASE
#define SVCRT_MPU_PERIPH2_BASE    (0u)
#endif
#ifndef SVCRT_MPU_PERIPH2_SIZE
#define SVCRT_MPU_PERIPH2_SIZE    (0u)
#endif
#ifndef SVCRT_MPU_PERIPH_RW
#define SVCRT_MPU_PERIPH_RW       (0)
#endif

/* ============================================================
 * 任务与调度相关参数
 * ============================================================ */
/* Task capacity moved to config/svcrt_partition.h:
 *   SVCRT_TASK_MAX_NUM        total task-table slots (static TCB array)
 *   SVCRT_TASK_TABLE_RAM_MAX  static TCB table RAM budget, fixed in bytes
 * Edit them there only; the partition header is included above. */
#ifndef SVCRT_TICK_PERIOD_US
#define SVCRT_TICK_PERIOD_US      (500)
#endif
#ifndef SVCRT_EVENT_NUM
#define SVCRT_EVENT_NUM           (10)
#endif
#ifndef SVCRT_MAX_EVENT_WAITERS
#define SVCRT_MAX_EVENT_WAITERS   (4)
#endif

/* 信号量、互斥锁及其等待者数量 */
#ifndef SVCRT_SEM_NUM
#define SVCRT_SEM_NUM             (8)
#endif
#ifndef SVCRT_MTX_NUM
#define SVCRT_MTX_NUM             (8)
#endif
#ifndef SVCRT_MAX_SYNC_WAITERS
#define SVCRT_MAX_SYNC_WAITERS    (4)
#endif

#define SVCRT_MS_TO_TICK(ms)      ((ms) * 1000 / SVCRT_TICK_PERIOD_US)

/* ============================================================
 * 设备框架最大设备数
 * ============================================================ */
#ifndef SVCRT_DEV_MAX_NUM
#define SVCRT_DEV_MAX_NUM         (8)
#endif

/* ============================================================
 * 消息队列配置
 * ============================================================ */
#ifndef SVCRT_USE_MQ
#define SVCRT_USE_MQ              1
#endif
#ifndef SVCRT_MQ_NUM
#define SVCRT_MQ_NUM              (8)
#endif

/* 条件变量：与互斥量配对使用（等待时原子释放锁，醒来重新获取）。数量与
 * 其它同步对象同级，够一组典型的“生产者 / 消费者”任务用。 */
#ifndef SVCRT_COND_NUM
#define SVCRT_COND_NUM            (8)
#endif
#ifndef SVCRT_MQ_DEPTH
#define SVCRT_MQ_DEPTH            (8)
#endif
#ifndef SVCRT_MQ_MSG_WORDS
#define SVCRT_MQ_MSG_WORDS        (4)
#endif

/* ============================================================
 * 软定时器配置
 * ============================================================ */
#ifndef SVCRT_USE_TIMER
#define SVCRT_USE_TIMER           1
#endif
#ifndef SVCRT_TIMER_NUM
#define SVCRT_TIMER_NUM           (8)
#endif
#ifndef SVCRT_TIMER_TASK_PRI
#define SVCRT_TIMER_TASK_PRI      (200)
#endif
#ifndef SVCRT_TIMER_TASK_STACK_WORDS
#define SVCRT_TIMER_TASK_STACK_WORDS  (96)
#endif

/* ============================================================
 * 故障记录与任务恢复配置
 * ============================================================ */
#ifndef SVCRT_USE_FAULT_RECOVER
#define SVCRT_USE_FAULT_RECOVER   1
#endif
#ifndef SVCRT_FAULT_RECORD_NUM
#define SVCRT_FAULT_RECORD_NUM    (8)
#endif

/* ============================================================
 * CPU 负载统计开关
 * ============================================================ */
/* 调度快速路径：节拍里只有"确实要换任务"时才拉 PendSV。
 * 0 = 旧行为：每拍无条件拉一次，即使没人要切换也要走一整趟异常往返
 *     （F427@96MHz 实测 841 cycles/拍，2kHz 节拍下约 1.7% CPU）。 */
#ifndef SVCRT_USE_FAST_TICK_SWITCH
#define SVCRT_USE_FAST_TICK_SWITCH   1
#endif

/* 同优先级时间片长度（节拍数）。
 * 同优先级任务连续运行满该节拍数后让出队首，由同优先级的下一个任务接手；
 * 0 = 不轮转（同优先级内始终由最早就绪的那个运行）。
 * 500us 节拍下 10 拍 = 5ms。抢占与片长无关：更高优先级任务就绪立即抢占。 */
#ifndef SVCRT_TIME_SLICE_TICKS
#define SVCRT_TIME_SLICE_TICKS       (10)
#endif

/* MPU 差异应用：切换时上下文没变就整个跳过，变了的区域才写寄存器。
 * 0 = 旧行为：每次都关 MPU、重写全部 8 个区域、再开 MPU + 两个屏障。 */
#ifndef SVCRT_USE_MPU_APPLY_CACHE
#define SVCRT_USE_MPU_APPLY_CACHE    1
#endif

/* 调度开销统计：用 DWT 周期计数记 PendSV 往返与 sched_activate 的
 * min/sum/max/次数。符号可直接被主机侧 read_variable 读到，不需要 shell 命令。
 * 依赖 mdk_trace_init() 打开 CYCCNT（板级 svcrt_trace_init() 会调用）。 */
#ifndef SVCRT_USE_SCHED_STAT
#define SVCRT_USE_SCHED_STAT         1
#endif

#ifndef SVCRT_USE_CPU_LOAD
#define SVCRT_USE_CPU_LOAD        1
#endif

/* ============================================================
 * 栈溢出检测开关与栈底标志值
 * ============================================================ */
#ifndef SVCRT_USE_STACK_CHECK
#define SVCRT_USE_STACK_CHECK     1
#endif
#ifndef SVCRT_STACK_END_FLAG
#define SVCRT_STACK_END_FLAG      (0xed01)
#endif

/* ============================================================
 * 共享内存配置
 * @brief 默认值，通常由板级配置文件 board/svcrt_board_config.h
 *        根据具体芯片的内存布局覆盖。
 * ============================================================ */
/* 注：SVCRT_SHARE_MEM_ADDR / SVCRT_SHARE_MEM_SIZE 已删除。
 * 共享内存位置由 config/svcrt_partition.h 的 SHARE_RAM_BASE / SHARE_RAM_SIZE 决定，
 * 内核与用户态都只从运行期分区表读取，不再有编译期地址宏。 */

/* ============================================================
 * 系统主频配置
 * @brief 默认值，通常由板级配置文件覆盖。
 *        svcrt_port_get_system_clock() 返回该频率，
 *        移植时一般与 CMSIS 的 SystemCoreClock 一致。
 * ============================================================ */
#ifndef SVCRT_SYSTEM_CLOCK_HZ
#define SVCRT_SYSTEM_CLOCK_HZ     (168000000)
#endif

/* ============================================================
 * 板级配置文件挂载点
 * @brief 已移至本文件开头（架构选择之前）加载，
 *        以便板级配置能覆盖 SVCRT_ARCH_CORE 并参与能力派生。
 * ============================================================ */

#endif
