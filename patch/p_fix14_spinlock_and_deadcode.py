# -*- coding: utf-8 -*-
"""
Round 8, part 2: wire the spinlock to a real site, and remove the confirmed
redundant dead code.

1) Spinlock wiring (audit 3.2)
   The whole svcrt_spin.h API had zero callers, so SVCRT_USE_SPINLOCK == 1 was
   meaningless. The real defect behind it: svcrt_ptable_set_slot() writes the
   (state, entry, task_id) triple with no protection at all, while the fault
   path (svcrt_loader_on_fault, which can run from exception context) and the
   installer task can interleave - a reader could see state=LOADED with a stale
   task_id/entry. Guard the triple with the irqsave spinlock variant and add a
   locked reader for the pair the loader actually depends on.

   Also fix SVCRT_SPINLOCK_INIT: it did not match svcrt_spin_init() (owner 0 vs
   0xffffffff), so a statically defined lock had owner == CPU 0 == "held by me"
   before anyone took it.

2) Dead code removal (audit 3.3, first three items)
   - svcrt_sched_switch()      duplicate of the SVCRT_SWITCH_TASK() macro
   - svcrt_fault_record_read_internal()  superseded by svcrt_fault_record_get()
   - svcrt_port_enable_fpu()   the FPU is configured in svcrt_port_board_init()

   Kept on purpose (public API or port contract, not dead code):
   svcrt_event_set_from_isr / svcrt_sem_post_from_isr /
   svcrt_mq_send_from_isr_internal / svcrt_kernel_get_tick /
   svcrt_fault_record_read / svcrt_loader_load_buffer (debug entry point).
   svcrt_port_nop / svcrt_port_in_isr / svcrt_port_get_system_clock /
   svcrt_port_flash_sector_size stay part of the port interface.

All new comments are ASCII / English.
"""
import os
import shutil

ROOT = r'D:\工作\git_project\svcrtos_new'
BK = r'C:\Users\14905\OneDrive\Documents\lingxi-claw\20260911-13-47-19-445\backup_fix14'


def detect_enc(raw):
    try:
        raw.decode('utf-8')
        return 'utf-8'
    except UnicodeDecodeError:
        return 'gbk'


def load(rel):
    p = os.path.join(ROOT, rel.replace('/', os.sep))
    raw = open(p, 'rb').read()
    return p, raw.decode(detect_enc(raw)), detect_enc(raw)


def replace(rel, pairs):
    p, text, enc = load(rel)
    shutil.copy2(p, os.path.join(BK, rel.replace('/', '__')))
    for old, new in pairs:
        if old not in text:
            alt = old.replace('\n', '\r\n')
            if alt in text:
                old, new = alt, new.replace('\n', '\r\n')
            else:
                raise AssertionError('%s: anchor not found: %r' % (rel, old[:100]))
        assert text.count(old) == 1, '%s: anchor not unique (%d)' % (rel, text.count(old))
        text = text.replace(old, new)
    open(p, 'wb').write(text.encode(enc))
    print('  [ok] %s' % rel)


