/**
* @file svcrt_spin.h
* @brief SVCrtOS 自旋锁与临界区（内核/驱动侧）
* @details 提供架构无关的自旋锁实现，用于：
*          1) 内核内部短小临界区保护（对象表、设备表等）；
*          2) 中断服务程序与任务共享数据的保护；
*          3) 为将来多核（SMP）扩展预留：锁字采用原子 CAS 操作，
*             并记录持有者 CPU ID，多核移植时只需实现 port 层原子接口。
*
*          【与调度器锁的区别】
*          - 自旋锁 svcrt_spin_*：内核态/驱动态使用，可选择是否关中断，
*            不引起任务切换，临界区必须极短（微秒级）。
*          - 调度器锁 svcrt_sched_lock()：用户任务通过 SVC 调用，
*            禁止任务切换（相当于 RT-Thread 的 rt_enter_critical），
*            用于保护较长的共享资源访问，不关闭中断。
*
*          【架构无关性】
*          本文件不包含任何架构指令，原子操作与 CPU ID 由 port 层提供：
*          - svcrt_port_atomic_cas() 比较交换（ARM: LDREX/STREX，
*            RISC-V: AMO/LR-SC，LoongArch: LL/SC）
*          - svcrt_port_cpu_id()     当前 CPU 编号（单核恒为 0）
*          - svcrt_port_spin_hint()  自旋等待提示（ARM: NOP/WFE）
*
* @note 同一 CPU 上重复获取同一把锁会累加嵌套计数（可重入），
*       但 ISR 与任务共用同一把锁时必须使用 irqsave 变体，
*       否则在单核上中断打断持锁任务会造成自死锁。
*/

#ifndef __SVCRT_SPIN_H__
#define __SVCRT_SPIN_H__

#include "svcrt_types.h"
#include "svcrt_config.h"
#include "svcrt_hal.h"

#if (SVCRT_USE_SPINLOCK == 1)

/**
* @brief 自旋锁对象
* @details lock 为原子访问的锁字（0 空闲 / 1 占用），owner 记录持有者 CPU，
*          nest 记录同一 CPU 的嵌套层数。
*/
typedef struct {
    volatile uint32 lock;        /* 锁字：0=空闲，非 0=已占用（CAS 目标） */
    volatile uint32 owner;       /* 持有者 CPU ID（SMP 用，单核恒为 0）  */
    volatile uint32 nest;        /* 同 CPU 重入层数，0 表示未持有         */
} svcrt_spinlock_t;

/** @brief 自旋锁静态初始化值（用于定义全局锁对象） */
#define SVCRT_SPINLOCK_INIT          { 0u, 0u, 0u }

/**
* @brief 定义并初始化一把全局自旋锁
* @param name 锁变量名
* @code
* SVCRT_SPINLOCK_DEFINE(g_dev_lock);
* @endcode
*/
#define SVCRT_SPINLOCK_DEFINE(name)  svcrt_spinlock_t name = SVCRT_SPINLOCK_INIT

/**
* @brief 初始化（或重新初始化）自旋锁
* @param p_lock 锁对象指针
* @note 仅可在确认无人持有该锁时调用。
*/
static inline void svcrt_spin_init(svcrt_spinlock_t *p_lock)
{
    p_lock->lock  = 0u;
    p_lock->owner = 0xffffffffu;
    p_lock->nest  = 0u;
    SVCRT_DMB();
}

/**
* @brief 尝试获取自旋锁（非阻塞）
* @param p_lock 锁对象指针
* @return 1=获取成功，0=锁已被其他 CPU 持有
* @note 同一 CPU 重入时直接累加嵌套计数并返回成功，避免自死锁。
*/
static inline int32 svcrt_spin_trylock(svcrt_spinlock_t *p_lock)
{
    uint32 cpu = svcrt_port_cpu_id();

    /* 同一 CPU 重入：仅累加嵌套计数 */
    if((p_lock->nest != 0u) && (p_lock->owner == cpu))
    {
        p_lock->nest++;
        return 1;
    }

    if(svcrt_port_atomic_cas(&p_lock->lock, 0u, 1u) != 0u)
    {
        SVCRT_DMB();
        p_lock->owner = cpu;
        p_lock->nest  = 1u;
        return 1;
    }

    return 0;
}

/**
* @brief 获取自旋锁（阻塞自旋直到成功）
* @param p_lock 锁对象指针
* @note 只能在任务或中断上下文短暂自旋，禁止在持锁期间调用任何
*       可能引起任务切换或长时间阻塞的接口。
*/
static inline void svcrt_spin_lock(svcrt_spinlock_t *p_lock)
{
    while(svcrt_spin_trylock(p_lock) == 0)
    {
        /* test-and-test-and-set：减少总线/互斥访问压力 */
        while(p_lock->lock != 0u)
        {
            svcrt_port_spin_hint();
        }
    }
}

/**
* @brief 释放自旋锁
* @param p_lock 锁对象指针
* @note 非持有者调用时直接返回（防御性处理，不破坏锁状态）。
*/
static inline void svcrt_spin_unlock(svcrt_spinlock_t *p_lock)
{
    if(p_lock->owner != svcrt_port_cpu_id())
    {
        return;
    }

    if(p_lock->nest > 1u)
    {
        p_lock->nest--;
        return;
    }

    p_lock->nest  = 0u;
    p_lock->owner = 0xffffffffu;
    SVCRT_DMB();
    p_lock->lock  = 0u;
}

/**
* @brief 关中断并获取自旋锁（中断与任务共用数据的标准做法）
* @param p_lock  锁对象指针
* @param p_state 输出参数，保存进入前的中断状态，供解锁时恢复
* @note 顺序为「先关中断，再抢锁」，与 unlock_irqrestore 严格对称。
*/
static inline void svcrt_spin_lock_irqsave(svcrt_spinlock_t *p_lock, uint32 *p_state)
{
    *p_state = SVCRT_ENTER_CRITICAL();
    svcrt_spin_lock(p_lock);
}

/**
* @brief 释放自旋锁并恢复中断状态
* @param p_lock 锁对象指针
* @param state  svcrt_spin_lock_irqsave 保存的中断状态
*/
static inline void svcrt_spin_unlock_irqrestore(svcrt_spinlock_t *p_lock, uint32 state)
{
    svcrt_spin_unlock(p_lock);
    SVCRT_EXIT_CRITICAL(state);
}

/**
* @brief 查询自旋锁当前是否被持有
* @param p_lock 锁对象指针
* @return 1=已持有，0=空闲
*/
static inline int32 svcrt_spin_is_locked(svcrt_spinlock_t *p_lock)
{
    return (p_lock->lock != 0u) ? 1 : 0;
}

#endif /* SVCRT_USE_SPINLOCK */

#endif /* __SVCRT_SPIN_H__ */