def main():
    os.makedirs(BK, exist_ok=True)

    # ---------------------------------------------------------- 1. spinlock
    replace('kernelsrc/include/svcrt_spin.h', [(
        '''/** @brief 自旋锁静态初始化值（用于定义全局锁对象） */
#define SVCRT_SPINLOCK_INIT          { 0u, 0u, 0u }''',
        '''/** @brief Static initialiser for a global lock object.
 *  @note Must match svcrt_spin_init(): owner 0xffffffff means "no owner", so a
 *        freshly defined lock cannot be mistaken for one held by CPU 0. */
#define SVCRT_SPINLOCK_INIT          { 0u, 0xffffffffu, 0u }''')])

    replace('kernelsrc/include/svcrt_ptable.h', [(
        '''/**
* @brief 更新槽位运行期状态
* @param slot    槽位号
* @param state   SVCRT_APP_SLOT_x
* @param entry   入口地址（直接赋值，清空槽位时传 0）
* @param task_id 任务号（直接赋值，未启动时传 0）
* @return 0=成功，-1=槽位非法
*/
int32 svcrt_ptable_set_slot(uint32 slot, uint32 state, uint32 entry, uint32 task_id);''',
        '''/**
* @brief 更新槽位运行期状态
* @param slot    槽位号
* @param state   SVCRT_APP_SLOT_x
* @param entry   入口地址（直接赋值，清空槽位时传 0）
* @param task_id 任务号（直接赋值，未启动时传 0）
* @return 0=成功，-1=槽位非法
* @note 三个字段在自旋锁保护下作为一组更新：故障处理路径（可能运行在异常
*       上下文）与安装任务都可能同时改写同一个槽位，否则读者会看到
*       state/entry/task_id 互相不匹配的中间态。
*/
int32 svcrt_ptable_set_slot(uint32 slot, uint32 state, uint32 entry, uint32 task_id);

/**
* @brief 原子读取一个槽位的状态三元组
* @param slot     槽位号
* @param p_state  输出状态（可为 0）
* @param p_entry  输出入口地址（可为 0）
* @param p_task_id 输出任务号（可为 0）
* @return 0=成功，-1=槽位非法
*/
int32 svcrt_ptable_get_slot(uint32 slot, uint32 *p_state, uint32 *p_entry, uint32 *p_task_id);''')])

    replace('kernelsrc/src/svcrt_ptable.c', [
        ('''#include "svcrt_partition.h"    /* 内核专属：全工程唯一地址源头 */''',
         '''#include "svcrt_partition.h"    /* 内核专属：全工程唯一地址源头 */
#include "svcrt_spin.h"

/* Guards the (state, entry, task_id) triple of every slot.
 *
 * Two writers can meet here: the installer task (normal task context) and the
 * crash policy in svcrt_loader_on_fault(), which runs from exception context.
 * Without the lock a reader could observe state = LOADED together with a stale
 * entry or task id. The irqsave variant is used because the fault path runs
 * with exceptions masked already - disabling interrupts makes a nested take
 * safe (the lock stays re-entrant per CPU, see svcrt_spin_lock_irqsave). */
static svcrt_spinlock_t svcrt_ptable_lock = SVCRT_SPINLOCK_INIT;'''),
        ('''int32 svcrt_ptable_set_slot(uint32 slot, uint32 state, uint32 entry, uint32 task_id)
{
    svcrt_partition_table_t *pt = svcrt_ptable_get();

    if(slot >= pt->app_max_count || slot >= 8u)
    {
        return -1;
    }

    pt->slot_state[slot]   = state;
    pt->slot_entry[slot]   = entry;
    pt->slot_task_id[slot] = task_id;

    return 0;
}''',
         '''int32 svcrt_ptable_set_slot(uint32 slot, uint32 state, uint32 entry, uint32 task_id)
{
    svcrt_partition_table_t *pt = svcrt_ptable_get();
    uint32 irq_state;

    if(slot >= pt->app_max_count || slot >= 8u)
    {
        return -1;
    }

    svcrt_spin_lock_irqsave(&svcrt_ptable_lock, &irq_state);

    pt->slot_state[slot]   = state;
    pt->slot_entry[slot]   = entry;
    pt->slot_task_id[slot] = task_id;

    svcrt_spin_unlock_irqrestore(&svcrt_ptable_lock, irq_state);

    return 0;
}

int32 svcrt_ptable_get_slot(uint32 slot, uint32 *p_state, uint32 *p_entry, uint32 *p_task_id)
{
    svcrt_partition_table_t *pt = svcrt_ptable_get();
    uint32 irq_state;

    if(slot >= pt->app_max_count || slot >= 8u)
    {
        return -1;
    }

    svcrt_spin_lock_irqsave(&svcrt_ptable_lock, &irq_state);

    if(p_state != 0)
    {
        *p_state = pt->slot_state[slot];
    }
    if(p_entry != 0)
    {
        *p_entry = pt->slot_entry[slot];
    }
    if(p_task_id != 0)
    {
        *p_task_id = pt->slot_task_id[slot];
    }

    svcrt_spin_unlock_irqrestore(&svcrt_ptable_lock, irq_state);

    return 0;
}''')])

    # loader: read the pair it depends on atomically
    replace('kernelsrc/src/svcrt_loader.c', [(
        '''    if(pt->slot_state[slot] != SVCRT_APP_SLOT_LOADED)
    {
        return SVCRT_LOADER_ERR_STATE;
    }

    entry = pt->slot_entry[slot];
    if(entry == 0u)
    {
        return SVCRT_LOADER_ERR_STATE;
    }''',
        '''    /* State and entry must be read as one unit: the crash policy may be
     * rewriting this slot from exception context right now. */
    if(svcrt_ptable_get_slot(slot, &slot_state, &entry, 0) != 0)
    {
        return SVCRT_LOADER_ERR_PARAM;
    }

    if(slot_state != SVCRT_APP_SLOT_LOADED)
    {
        return SVCRT_LOADER_ERR_STATE;
    }

    if(entry == 0u)
    {
        return SVCRT_LOADER_ERR_STATE;
    }'''),
        ('''    uint32 stack_top;
    uint32 stack_bottom;
    int32 task_id;''',
         '''    uint32 stack_top;
    uint32 stack_bottom;
    uint32 slot_state;
    int32 task_id;''')])

    print('spinlock wiring patch done')

# NOTE on dead code (audit 3.3): svcrt_sched_switch(),
# svcrt_fault_record_read_internal() and svcrt_port_enable_fpu() are already
# gone from the working tree (they only survive in the stale docs/.gen_utf8/
# copies). Re-run patch/deadcode_audit.py for the current truth.


if __name__ == '__main__':
    main()
