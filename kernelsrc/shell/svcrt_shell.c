/**
* @file svcrt_shell.c
* @brief SVCrtOS 内核 Shell 控制台任务与内核命令集
* @details 把 ark-shell（kernelsrc/shell/ark_shell/，上游原样引入）挂成一个
*          内核任务：
*          - 任务栈取自内核 RAM（大小见 config/svcrt_partition.h 的 SHELL_TASK_*）；
*          - 每轮非阻塞跑一次 ark_shell_run()，然后按 SHELL_TASK_PERIOD_MS 让出 CPU；
*          - 命令直接调用内核内部接口（loader / ptable / task / fault），
*            所以 shell 是特权态任务，不做 SVC 切换，也不经过 App SDK。
*
*          命令集：
*            help                  内置：列出所有命令
*            info / app / drv / task / sched / fault / install [slot] / cfg / blk / fs
*            app / drv 支持 uninstall <slot>：卸载镜像并回收池内空间
*            version / clear / echo / reboot  为 ark-shell 内置命令
*
*          「开机是否自启」在打包时写进镜像头（tools/pack_app.py --autostart），
*          运行期由 `app list` / `drv list` 的 auto 列展示；本 shell 只负责
*          运行期启停（start / stop），不改写自启标志。
*
* @note 本模块属于内核特权态代码。
*/

#include "svcrt_shell.h"

#include "ark_shell.h"
#include "ark_shell_commands.h"
#include "platform.h"

#include "svcrt_config.h"
#include "svcrt_cfg.h"
#include "svcrt_fault.h"
#include "svcrt_installer.h"
#include "svcrt_loader.h"
#include "svcrt_dev.h"
#include "svcrt_layout.h"
#include "svcrt_ptable.h"
#include "svcrt_log.h"
#include "svcrt_fifo.h"
#include "svcrt_sync.h"
#include "svcrt_share.h"
#include "svcrt_task.h"
#include "svcrt_trace.h"
#include "svcrt_blk.h"
#include "svcrt_fs.h"
#include "svcrt_audit.h"
#include "svcrt_guard.h"
#include "svcrt_crash.h"
#include "svcrt_partition.h"

#include <string.h>

#if (SHELL_ENABLE == 1)

/* 控制台实例（含行编辑缓冲与命令历史，放 .bss 而不是任务栈） */
static ark_shell_t g_shell;

/* 控制台任务栈（内核任务，取自内核 RAM） */
static uint32 g_shell_stack[SHELL_TASK_STACK_SIZE / 4u];

/* ============================================================
 * 小工具
 * ============================================================ */

static void sh_out(const char *s)
{
    platform_uart_send_string(s);
}

/* 解析十进制非负整数；非法返回 -1 */
static int parse_u32(const char *s, uint32 *out)
{
    uint32 v = 0u;

    if((s == 0) || (*s == '\0'))
    {
        return -1;
    }

    while(*s != '\0')
    {
        if((*s < '0') || (*s > '9'))
        {
            return -1;
        }
        v = v * 10u + (uint32)(*s - '0');
        s++;
    }

    *out = v;
    return 0;
}

static const char *slot_state_name(uint32 st)
{
    switch(st)
    {
        case SVCRT_APP_SLOT_EMPTY:      return "EMPTY";
        case SVCRT_APP_SLOT_LOADED:     return "LOADED";
        case SVCRT_APP_SLOT_RUNNING:    return "RUNNING";
        case SVCRT_APP_SLOT_INVALID:    return "INVALID";
        case SVCRT_APP_SLOT_INSTALLING: return "INSTALL";
        case SVCRT_APP_SLOT_RAW:        return "RAW";
        default:                        return "?";
    }
}

static const char *slot_type_name(uint32 t)
{
    switch(t)
    {
        case SVCRT_SLOT_APP:    return "app";
        case SVCRT_SLOT_DRIVER: return "drv";
        case SVCRT_SLOT_FREE:   return "free";
        default:                return "?";
    }
}

static const char *cfg_mode_text(uint32 mode)
{
    if(mode == SVCRT_LAYOUT_MODE_FIXED)
    {
        return "fixed";
    }
    if(mode == SVCRT_LAYOUT_MODE_AUTO)
    {
        return "auto";
    }
    return "unknown";
}

static const char *cfg_source_text(uint32 src)
{
    if(src == SVCRT_LAYOUT_SOURCE_CONFIG)
    {
        return "device config region";
    }
    if(src == SVCRT_LAYOUT_SOURCE_DEFAULT)
    {
        return "compile-time default";
    }
    return "unknown";
}

static const char *cfg_reclaim_text(uint32 rm)
{
    if(rm == SVCRT_CFG_RECLAIM_GLOBAL)
    {
        return "global";
    }
    if(rm == SVCRT_CFG_RECLAIM_MINIMAL)
    {
        return "minimal";
    }
    if(rm == SVCRT_CFG_RECLAIM_NONE)
    {
        return "none";
    }
    return "unknown";
}

static const char *cfg_type_text(uint32 t)
{
    if(t == SVCRT_CFG_TYPE_APP)
    {
        return "app";
    }
    if(t == SVCRT_CFG_TYPE_DRIVER)
    {
        return "drv";
    }
    return "-";
}

// moved up: cmd_pool (defined below) prints the fixed slot types too
static const char *task_state_name(uint32 st)
{
    switch(st)
    {
        case SVCRT_TASK_INVALID: return "INVALID";
        case SVCRT_TASK_READY:   return "READY";
        case SVCRT_TASK_WAIT:    return "WAIT";
        case SVCRT_TASK_RUNNING: return "RUNNING";
        default:                 return "?";
    }
}

static const char *fault_type_name(uint32 t)
{
    switch(t)
    {
        case SVCRT_FAULT_HARDFAULT:    return "HARDFAULT";
        case SVCRT_FAULT_STACKOVF:     return "STACKOVF";
        case SVCRT_FAULT_TASKKILL:     return "TASKKILL";
        case SVCRT_FAULT_RECOVER:      return "RECOVER";
        case SVCRT_FAULT_SCHEDLOCK:    return "SCHEDLOCK";
        case SVCRT_FAULT_APPDISABLED:  return "APPDISABLED";
        case SVCRT_FAULT_NOSLOT:       return "NOSLOT";
        case SVCRT_FAULT_MEMFAULT:     return "MEMFAULT";
        case SVCRT_FAULT_BUSFAULT:     return "BUSFAULT";
        case SVCRT_FAULT_USGFAULT:     return "USGFAULT";
        case SVCRT_FAULT_INSTALLFAIL:  return "INSTALLFAIL";
        default:                       return "?";
    }
}

/* ============================================================
 * info：内核与分区概览
 * ============================================================ */
/* 池地图：按地址升序列出池内每个镜像的落点与跨度。
 * 线性扫「下一个更大的落点」而不是排序整个数组：记录数上限是 16，
 * 这点平方开销无所谓，却省掉一份临时副本。 */
static void sh_pool_map(const svcrt_partition_table_t *pt)
{
    uint32 last = 0u;
    uint32 printed = 0u;
    uint32 n;

    ark_shell_printf("\r\npool map (address order)\r\n");

    for(n = 0u; (n < pt->slot_max) && (n < SVCRT_SLOT_ARRAY_MAX); n++)
    {
        uint32 i;
        uint32 best = 0u;
        uint32 best_idx = 0xFFFFFFFFu;

        for(i = 0u; (i < pt->slot_max) && (i < SVCRT_SLOT_ARRAY_MAX); i++)
        {
            uint32 base = pt->slot_base[i];

            if((pt->slot_type[i] == SVCRT_SLOT_FREE) || (base == 0u))
            {
                continue;
            }

            if((base < last) || ((best_idx != 0xFFFFFFFFu) && (base >= best)))
            {
                continue;
            }

            best     = base;
            best_idx = i;
        }

        if(best_idx == 0xFFFFFFFFu)
        {
            break;
        }

        ark_shell_printf("  0x%08X +%-5uB  %-4s %-8s  slot %u\r\n",
                         best, pt->slot_size[best_idx],
                         slot_type_name(pt->slot_type[best_idx]),
                         slot_state_name(pt->slot_state[best_idx]),
                         best_idx);

        last = best + 1u;
        printed++;
    }

    if(printed == 0u)
    {
        ark_shell_printf("  (pool is empty)\r\n");
    }
}

static int cmd_info(int argc, char *argv[])
{
    svcrt_partition_table_t *pt = svcrt_ptable_get();
    uint32 used = 0u;
    uint32 total = 0u;
    uint32 frag = 0u;

    (void)argc;
    (void)argv;

    (void)svcrt_ptable_stats(&used, &total, &frag);

    ark_shell_printf("\r\nSVCrtOS kernel shell\r\n");
    ark_shell_printf("build     : " __DATE__ " " __TIME__ "\r\n");
    ark_shell_printf("partition : ABI v%u, hw 0x%08X\r\n",
                     (uint32)pt->version, pt->hw_compat_id);
    ark_shell_printf("kernel    : flash 0x%08X +%uK, ram 0x%08X +%uK\r\n",
                     pt->kernel_base, pt->kernel_size / 1024u,
                     pt->kernel_ram_base, pt->kernel_ram_size / 1024u);
    /* 池：按 1KB 分配单元细粒度落位，尾部 pool_reserve 个扇区留作压实余量 */
    ark_shell_printf("pool      : 0x%08X +%uK (%u sectors of %uK), unit %uB, reserve %u\r\n",
                     pt->pool_base, pt->pool_size / 1024u,
                     pt->pool_units, pt->pool_sector / 1024u,
                     pt->pool_alloc_unit, pt->pool_reserve);
    ark_shell_printf("pool used : %uK of %uK, free %uK, fragments %u\r\n",
                     used / 1024u, total / 1024u,
                     (total - used) / 1024u, frag);
    ark_shell_printf("image ram : 0x%08X +%uK, block %uB..%uK, %u slots\r\n",
                     pt->image_ram_base, pt->image_ram_total / 1024u,
                     pt->image_ram_min_block, pt->image_ram_max_block / 1024u,
                     pt->slot_max);
    ark_shell_printf("tasks     : %d used / %u max\r\n",
                     (int)svcrt_task_count, (uint32)SVCRT_TASK_MAX_NUM);
    ark_shell_printf("tick      : %u ms, period %u us\r\n",
                     svcrt_kernel_get_time(), (uint32)SVCRT_TICK_PERIOD_US);

    sh_pool_map(pt);

    return 0;
}

/* ============================================================
 * app：App 影像列表 / 启动 / 停止
 * @details v4 起 App 与驱动共用同一个镜像池，
 *          因此命令里的 <slot> 是“分区表记录号”，
 *          列表仅展示类型匹配的记录。
 * ============================================================ */
static int cmd_app(int argc, char *argv[])
{
    svcrt_partition_table_t *pt = svcrt_ptable_get();
    uint32 i;
    uint32 slot;

    if((argc >= 2) && (strcmp(argv[1], "start") == 0))
    {
        if((argc < 3) || (parse_u32(argv[2], &slot) != 0) ||
           (slot >= pt->slot_max) || (pt->slot_type[slot] != SVCRT_SLOT_APP))
        {
            sh_out("usage: app start <slot>   (slot id from 'app list')\r\n");
            return -1;
        }

        if(svcrt_loader_start_manual(slot) > 0)
        {
            sh_out("app: started\r\n");
        }
        else
        {
            sh_out("app: start failed (see 'crash' for a held slot; state in 'app list')\r\n");
        }
        return 0;
    }

    if((argc >= 2) && (strcmp(argv[1], "stop") == 0))
    {
        if((argc < 3) || (parse_u32(argv[2], &slot) != 0) ||
           (slot >= pt->slot_max) || (pt->slot_type[slot] != SVCRT_SLOT_APP))
        {
            sh_out("usage: app stop <slot>   (slot id from 'app list')\r\n");
            return -1;
        }

        if(svcrt_loader_stop(slot) == 0)
        {
            sh_out("app: stopped (image kept in flash)\r\n");
        }
        else
        {
            sh_out("app: stop failed (slot must be LOADED/RUNNING)\r\n");
        }
        return 0;
    }

    if((argc >= 2) && (strcmp(argv[1], "uninstall") == 0))
    {
        if((argc < 3) || (parse_u32(argv[2], &slot) != 0) ||
           (slot >= pt->slot_max) || (pt->slot_type[slot] != SVCRT_SLOT_APP))
        {
            sh_out("usage: app uninstall <slot>   (erases the image and reclaims space)\r\n");
            return -1;
        }

        {
            int32 rc;

            rc = svcrt_loader_uninstall(slot);

            if(rc > 0)
            {
                sh_out("app: uninstalled (image invalidated, its sector was erased)\r\n");
            }
            else if(rc == 0)
            {
                sh_out("app: uninstalled (image invalidated; space returns once its sector is free)\r\n");
            }
            else
            {
                sh_out("app: uninstall failed (raw images are not managed here)\r\n");
            }
        }
        return 0;
    }

    if((argc >= 2) && (strcmp(argv[1], "install") == 0))
    {
        if(argc < 3)
        {
            sh_out("usage: app install <path> [slot]   (path is a .svcapp in the mounted volume)\r\n");
            return -1;
        }

        if(argc >= 4)
        {
            /* Same contract as the serial 'install [slot]': a slot only means
             * something in fixed-slot mode, where the landing address comes
             * from the device layout table instead of from the pool. */
            uint32 want;
            const svcrt_cfg_slot_t *cs;

            if((svcrt_ptable_get()->layout_mode != (uint32)SVCRT_LAYOUT_MODE_FIXED) ||
               (parse_u32(argv[3], &want) != 0) ||
               (want >= svcrt_layout_slot_count()))
            {
                sh_out("usage: app install <path> [slot]   (a slot is only valid in fixed-slot mode)\r\n");
                return -1;
            }

            cs = svcrt_layout_slot(want);

            if(cs == 0)
            {
                sh_out("app install: no such slot\r\n");
                return -1;
            }

            svcrt_loader_slot_hint_set((int32)want);
            ark_shell_printf("app install: fixed slot %u -> 0x%08X\r\n",
                             (unsigned)want, (unsigned)cs->base);
        }
        else
        {
            svcrt_loader_slot_hint_set(-1);
        }

        if(!svcrt_fs_mounted())
        {
            /* Name the missing prerequisite: "failed, see fault" would
             * send the operator after a fault that was never recorded. */
            sh_out("app install: no file system mounted (try 'fs mount')\r\n");
            return -1;
        }

        /* No one-shot window here: the image is already on the device, so
         * the console keeps working while it is being installed. */
        ark_shell_printf("\r\napp install: %s\r\n", argv[2]);

        {
            int32 rc = svcrt_installer_from_file(argv[2]);

            if(rc >= 0)
            {
                ark_shell_printf("app install: ok, slot %d\r\n", (int)rc);
            }
            else
            {
                sh_out("app install: failed (see 'fault'; check the path with 'fs ls')\r\n");
            }
        }

        return 0;
    }

    if((argc >= 2) && (strcmp(argv[1], "list") != 0))
    {
        sh_out("usage: app [list | install <path> [slot] | start <slot> | stop <slot> | uninstall <slot>]\r\n");
        return -1;
    }

    /* 落点是运行期分配出来的，所以列出来的是真实地址与真实占用字节数，
     * 而不是链接期的固定槽位。 */
    ark_shell_printf("\r\nid  type  state     auto  task  crash  held       base        size    ram         entry\r\n");
    for(i = 0u; (i < pt->slot_max) && (i < SVCRT_SLOT_ARRAY_MAX); i++)
    {
        if(pt->slot_type[i] != SVCRT_SLOT_APP)
        {
            continue;
        }

        ark_shell_printf(" %u   %-4s  %-8s  %-4s  %-4u  %-5u  %-9s  0x%08X  %-6u  0x%08X  0x%08X\r\n",
                         i,
                         slot_type_name(pt->slot_type[i]),
                         slot_state_name(pt->slot_state[i]),
                         (pt->slot_autostart[i] != 0u) ? "yes" : "no",
                         pt->slot_task_id[i],
                         pt->slot_crash_cnt[i],
                         svcrt_crash_reason_name(svcrt_crash_disabled(i)),
                         pt->slot_base[i],
                         pt->slot_size[i],
                         pt->slot_ram_base[i],
                         pt->slot_entry[i]);
    }
    return 0;
}

/* ============================================================
 * drv：驱动镜像列表 / 启动 / 停止
 * ============================================================ */
static int cmd_drv(int argc, char *argv[])
{
    svcrt_partition_table_t *pt = svcrt_ptable_get();
    uint32 i;
    uint32 slot;

    if((argc >= 2) && (strcmp(argv[1], "start") == 0))
    {
        if((argc < 3) || (parse_u32(argv[2], &slot) != 0) ||
           (slot >= pt->slot_max) || (pt->slot_type[slot] != SVCRT_SLOT_DRIVER))
        {
            sh_out("usage: drv start <slot>   (slot id from 'drv list')\r\n");
            return -1;
        }

        if(svcrt_loader_start_driver_manual(slot) > 0)
        {
            sh_out("drv: started\r\n");
        }
        else
        {
            sh_out("drv: start failed (see 'crash' for a held slot; state in 'drv list')\r\n");
        }
        return 0;
    }

    if((argc >= 2) && (strcmp(argv[1], "stop") == 0))
    {
        if((argc < 3) || (parse_u32(argv[2], &slot) != 0) ||
           (slot >= pt->slot_max) || (pt->slot_type[slot] != SVCRT_SLOT_DRIVER))
        {
            sh_out("usage: drv stop <slot>   (slot id from 'drv list')\r\n");
            return -1;
        }

        if(svcrt_loader_stop_driver_slot(slot) == 0)
        {
            sh_out("drv: stopped (image kept in flash)\r\n");
        }
        else
        {
            sh_out("drv: stop failed (slot must be LOADED/RUNNING)\r\n");
        }
        return 0;
    }

    if((argc >= 2) && (strcmp(argv[1], "uninstall") == 0))
    {
        if((argc < 3) || (parse_u32(argv[2], &slot) != 0) ||
           (slot >= pt->slot_max) || (pt->slot_type[slot] != SVCRT_SLOT_DRIVER))
        {
            sh_out("usage: drv uninstall <slot>   (erases the image and reclaims space)\r\n");
            return -1;
        }

        {
            int32 rc;

            rc = svcrt_loader_uninstall(slot);

            if(rc > 0)
            {
                sh_out("drv: uninstalled (image invalidated, its sector was erased)\r\n");
            }
            else if(rc == 0)
            {
                sh_out("drv: uninstalled (image invalidated; space returns once its sector is free)\r\n");
            }
            else
            {
                sh_out("drv: uninstall failed (raw images are not managed here)\r\n");
            }
        }
        return 0;
    }

    if((argc >= 2) && (strcmp(argv[1], "list") != 0))
    {
        sh_out("usage: drv [list | start <slot> | stop <slot> | uninstall <slot>]\r\n");
        return -1;
    }

    ark_shell_printf("\r\nid  type  state     auto  task  crash  held       base        size    ram         entry\r\n");
    for(i = 0u; (i < pt->slot_max) && (i < SVCRT_SLOT_ARRAY_MAX); i++)
    {
        if(pt->slot_type[i] != SVCRT_SLOT_DRIVER)
        {
            continue;
        }

        ark_shell_printf(" %u   %-4s  %-8s  %-4s  %-4u  %-5u  %-9s  0x%08X  %-6u  0x%08X  0x%08X\r\n",
                         i,
                         slot_type_name(pt->slot_type[i]),
                         slot_state_name(pt->slot_state[i]),
                         (pt->slot_autostart[i] != 0u) ? "yes" : "no",
                         pt->slot_task_id[i],
                         pt->slot_crash_cnt[i],
                         svcrt_crash_reason_name(svcrt_crash_disabled(i)),
                         pt->slot_base[i],
                         pt->slot_size[i],
                         pt->slot_ram_base[i],
                         pt->slot_entry[i]);
    }
    return 0;
}

/* ============================================================
 * task：任务表概览
 * ============================================================ */
static int cmd_task(int argc, char *argv[])
{
    int32 i;

    if(argc >= 2)
    {
        /* One suspended task, its saved context frame, and nothing else.
         * A blocked task is read-only here: the frame holds the return
         * address of the call that never came back, which is the only
         * honest answer to "where is it stuck". */
        uint32 id = 0u;
        volatile uint32 *sp;
        int32 n;

        if((parse_u32(argv[1], &id) != 0) ||
           ((int32)id >= svcrt_task_count) ||
           (svcrt_task_table[id].stack_ptr == 0u))
        {
            ark_shell_printf("task: no such task\r\n");
            return 1;
        }
        sp = (volatile uint32 *)svcrt_task_table[id].stack_ptr;
        ark_shell_printf("task %u sp=0x%08X st=%s frame words:\r\n",
                         id, (uint32)svcrt_task_table[id].stack_ptr,
                         task_state_name((uint32)svcrt_task_table[id].status));
        for(n = 0; n < 56; n++)
        {
            /* Raw, unfiltered: the frame layout depends on whether the task
             * owns a floating point extended frame, and guessing which word
             * is the PC is how a backtrace turns into a wrong answer. */
            ark_shell_printf("  sp+%-3u  0x%08X\r\n", (uint32)(n * 4), sp[n]);
        }
        if(argc >= 3)
        {
            /* Wake reason this task actually read on its last blocking
             * call, and how many times it blocked at all.  4 = HANDOFF
             * observed, 0 = a wake that carried no token claim. */
            extern volatile uint8 svcrt_dbg_wake_last[];
            extern volatile uint8 svcrt_dbg_wake_hits[];

            ark_shell_printf("  blocks=%u last_reason=%u\r\n",
                             (unsigned int)svcrt_dbg_wake_hits[id],
                             (unsigned int)svcrt_dbg_wake_last[id]);
        }
        return 0;
    }

    ark_shell_printf("\r\nid  prio  state     period  wait    peak/low  entry\r\n");
    for(i = 0; i < svcrt_task_count; i++)
    {
        ark_shell_printf(" %-3d %-5u %-8s  %-6d  %-6d  %-8u  0x%08X\r\n",
                         (int)i,
                         (uint32)svcrt_task_table[i].priority,
                         task_state_name((uint32)svcrt_task_table[i].status),
                         svcrt_task_table[i].period,
                         svcrt_task_table[i].wait_time,
                         svcrt_task_table[i].stack_peak_low,
                         svcrt_task_table[i].entry);
    }
    ark_shell_printf("tasks: %d used / %u max\r\n",
                     (int)svcrt_task_count, (uint32)SVCRT_TASK_MAX_NUM);
    return 0;
}

/* ============================================================
 * sched：调度器就绪队列自检
 *
 * 内核的就绪集现在是「256 位两级位图 + 每优先级双向循环链表」，
 * 选任务 O(1)。复杂度换来的风险是结构性的：一旦链表/位图与任务表
 * 真实状态不一致，调度器会静默选错任务——不报错、不崩溃。
 * 所以这里保留一条全表扫描的慢路径专门用来对账：
 *   sched         打印就绪队列概况 + 对账结果
 * 一致返回 0，不一致返回不一致条目数（可直接用于脚本判据）。
 * ============================================================ */
/* ============================================================
 * audit: does the shared state still satisfy its own invariants?
 * Read-only, so it is safe to run while the system keeps working.
 * ============================================================ */
/* ============================================================
 * guard: watchdog state and the per-slot heartbeat contract
 * ============================================================ */
static const char *guard_state_name(uint32 state)
{
    switch(state)
    {
    case SVCRT_TASK_INVALID: return "invalid";
    case SVCRT_TASK_READY:   return "ready";
    case SVCRT_TASK_WAIT:    return "wait";
    case SVCRT_TASK_RUNNING: return "run";
    default:                 return "-";
    }
}

static void guard_print_age(uint32 age_ms)
{
    if(age_ms == 0xFFFFFFFFu)
    {
        ark_shell_printf("-");
        return;
    }
    ark_shell_printf("%u", age_ms);
}

static int cmd_guard(int argc, char *argv[])
{
    svcrt_guard_status_t st;
    svcrt_guard_task_t t;
    uint32 i;

    (void)argc;
    (void)argv;

    svcrt_guard_status(&st);

    if(st.enabled == 0u)
    {
        sh_out("watchdog: disabled by the board (SVCRT_WDG_ENABLE = 0)\r\n");
    }
    else if(st.timeout_ms == 0u)
    {
        sh_out("watchdog: requested but NOT running (the port refused it)\r\n");
    }
    else
    {
        ark_shell_printf("watchdog: armed, hardware timeout %u ms\r\n", st.timeout_ms);
    }
    ark_shell_printf("feeds=%u starves=%u verdict=0x%X audit_mask=0x%X audit_runs=%u\r\n",
                     st.feeds, st.starves, st.verdict, st.audit_mask, st.audit_runs);
    ark_shell_printf("contracts: %u declared, %u violating now\r\n",
                     st.declared, st.violating);
    ark_shell_printf("\r\ntask slot period beat_ms svc_ms cpu_ms misses state\r\n");

    for(i = 0u; i < svcrt_guard_task_slots(); i++)
    {
        if(svcrt_guard_task_at(i, &t) != 0)
        {
            break;
        }
        if((t.task_id == 0) && (t.declared == 0u))
        {
            continue;
        }
        ark_shell_printf("%u %d %u ", (uint32)t.task_id, (int)t.slot, t.period_ms);
        guard_print_age(t.beat_age_ms);
        ark_shell_printf(" ");
        guard_print_age(t.svc_age_ms);
        ark_shell_printf(" ");
        guard_print_age(t.cpu_age_ms);
        ark_shell_printf(" %u %s\r\n", t.misses, guard_state_name(t.state));
    }

    if(st.declared == 0u)
    {
        sh_out("\r\nnote: no image declared a heartbeat period, so the kernel has\r\n"
               "      no health verdict to give for any slot (see svcrt_heartbeat).\r\n");
    }

    return 0;
}

/* ============================================================
 * crash：跨复位崩溃账本
 * @details 这是唯一能在复位后活下来的那份计数：它放在共享 RAM 尾部的
 *          UNINIT 区（CRASH_LOG_BASE），上电清零、复位不清零。
 *          因此“镜像崩溃 -> 整机复位 -> 镜像又自启 -> 再崩”这条环
 *          能被记满上限并落成禁用，板子活下来。
 * @note 掉电即清零是设计取舍而不是遗漏：复位才是要数的那个事件，
 *       断电是操作者说“重新开始”。详见 svcrt_crash.h。
 * ============================================================ */
static int cmd_crash(int argc, char *argv[])
{
    svcrt_partition_table_t *pt = svcrt_ptable_get();
    uint32 i;
    uint32 shown = 0u;

    (void)argc;
    (void)argv;

    ark_shell_printf("journal: 0x%08X +%u bytes (uninitialized RAM: kept across reset, cleared by power-on)\r\n",
                     svcrt_crash_area_base(), svcrt_crash_area_size());
    ark_shell_printf("boots=%u seq=%u\r\n", svcrt_crash_boots(), svcrt_crash_seq());
    ark_shell_printf("\r\nslot type  faults last        hold      held_at\r\n");

    for(i = 0u; (i < pt->slot_max) && (i < SVCRT_SLOT_ARRAY_MAX); i++)
    {
        uint32 cnt = svcrt_crash_count(i);
        uint32 hold = svcrt_crash_disabled(i);

        if((cnt == 0u) && (hold == 0u))
        {
            continue;
        }

        shown++;
        ark_shell_printf(" %u   %-4s  %-6u  %-10s  %-8s  %u\r\n",
                         i,
                         slot_type_name(pt->slot_type[i]),
                         cnt,
                         svcrt_crash_reason_name(svcrt_crash_last_reason(i)),
                         svcrt_crash_reason_name(hold),
                         svcrt_crash_disabled_tick(i));
    }

    if(shown == 0u)
    {
        sh_out("\r\nno slot has a fault on record since the last power-on.\r\n");
    }
    else
    {
        sh_out("\r\nhold = the kernel will not autostart this slot. 'app start' / 'drv start'\r\n"
               "       is the explicit retry that clears it (the image must still validate).\r\n");
    }

    return 0;
}

static int cmd_audit(int argc, char *argv[])
{
    svcrt_audit_result_t r;
    uint32 bit;
    uint32 bad = 0u;

    (void)argc;
    (void)argv;

    svcrt_audit_run(&r);

    for(bit = 1u; bit <= SVCRT_AUDIT_BIT_MAX; bit <<= 1)
    {
        const char *name = svcrt_audit_name(bit);

        if(name == 0)
        {
            continue;
        }
        if((r.mask & bit) != 0u)
        {
            bad++;
        }
        ark_shell_printf("  %s: %s\r\n", name, ((r.mask & bit) != 0u) ? "BAD" : "ok");
    }

    ark_shell_printf("\r\nchecks=%u bad_groups=%u mask=0x%08X first_offender=%u\r\n",
                     r.checks, bad, r.mask, r.index);

    if(bad != 0u)
    {
        /* The groups above say which structure and which index. The cause is
         * not guessed here: a wrong cause costs a round of checking the wrong
         * thing, which is worse than an unanswered question. */
        sh_out("audit: INCONSISTENT (see the group lines above)\r\n");
        return (int)bad;
    }

    sh_out("audit: all structures self-consistent\r\n");
    return 0;
}

static int cmd_sched(int argc, char *argv[])
{
    int32  bad;
    int32  top;
    uint32 n;

    (void)argc;
    (void)argv;

    n   = svcrt_sched_ready_count();
    top = svcrt_ready_top();
    bad = svcrt_sched_check();

    ark_shell_printf("\r\nready=%u top=%d\r\n", n, (int)top);
    if(bad != 0)
    {
        /* 光报一个数字没法定位：逐条打印「哪个任务、差在哪」。
         * 只看两种偏差——状态与就绪链不符（会漏调度或选到不该选的任务）。 */
        int32 i;

        for(i = 0; i < svcrt_task_count; i++)
        {
            const svcrt_task_t *p = &svcrt_task_table[i];
            uint8 on = svcrt_ready_contains(i);
            uint8 should = ((p->status == SVCRT_TASK_READY) ||
                            (p->status == SVCRT_TASK_RUNNING)) ? 1u : 0u;

            if(on != should)
            {
                ark_shell_printf("  #%d prio=%u status=%d on_ready=%u next=%d prev=%d\r\n",
                                 (int)i, (uint32)p->priority, (int)p->status,
                                 (uint32)on, (int)p->ready_next, (int)p->ready_prev);
            }
        }
        ark_shell_printf("sched: INCONSISTENT, %d mismatch(es)\r\n", (int)bad);
        return (int)bad;
    }

    sh_out("sched: consistent (bitmap/links == task table)\r\n");
    return 0;
}


/* ============================================================
 * fault：故障记录
 * ============================================================ */
static int cmd_fault(int argc, char *argv[])
{
    int32 n;
    int32 i;

    (void)argc;
    (void)argv;

    n = svcrt_fault_record_count_internal();
    if(n <= 0)
    {
        sh_out("no fault recorded\r\n");
        return 0;
    }

    ark_shell_printf("\r\n#   tick      task  type\r\n");
    for(i = 0; i < n; i++)
    {
        const svcrt_fault_record_t *r = svcrt_fault_record_get(i);

        if(r == 0)
        {
            continue;
        }

        ark_shell_printf(" %-3d %-9u %-5d %s\r\n",
                         (int)i, r->tick, (int)r->task_id, fault_type_name(r->type));
    }
    return 0;
}

/* ============================================================
 * install：打开一次性安装窗口
 *
 * 窗口期间本任务不跑 ark_shell_run()，串口读权全部交给安装器；
 * 因此不存在「两个读者把镜像字节流随机分掉」的问题。
 * ============================================================ */
static int cmd_install(int argc, char *argv[])
{
    int32 slot;
    int32 dev;

    if(argc >= 2)
    {
        uint32 want;
        const svcrt_cfg_slot_t *cs;

        if((svcrt_ptable_get()->layout_mode != (uint32)SVCRT_LAYOUT_MODE_FIXED) ||
           (parse_u32(argv[1], &want) != 0) ||
           (want >= svcrt_layout_slot_count()))
        {
            sh_out("usage: install [slot]   (a slot number is only valid in fixed-slot mode)\r\n");
            return -1;
        }

        cs = svcrt_layout_slot(want);

        if(cs == 0)
        {
            sh_out("install: no such slot\r\n");
            return -1;
        }

        svcrt_loader_slot_hint_set((int32)want);
        ark_shell_printf("install: fixed slot %u -> 0x%08X\r\n",
                         (unsigned)want, (unsigned)cs->base);
    }
    else
    {
        svcrt_loader_slot_hint_set(-1);
    }

    sh_out("\r\ninstall: waiting for one .svcapp image on " SHELL_DEV_NAME "\r\n");
    sh_out("install: send the file now; console input is ignored while waiting\r\n");

    dev = svcrt_shell_uart_open();

    slot = svcrt_installer_run_once(dev, (uint32)SHELL_INSTALL_TIMEOUT_MS);

    /* Drop whatever arrived after the frame was already decided. A host that
     * sends header + relocation table as one burst still has bytes on the
     * wire when the header alone is rejected (compat id, size...); anything
     * the installer did not consume would be replayed to the shell as a
     * command line - seen in practice as "Command not found: )8PX`hpdrv".
     * The window is over here, so nothing legitimate is in flight. */
    {
        uint8 sink[32];
        uint32 spins = 0u;

        while((spins < 64u) &&
              (svcrt_dev_read_internal(dev, sink, (int32)sizeof(sink)) > 0))
        {
            spins++;
        }
    }

    if(slot >= 0)
    {
        ark_shell_printf("install: ok, slot %d\r\n", (int)slot);
    }
    else
    {
        sh_out("install: failed or timed out (see 'fault')\r\n");
    }
    return 0;
}

/* ============================================================
 * 命令注册（紧跟 ark_shell_commands_init() 之后追加）
 * ============================================================ */

/* ============================================================
 * 用户命令（SVC 0x1A）：注册表 + 内核跳板
 *
 * 内核不改 ark_shell（上游原样引入），而是在命令表里追加一条指向本文件的
 * 跳板命令：
 *   - 命令名与帮助文本注册时复制进内核 RAM，镜像卸载后不留下悬垂指针；
 *   - 跳板执行前先确认处理器仍落在某个「已装载」镜像的区间内，
 *     镜像被卸载后命令自动失效并给出提示，而不是跳到已擦除的 Flash 上。
 * ============================================================ */

#define SVCRT_USHELL_MAX_CMDS   (8)

/* 控制台分段输出缓冲：SVC 处理跑在用户任务的 PSP 上，栈很浅 */
#define SVCRT_USHELL_OUT_CHUNK  (64)

typedef struct
{
    char     name[SVCRT_USHELL_NAME_MAX + 1];
    char     help[SVCRT_USHELL_HELP_MAX + 1];
    int    (*func)(int argc, char **argv);
    uint32   max_args;
    uint32   in_use;
    uint32   owner_task;    /* task that registered it; 0 = kernel, kept */
} svcrt_ushell_entry_t;

static svcrt_ushell_entry_t g_ushell[SVCRT_USHELL_MAX_CMDS];
static char g_ushell_out[SVCRT_USHELL_OUT_CHUNK + 1];

/* 命令名比较：不区分大小写，且要求两边同时结束 */
static int ushell_name_eq(const char *a, const char *b)
{
    if((a == 0) || (b == 0))
    {
        return 0;
    }

    while((*a != '\0') && (*b != '\0'))
    {
        uint32 ca = (uint32)(uint8)*a;
        uint32 cb = (uint32)(uint8)*b;

        if((ca >= 'A') && (ca <= 'Z'))
        {
            ca += 32u;
        }
        if((cb >= 'A') && (cb <= 'Z'))
        {
            cb += 32u;
        }
        if(ca != cb)
        {
            return 0;
        }
        a++;
        b++;
    }

    return ((*a == '\0') && (*b == '\0')) ? 1 : 0;
}

static void ushell_copy(char *dst, uint32 cap, const char *src)
{
    uint32 i = 0u;

    if((dst == 0) || (cap == 0u))
    {
        return;
    }

    if(src != 0)
    {
        while((src[i] != '\0') && ((i + 1u) < cap))
        {
            dst[i] = src[i];
            i++;
        }
    }

    dst[i] = '\0';
}

/* Defined below, next to the dispatch itself: the drop helper needs the
 * trampoline address to find its row in the command table. */
static int svcrt_shell_ext_trampoline(int argc, char **argv);

/* Remove one entry: the trampoline row is deleted by shifting the later rows
 * down (their order is the help display order, so no hole is left), then the
 * slot itself is cleared. Register / unregister / task-exit all go through
 * here, so the bookkeeping cannot drift between the three.
 * Runs with interrupts off (called from a critical section); it only touches
 * this file's tables and performs no blocking call. */
static void ushell_drop(uint32 i)
{
    uint32 k;

    for(k = 0u; (k < (uint32)g_cmd_count) && (k < (uint32)ARK_SHELL_MAX_COMMANDS); k++)
    {
        if((g_cmd_table[k].func == svcrt_shell_ext_trampoline) &&
           (g_cmd_table[k].name == g_ushell[i].name))
        {
            uint32 j;

            for(j = k; (j + 1u) < (uint32)g_cmd_count; j++)
            {
                g_cmd_table[j] = g_cmd_table[j + 1u];
            }
            g_cmd_count--;
            break;
        }
    }

    g_ushell[i].in_use = 0u;
    g_ushell[i].func = 0;
    g_ushell[i].owner_task = 0u;
    g_ushell[i].name[0] = '\0';
    g_ushell[i].help[0] = '\0';
}

/* 处理器是否还活着：必须仍落在某个已装载镜像的 Flash 区间内 */
static int ushell_func_alive(const void *p)
{
    const svcrt_partition_table_t *pt = svcrt_ptable_get();
    uint32 addr = (uint32)p;
    uint32 i;

    if((p == 0) || (pt == 0))
    {
        return 0;
    }

    for(i = 0u; (i < pt->slot_max) && (i < SVCRT_SLOT_ARRAY_MAX); i++)
    {
        if((pt->slot_type[i] == SVCRT_SLOT_FREE) || (pt->slot_base[i] == 0u))
        {
            continue;
        }
        if((pt->slot_state[i] != SVCRT_APP_SLOT_LOADED) &&
           (pt->slot_state[i] != SVCRT_APP_SLOT_RUNNING))
        {
            continue;
        }
        if((addr >= pt->slot_base[i]) &&
           (addr < (pt->slot_base[i] + pt->slot_size[i])))
        {
            return 1;
        }
    }

    return 0;
}

/* 所有用户命令共用同一个跳板：用 argv[0] 反查注册表项 */
static int svcrt_shell_ext_trampoline(int argc, char **argv)
{
    uint32 i;

    for(i = 0u; i < SVCRT_USHELL_MAX_CMDS; i++)
    {
        if((g_ushell[i].in_use == 0u) || (argv == 0) || (argc <= 0) || (argv[0] == 0))
        {
            continue;
        }

        if(ushell_name_eq(g_ushell[i].name, argv[0]) == 0)
        {
            continue;
        }

        if(ushell_func_alive((const void *)g_ushell[i].func) == 0)
        {
            ark_shell_printf("%s: handler no longer available (image unloaded?)\r\n",
                             g_ushell[i].name);
            return -1;
        }

        {
            /* The callback belongs to an App but runs on the kernel shell task
             * stack: the buffers it renders match no user RAM window, so every
             * buffered SVC service call from inside the callback would be
             * rejected. Raise the flag for the call and drop it right after. */
            extern uint32 svcrt_kernel_ushell_active;
            int rc;

            svcrt_kernel_ushell_active = 1u;
            rc = g_ushell[i].func(argc, argv);
            svcrt_kernel_ushell_active = 0u;
            return rc;
        }
    }

    sh_out("user command not available\r\n");
    return -1;
}

int32 svcrt_shell_ext_register(const svcrt_ushell_cmd_t *cmd)
{
    uint32 i;
    uint32 len;

    if((cmd == 0) || (cmd->name == 0) || (cmd->func == 0))
    {
        return SVCRT_USHELL_ERR_PARAM;
    }

    len = (uint32)strlen(cmd->name);
    if((len == 0u) || (len > (uint32)SVCRT_USHELL_NAME_MAX))
    {
        return SVCRT_USHELL_ERR_PARAM;
    }

    if(g_cmd_count >= ARK_SHELL_MAX_COMMANDS)
    {
        return SVCRT_USHELL_ERR_FULL;
    }

    for(i = 0u; i < SVCRT_USHELL_MAX_CMDS; i++)
    {
        if((g_ushell[i].in_use != 0u) && (ushell_name_eq(g_ushell[i].name, cmd->name) != 0))
        {
            return SVCRT_USHELL_ERR_DUP;
        }
    }

    for(i = 0u; i < SVCRT_USHELL_MAX_CMDS; i++)
    {
        if(g_ushell[i].in_use == 0u)
        {
            break;
        }
    }
    if(i >= SVCRT_USHELL_MAX_CMDS)
    {
        return SVCRT_USHELL_ERR_FULL;
    }

    ushell_copy(g_ushell[i].name, (uint32)sizeof(g_ushell[i].name), cmd->name);
    ushell_copy(g_ushell[i].help, (uint32)sizeof(g_ushell[i].help), cmd->help);
    g_ushell[i].func = cmd->func;
    g_ushell[i].max_args = cmd->max_args;
    g_ushell[i].in_use = 1u;
    /* Remember who owns it. A kernel side registration (task id 0) is not tied
     * to any task lifetime and must survive every App stopping. */
    g_ushell[i].owner_task = (svcrt_current_task_id > 0)
                             ? (uint32)svcrt_current_task_id : 0u;

    g_cmd_table[g_cmd_count].name = g_ushell[i].name;
    g_cmd_table[g_cmd_count].func = svcrt_shell_ext_trampoline;
    g_cmd_table[g_cmd_count].help = g_ushell[i].help;
    g_cmd_table[g_cmd_count].max_args = (int)cmd->max_args;
    g_cmd_count++;

    return SVCRT_USHELL_OK;
}

int32 svcrt_shell_ext_unregister(const char *name)
{
    uint32 i;

    if(name == 0)
    {
        return SVCRT_USHELL_ERR_PARAM;
    }

    for(i = 0u; i < SVCRT_USHELL_MAX_CMDS; i++)
    {
        if((g_ushell[i].in_use == 0u) || (ushell_name_eq(g_ushell[i].name, name) == 0))
        {
            continue;
        }

        /* 摘掉命令表里的跳板条目：表内顺序即 help 的显示顺序，
         * 所以用后项前移而不是留空洞 */
        ushell_drop(i);

        return SVCRT_USHELL_OK;
    }

    return SVCRT_USHELL_ERR_NOTFND;
}

int32 svcrt_shell_print_n(const char *msg, uint32 len)
{
    uint32 off = 0u;

    if(msg == 0)
    {
        return SVCRT_USHELL_ERR_PARAM;
    }

    while(off < len)
    {
        uint32 n = len - off;
        uint32 i;

        if(n > (uint32)SVCRT_USHELL_OUT_CHUNK)
        {
            n = (uint32)SVCRT_USHELL_OUT_CHUNK;
        }

        for(i = 0u; i < n; i++)
        {
            g_ushell_out[i] = msg[off + i];
        }
        g_ushell_out[n] = '\0';

        sh_out(g_ushell_out);
        off += n;
    }

    return SVCRT_USHELL_OK;
}

/* ============================================================
 * log：查看 / 调整运行期日志级别（内核与所有 App、驱动同时生效）
 * ============================================================ */
static void syncinfo_row(const char *tag, int32 idx, int32 total,
                          const svcrt_sync_dbg_row_t *row)
{
    int32 i;

    if(idx == 0)
    {
        ark_shell_printf("\r\n%s  used  count  owner  creator  waiters\r\n", tag);
    }
    if(row->used == 0)
    {
        return;
    }
    ark_shell_printf(" %-3d %-5u %-6d %-6d %-8d ", (int)idx, (uint32)row->used,
                     (int)row->count, (int)row->owner_id, (int)row->creator_id);
    if(row->nwait == 0)
    {
        ark_shell_printf("-\r\n");
        return;
    }
    for(i = 0; i < row->nwait; i++)
    {
        ark_shell_printf("t%u%s", (unsigned int)row->waiter_id[i],
                         ((i + 1) < row->nwait) ? "," : "\r\n");
    }
    (void)total;
}

static int cmd_syncinfo(int argc, char *argv[])
{
    svcrt_sync_dbg_row_t row;
    int32 i;

    (void)argc;
    (void)argv;

    for(i = 0; i < (int32)svcrt_sync_sem_num(); i++)
    {
        if(svcrt_sync_sem_debug(i, &row) != 0)
        {
            break;
        }
        syncinfo_row("sem", i, (int32)svcrt_sync_sem_num(), &row);
    }
    for(i = 0; i < (int32)svcrt_sync_mtx_num(); i++)
    {
        if(svcrt_sync_mtx_debug(i, &row) != 0)
        {
            break;
        }
        syncinfo_row("mtx", i, (int32)svcrt_sync_mtx_num(), &row);
    }
    for(i = 0; i < (int32)svcrt_sync_cond_num(); i++)
    {
        if(svcrt_sync_cond_debug(i, &row) != 0)
        {
            break;
        }
        syncinfo_row("cond", i, (int32)svcrt_sync_cond_num(), &row);
    }
    for(i = 0; i < (int32)svcrt_sync_sem_num(); i++)
    {
        uint32 posts = 0u, takes = 0u, queued = 0u, last = 0u;
        uint32 woke = 0u, reason = 0u, also = 0u;
        uint32 qbefore = 0u;

        if(svcrt_sync_sem_debug(i, &row) != 0)
        {
            break;
        }
        if(row.used == 0)
        {
            continue;
        }
        svcrt_sync_sem_trace(i, &posts, &takes, &queued, &last);
        svcrt_sync_sem_handoff(i, &woke, &reason, &also);
        qbefore = svcrt_sync_sem_qbefore(i);
        ark_shell_printf("  sem %d: posts=%u takes=%u queued=%u last_waiter=t%u "
                         "qbefore=%u woke=t%u reason=%u\r\n",
                         (int)i, (unsigned int)posts, (unsigned int)takes,
                         (unsigned int)queued, (unsigned int)last,
                         (unsigned int)qbefore,
                         (unsigned int)woke, (unsigned int)reason);
    }
    ark_shell_printf("sync diagnostics: ghost=%u dup=%u\r\n",
                     (unsigned int)svcrt_diag_sync_ghost,
                     (unsigned int)svcrt_diag_sync_dup);
    ark_shell_printf("sync ghost  : n=%u idx=%d tid=t%u\r\n",
                     (unsigned int)dbg_sem_ghost_cnt,
                     (int)(int32)dbg_sem_ghost_idx,
                     (unsigned int)dbg_sem_ghost_tid);
    ark_shell_printf("sync appwait: hit=%u\r\n",
                     (unsigned int)dbg_sem_appq_hit);
    return 0;
}

static int cmd_log(int argc, char *argv[])
{
    const char *names[5] = { "off", "error", "warning", "info", "debug" };

    if(argc >= 2)
    {
        uint32 lvl;

        if((parse_u32(argv[1], &lvl) != 0) || (lvl > (uint32)SVCRT_LOG_DEBUG))
        {
            sh_out("usage: log <0..4>   (0=off 1=error 2=warn 3=info 4=debug)\r\n");
            return -1;
        }

        svcrt_log_set_level(lvl);
    }

    ark_shell_printf("log level: %u (%s)\r\n",
                     svcrt_log_get_level(), names[svcrt_log_get_level() & 7u]);

    /* Console health.  lock_giveup counts output units written without the
     * console lock (they may have been spliced); fifo_refused counts bytes
     * the transmit pipe refused, i.e. output that never left the MCU. */
    ark_shell_printf("console   : lock_giveup=%u tx_drop=%u B fifo_full_retries=%u\r\n",
                     svcrt_console_busy_count(), svcrt_console_tx_drop_count(),
                     svcrt_fifo_write_refused());
                     /* tx_drop is the number that matters: bytes that never
                      * left the MCU.  fifo_full_retries is attempts, not loss. */

    ark_shell_printf("fifo      : bad_header=%u bytes_lost=%u\r\n",
                     svcrt_fifo_bad_magic(), svcrt_fifo_bad_magic_bytes());

    /* Token conservation on the sync objects.  ghost counts wakes that carried
     * no SVCRT_WAKE_HANDOFF bit, i.e. successes the kernel refused to claim;
     * dup counts a task that tried to register twice in one waiter queue. */
    ark_shell_printf("sync      : ghost=%u dup=%u\r\n",
                     svcrt_diag_sync_ghost, svcrt_diag_sync_dup);
    /* Non-zero means a FIFO header was overwritten: input or output
     * through it is being dropped for good, not merely throttled. */

    return 0;
}

/* ============================================================
 * pool：池内还能装下多少（与 SVC 0x12 子命令 5 同一数据源）
 * ============================================================ */
static int cmd_pool(int argc, char *argv[])
{
    uint32 largest = 0u;
    uint32 total = svcrt_loader_pool_free(&largest);

    (void)argc;
    (void)argv;

    ark_shell_printf("\r\npool free : %u B (%uK)\r\n", total, total / 1024u);
    ark_shell_printf("largest   : %u B (%uK)\r\n", largest, largest / 1024u);

    /* 固定槽位模式下「池里还剩多少字节」并不回答「我还能装下一个镜像吗」：
     * 每个槽各占一块固定地址，能装多大事先已经由槽大小定死了。
     * 所以这里直接列出各槽自己还有没有位置，免得操作者拿一个与本次安装
     * 无关的数字去做决定。 */
    if(svcrt_ptable_get()->layout_mode == (uint32)SVCRT_LAYOUT_MODE_FIXED)
    {
        const svcrt_partition_table_t *pt = svcrt_ptable_get();
        const svcrt_cfg_slot_t *slots = svcrt_layout_slots();
        uint32 count = svcrt_layout_slot_count();
        uint32 i;

        sh_out("\r\nfixed slots\r\n");
        sh_out(" id  type  base        size     state\r\n");

        for(i = 0u; i < count; i++)
        {
            const char *state = "empty";
            uint32 j;

            for(j = 0u; (j < pt->slot_max) && (j < SVCRT_SLOT_ARRAY_MAX); j++)
            {
                if((pt->slot_type[j] != SVCRT_SLOT_FREE) &&
                   (pt->slot_base[j] == slots[i].base) &&
                   (pt->slot_size[j] == slots[i].size))
                {
                    state = slot_state_name(pt->slot_state[j]);
                    break;
                }
            }

            ark_shell_printf(" %-3u %-4s  0x%08X  %-8u  %s\r\n",
                             (unsigned)i, cfg_type_text(slots[i].type),
                             (unsigned)slots[i].base, (unsigned)slots[i].size,
                             state);
        }
    }

    return 0;
}

/* ============================================================
 * cfg：设备端布局配置区（CONFIG 区）
 *
 * 记录格式见 kernelsrc/include/svcrt_layout_def.h（与上位机工具共用），
 * 生成工具 tools/svcrt_layout.py，串口下发工具 tools/svcrt_cfg.py。
 *
 * 上位机协议（与 install 同构：窗口期间本任务不跑 ark_shell_run()，
 * 串口读权全部交给本命令，不存在两个读者分掉同一 FIFO 的问题）：
 *   1) 设备打印就绪行；
 *   2) 主机按 SVCRT_CFG_CHUNK_SIZE 字节分块下行，每块发完等一个流控字节：
 *      ACK(0x06) 表示「已收下，发下一块」，NAK(0x15) 表示「到此为止」；
 *   3) 第 4 块收满后设备校验并写入：成功发 ACK，失败先打印原因行再发 NAK。
 * 用二进制流而不是 hex 文本，是因为 ARK_SHELL_LINE_SIZE 只有 128 字节，
 * 512 字节的记录没法走命令行文本（见 ark_shell_config.h）。
 * ============================================================ */

#define SVCRT_CFG_CHUNK_SIZE     (128u)
#define SVCRT_CFG_CHUNK_COUNT    (SVCRT_CFG_RECORD_SIZE / SVCRT_CFG_CHUNK_SIZE)
#define SVCRT_CFG_CHUNK_TIMEOUT  (5000u)   /* 单块等待上限（ms） */
#define SVCRT_CFG_POLL_MS        (2u)      /* 空转一次让出 CPU 的步长（ms） */
#define SVCRT_CFG_FLOW_ACK       (0x06u)
#define SVCRT_CFG_FLOW_NAK       (0x15u)

/* 收满 len 字节；idle 累计到 timeout_ms 仍没收满就放弃。
 * 未收满时已收的字节就地丢弃（记录要么整条有效，要么整条不要）。 */
static int32 cfg_recv_exact(int32 dev, uint8 *buf, uint32 len, uint32 timeout_ms)
{
    uint32 got = 0u;
    uint32 idle = 0u;

    while(got < len)
    {
        int32 r = svcrt_dev_read_internal(dev, buf + got, (int32)(len - got));

        if(r > 0)
        {
            got += (uint32)r;
            idle = 0u;
            continue;
        }

        if(idle >= timeout_ms)
        {
            return -1;
        }

        svcrt_task_wait_internal(SVCRT_CFG_POLL_MS);
        idle += SVCRT_CFG_POLL_MS;
    }

    return 0;
}

static void cfg_send_flow(int32 dev, uint8 flow)
{
    uint8 b = flow;

    (void)svcrt_dev_write_internal(dev, &b, 1);
}

static int cmd_cfg_show(void)
{
    const svcrt_cfg_slot_t *slots = svcrt_layout_slots();
    uint32 reason = svcrt_layout_reason();
    uint32 count  = svcrt_layout_slot_count();
    uint32 i;

    ark_shell_printf("\r\nlayout    : %s (source: %s)\r\n",
                     cfg_mode_text(svcrt_layout_mode()),
                     cfg_source_text(svcrt_layout_source()));
    ark_shell_printf("config    : 0x%08X +%uK, %u record(s) per sector\r\n",
                     (unsigned)CONFIG_BASE,
                     (unsigned)(CONFIG_SIZE / 1024u),
                     (unsigned)(CONFIG_SIZE / SVCRT_CFG_RECORD_SIZE));
    ark_shell_printf("reclaim   : %s\r\n", cfg_reclaim_text(svcrt_layout_reclaim_mode()));
    ark_shell_printf("knobs     : log %u (record %u), restart max %u\r\n",
                     svcrt_log_get_level(),
                     svcrt_layout_cfg_log_level(),
                     svcrt_layout_cfg_restart_max());
    ark_shell_printf("boot      : hold %u ms before autostart, raw images %s\r\n",
                     svcrt_layout_boot_delay_ms(),
                     (svcrt_layout_raw_allow() != 0u) ? "allowed" : "refused");

    if(reason != SVCRT_CFG_OK)
    {
        ark_shell_printf("record    : rejected, %s\r\n", svcrt_layout_reason_text(reason));
    }
    else if(svcrt_layout_source() == SVCRT_LAYOUT_SOURCE_CONFIG)
    {
        sh_out("record    : accepted\r\n");
    }
    else
    {
        sh_out("record    : none, compile-time default in use\r\n");
    }

    if(count == 0u)
    {
        return 0;
    }

    sh_out("\r\nslot  type  base        size     ram         auto\r\n");

    for(i = 0u; i < count; i++)
    {
        ark_shell_printf(" %-3u  %-4s  0x%08X  %-7u  0x%08X  %u\r\n",
                         (unsigned)i,
                         cfg_type_text(slots[i].type),
                         (unsigned)slots[i].base,
                         (unsigned)slots[i].size,
                         (unsigned)slots[i].ram_base,
                         (unsigned)slots[i].autostart);
    }

    return 0;
}

static int cmd_cfg_load(void)
{
    static uint8 rec[SVCRT_CFG_RECORD_SIZE];
    int32 dev;
    uint32 i;
    uint32 erased = 0u;

    dev = svcrt_shell_uart_open();

    /* Drop whatever the console still holds before the handshake starts.
     * The shell ends a command line on CR, so a CRLF terminator leaves the
     * LF behind; a stray byte left here would shift the whole record by one
     * and the record would only fail its magic check at the very end.
     * Draining before the ready line is printed (not after) keeps the
     * host's first chunk, which is sent once it sees that line, intact. */
    {
        uint8 sink[32];
        uint32 spins = 0u;

        while((spins < 64u) && (svcrt_dev_read_internal(dev, sink, (int32)sizeof(sink)) > 0))
        {
            spins++;
        }
    }

    ark_shell_printf("\r\ncfg load: ready, send %u bytes as %u chunks of %u\r\n",
                     (unsigned)SVCRT_CFG_RECORD_SIZE,
                     (unsigned)SVCRT_CFG_CHUNK_COUNT,
                     (unsigned)SVCRT_CFG_CHUNK_SIZE);
    sh_out("cfg load: one flow byte per chunk (0x06 = next, 0x15 = stop)\r\n");

    /* 收块阶段串口上的行编辑输入也一并被吃掉，这正是我们要的：
     * ARK_SHELL_LINE_SIZE 装不下 512 字节，命令行文本走不通。 */
    for(i = 0u; i < SVCRT_CFG_CHUNK_COUNT; i++)
    {
        uint8 *p = &rec[i * SVCRT_CFG_CHUNK_SIZE];
        uint32 reason;
        int32  rc;

        if(cfg_recv_exact(dev, p, SVCRT_CFG_CHUNK_SIZE, SVCRT_CFG_CHUNK_TIMEOUT) != 0)
        {
            sh_out("cfg load: timed out waiting for the record\r\n");
            cfg_send_flow(dev, SVCRT_CFG_FLOW_NAK);
            return 0;
        }

        /* 前 3 块只回 ACK：此时记录还不完整，校验没有意义 */
        if((i + 1u) < SVCRT_CFG_CHUNK_COUNT)
        {
            cfg_send_flow(dev, SVCRT_CFG_FLOW_ACK);
            continue;
        }

        reason = svcrt_layout_validate((const svcrt_cfg_record_t *)rec);

        if(reason != SVCRT_CFG_OK)
        {
            ark_shell_printf("cfg load: record rejected, %s\r\n",
                             svcrt_layout_reason_text(reason));
            cfg_send_flow(dev, SVCRT_CFG_FLOW_NAK);
            return 0;
        }

        rc = svcrt_layout_write(rec, (uint32)SVCRT_CFG_RECORD_SIZE, &erased);

        if(rc < 0)
        {
            ark_shell_printf("cfg load: write failed (%d), config region not usable\r\n",
                             (int)rc);
            cfg_send_flow(dev, SVCRT_CFG_FLOW_NAK);
            return 0;
        }

        ark_shell_printf("cfg load: ok, record %d%s\r\n",
                         (int)rc,
                         (erased != 0u) ? " (region erased first)" : "");
        sh_out("cfg load: applied immediately; it is also what the next boot reads\r\n");

        cfg_send_flow(dev, SVCRT_CFG_FLOW_ACK);

        /* 记录里写的是布局策略与运行期参数：分区几何、RAM 窗口和任务表都在
         * 下一次 svcrt_layout_init() 之后才完全一致（池扫描用的是启动时的模式），
         * 所以这里提示重启，而不是假装一切都已到位。 */
        sh_out("cfg load: reboot recommended before installing images\r\n");
    }

    return 0;
}

static int cmd_cfg_clear(void)
{
    int32 rc = svcrt_layout_erase();

    if(rc < 0)
    {
        ark_shell_printf("cfg clear: failed (%d)\r\n", (int)rc);
        return -1;
    }

    sh_out("cfg clear: config region erased, compile-time default layout in use\r\n");
    sh_out("cfg clear: reboot recommended before installing images\r\n");
    return 0;
}

static int cmd_cfg(int argc, char *argv[])
{
    if(argc <= 1)
    {
        return cmd_cfg_show();
    }

    if(strcmp(argv[1], "show") == 0)
    {
        return cmd_cfg_show();
    }
    if(strcmp(argv[1], "load") == 0)
    {
        return cmd_cfg_load();
    }
    if(strcmp(argv[1], "clear") == 0)
    {
        return cmd_cfg_clear();
    }

    sh_out("usage: cfg [show | load | clear]\r\n");
    return -1;
}

/* ============================================================
 * blk: raw access to the non-volatile backends
 *
 * This is the bring-up self-check for a storage backend. It is the only
 * place in the kernel that lets an operator poke raw offsets, and it is kept
 * deliberately small. `blk test` is the interesting one: it never reports
 * "ok" from the write call alone - it re-reads what the chip actually holds
 * after an erase and after a program, so a backend that answers successfully
 * while changing nothing cannot pass.
 * ============================================================ */
#define BLK_TEST_LEN_MAX   (256u)   /* scratch size, see g_blk_scratch   */
#define BLK_WR_BYTES_MAX   (32u)    /* hex bytes accepted by `blk wr`    */
#define BLK_DUMP_LEN_MAX   (4096u)  /* cap so one command cannot flood    */
#define BLK_DUMP_COLS      (16u)

static uint8 g_blk_scratch[BLK_TEST_LEN_MAX];

static int blk_nibble(char c, uint32 *out)
{
    if((c >= '0') && (c <= '9')) { *out = (uint32)(c - '0');      return 0; }
    if((c >= 'a') && (c <= 'f')) { *out = (uint32)(c - 'a' + 10); return 0; }
    if((c >= 'A') && (c <= 'F')) { *out = (uint32)(c - 'A' + 10); return 0; }
    return -1;
}

/* "A5 01 ff" / "A501FF" -> bytes. Returns -1 on odd length or a non-hex char. */
static int blk_hex_to_bytes(const char *s, uint8 *out, uint32 max, uint32 *out_len)
{
    uint32 n = 0u;

    if((s == 0) || (*s == '\0'))
    {
        return -1;
    }

    while(*s != '\0')
    {
        uint32 hi;
        uint32 lo;

        if(n >= max)
        {
            return -1;
        }

        if(blk_nibble(s[0], &hi) != 0)
        {
            return -1;
        }

        if(s[1] == '\0')
        {
            return -1;
        }

        if(blk_nibble(s[1], &lo) != 0)
        {
            return -1;
        }

        out[n] = (uint8)((hi << 4) | lo);
        n++;
        s += 2;
    }

    *out_len = n;
    return 0;
}

static int blk_usage(void)
{
    sh_out("\r\nusage:\r\n");
    sh_out("  blk                          list devices (brings each one up)\r\n");
    sh_out("  blk probe <name>             report capacity and erase unit\r\n");
    sh_out("  blk rd <name> <off> [len]    hex dump, default 64 bytes\r\n");
    sh_out("  blk wr <name> <off> <hex>    write bytes; target must be erased\r\n");
    sh_out("  blk erase <name> <off> <len> off and len must be erase-unit aligned\r\n");
    sh_out("  blk test <name> <off> [len]  erase + program + read back, len <= unit\r\n");
    return -1;
}

static int blk_list(void)
{
    uint32 n = svcrt_blk_count();
    uint32 i;

    if(n == 0u)
    {
        sh_out("\r\nno block device registered on this board\r\n");
        return -1;
    }

    sh_out("\r\n name  erase unit  capacity   state\r\n");

    for(i = 0u; i < n; i++)
    {
        const svcrt_blk_dev_t *dev = svcrt_blk_at(i);

        /* Listing doubles as a presence check: bring-up here is cheap and
         * reports the truth, whereas a stale "probably fine" flag would not. */
        if(svcrt_blk_init(dev) == 0)
        {
            ark_shell_printf(" %-5s %-11u %-10u ready\r\n",
                             dev->name, (unsigned)dev->erase_unit,
                             (unsigned)svcrt_blk_size(dev));
        }
        else
        {
            ark_shell_printf(" %-5s %-11u %-10s down\r\n",
                             dev->name, (unsigned)dev->erase_unit, "-");
        }
    }

    return 0;
}

static int blk_dump(const svcrt_blk_dev_t *dev, uint32 off, uint32 len)
{
    uint32 done = 0u;

    while(done < len)
    {
        uint32 chunk = len - done;
        uint32 j;

        if(chunk > BLK_DUMP_COLS)
        {
            chunk = BLK_DUMP_COLS;
        }

        if(svcrt_blk_read(dev, off + done, g_blk_scratch, chunk) != 0)
        {
            ark_shell_printf("\r\nread failed at offset 0x%X\r\n",
                             (unsigned)(off + done));
            return -1;
        }

        ark_shell_printf(" %06X:", (unsigned)(off + done));

        for(j = 0u; j < chunk; j++)
        {
            ark_shell_printf(" %02X", (unsigned)g_blk_scratch[j]);
        }

        for(j = chunk; j < BLK_DUMP_COLS; j++)
        {
            sh_out("   ");
        }

        sh_out("  ");

        for(j = 0u; j < chunk; j++)
        {
            uint32 c = (uint32)g_blk_scratch[j];

            /* printable range only; everything else shows as '.' so a dump
             * of erased (0xFF) or blank (0x00) flash stays readable */
            ark_shell_printf("%c", ((c >= 0x20u) && (c < 0x7Fu)) ? (char)c : '.');
        }

        sh_out("\r\n");
        done += chunk;
    }

    return 0;
}

/* Erase the unit containing off, check it reads back as 0xFF, program a
 * deterministic pattern over len bytes, then read it back and compare. */
static int blk_test(const svcrt_blk_dev_t *dev, uint32 off, uint32 len)
{
    uint32 unit = dev->erase_unit;
    uint32 base = off & ~(unit - 1u);
    uint8  got[BLK_DUMP_COLS];
    uint32 i;

    if(len == 0u)
    {
        sh_out("\r\nlen must be >= 1\r\n");
        return -1;
    }

    if(len > unit)
    {
        ark_shell_printf("\r\nlen must be <= erase unit (%u)\r\n", (unsigned)unit);
        return -1;
    }

    if(len > BLK_TEST_LEN_MAX)
    {
        return -1;
    }

    ark_shell_printf("\r\ntest %s at 0x%X, %u bytes (unit base 0x%X, unit %u)\r\n",
                     dev->name, (unsigned)off, (unsigned)len,
                     (unsigned)base, (unsigned)unit);

    if(svcrt_blk_erase(dev, base, unit) != 0)
    {
        sh_out(" erase   : FAILED (device refused or timed out)\r\n");
        return -1;
    }

    if(svcrt_blk_read(dev, base, g_blk_scratch, len) != 0)
    {
        sh_out(" erase   : FAILED (read back failed)\r\n");
        return -1;
    }

    for(i = 0u; i < len; i++)
    {
        if(g_blk_scratch[i] != 0xFFu)
        {
            ark_shell_printf(" erase   : FAILED at +%u, got 0x%02X, want 0xFF\r\n",
                             (unsigned)i, (unsigned)g_blk_scratch[i]);
            return -1;
        }
    }

    sh_out(" erase   : ok, all 0xFF\r\n");

    for(i = 0u; i < len; i++)
    {
        g_blk_scratch[i] = (uint8)((((base + i) * 31u) + 7u) & 0xFFu);
    }

    if(svcrt_blk_write(dev, base, g_blk_scratch, len) != 0)
    {
        sh_out(" program : FAILED (device refused or timed out)\r\n");
        return -1;
    }

    /* Compare in small chunks: a full-size shadow buffer would double the
     * static cost of this command for no extra evidence. */
    for(i = 0u; i < len; i += BLK_DUMP_COLS)
    {
        uint32 chunk = len - i;
        uint32 j;

        if(chunk > BLK_DUMP_COLS)
        {
            chunk = BLK_DUMP_COLS;
        }

        if(svcrt_blk_read(dev, base + i, got, chunk) != 0)
        {
            sh_out(" verify  : FAILED (read back failed)\r\n");
            return -1;
        }

        for(j = 0u; j < chunk; j++)
        {
            if(got[j] != g_blk_scratch[i + j])
            {
                ark_shell_printf(" verify  : FAILED at +%u, got 0x%02X, want 0x%02X\r\n",
                                 (unsigned)(i + j), (unsigned)got[j],
                                 (unsigned)g_blk_scratch[i + j]);
                return -1;
            }
        }
    }

    ark_shell_printf(" program : ok, %u bytes verified\r\n", (unsigned)len);

    return 0;
}

static int cmd_blk(int argc, char *argv[])
{
    const svcrt_blk_dev_t *dev;
    uint32 off = 0u;
    uint32 len = 0u;
    const char *sub = (argc >= 2) ? argv[1] : "list";

    if(strcmp(sub, "list") == 0)
    {
        return blk_list();
    }

    if(argc < 3)
    {
        return blk_usage();
    }

    dev = svcrt_blk_find(argv[2]);

    if(dev == 0)
    {
        ark_shell_printf("\r\nunknown block device: %s\r\n", argv[2]);
        (void)blk_list();
        return -1;
    }

    if(svcrt_blk_init(dev) != 0)
    {
        ark_shell_printf("\r\n%s: not available\r\n", argv[2]);
        return -1;
    }

    if(strcmp(sub, "probe") == 0)
    {
        ark_shell_printf("\r\n%s: %u bytes, erase unit %u\r\n",
                         dev->name, (unsigned)svcrt_blk_size(dev),
                         (unsigned)dev->erase_unit);
        return 0;
    }

    if(strcmp(sub, "rd") == 0)
    {
        if((argc < 4) || (parse_u32(argv[3], &off) != 0))
        {
            return blk_usage();
        }

        len = 64u;

        if((argc >= 5) && (parse_u32(argv[4], &len) != 0))
        {
            return blk_usage();
        }

        if((len == 0u) || (len > BLK_DUMP_LEN_MAX))
        {
            ark_shell_printf("\r\nlen must be 1..%u\r\n", (unsigned)BLK_DUMP_LEN_MAX);
            return -1;
        }

        return blk_dump(dev, off, len);
    }

    if(strcmp(sub, "wr") == 0)
    {
        uint32 n = 0u;

        if((argc < 5) || (parse_u32(argv[3], &off) != 0))
        {
            return blk_usage();
        }

        if(blk_hex_to_bytes(argv[4], g_blk_scratch, BLK_WR_BYTES_MAX, &n) != 0)
        {
            ark_shell_printf("\r\nbad hex string (even number of digits, max %u bytes)\r\n",
                             (unsigned)BLK_WR_BYTES_MAX);
            return -1;
        }

        if(svcrt_blk_write(dev, off, g_blk_scratch, n) != 0)
        {
            sh_out("\r\nwrite refused or failed (is the range erased?)\r\n");
            return -1;
        }

        ark_shell_printf("\r\nwrote %u bytes at 0x%X\r\n", (unsigned)n, (unsigned)off);

        return blk_dump(dev, off, n);
    }

    if(strcmp(sub, "erase") == 0)
    {
        if((argc < 5) || (parse_u32(argv[3], &off) != 0) ||
           (parse_u32(argv[4], &len) != 0))
        {
            return blk_usage();
        }

        if(svcrt_blk_erase(dev, off, len) != 0)
        {
            ark_shell_printf("\r\nerase refused or failed (off and len must be multiples of %u)\r\n",
                             (unsigned)dev->erase_unit);
            return -1;
        }

        ark_shell_printf("\r\nerased %u bytes at 0x%X\r\n",
                         (unsigned)len, (unsigned)off);
        return 0;
    }

    if(strcmp(sub, "test") == 0)
    {
        if((argc < 4) || (parse_u32(argv[3], &off) != 0))
        {
            return blk_usage();
        }

        len = 128u;

        if((argc >= 5) && (parse_u32(argv[4], &len) != 0))
        {
            return blk_usage();
        }

        return blk_test(dev, off, len);
    }

    return blk_usage();
}

/* ============================================================
 * fs: the littlefs volume, on top of a block device from `blk list`
 *
 * The file system goes through the same block layer the raw `blk` commands
 * use, so "where the storage is" is written down exactly once. The volume is
 * named, never an address: `fs mount nor0 0 4194304` means offset 0 of that
 * device, and nothing here knows what CPU address that ends up at.
 *
 * `fs test` is the one that matters. It writes a file, reads it back, drops
 * the mount, mounts again, reads it back a second time and only then removes
 * it. A write that answers "ok" without reaching the chip cannot pass, and
 * neither can a cache that never flushes - the remount throws both away.
 * ============================================================ */
#define FS_SCRATCH_LEN   (512u)   /* one file's worth for wr / rd / test */
#define FS_RD_DEFAULT    (64u)    /* bytes `fs rd` reads when not told    */
#define FS_DUMP_COLS     (16u)
#define FS_TEST_PATH     "/fstest.bin"

/* fs put: binary upload window. One chunk per handshake, because the console
 * FIFO is 128 bytes deep and a littlefs block program may have to erase a
 * sector first - a sender that ran ahead would lose bytes silently. */
#define FS_PUT_CHUNK      (256u)
#define FS_PUT_TIMEOUT_MS (5000u)
#define FS_PUT_ACK        (0x06u)
#define FS_PUT_NAK        (0x15u)

static uint8 g_fs_scratch[FS_SCRATCH_LEN];

static int fs_usage(void)
{
    sh_out("\r\nusage:\r\n");
    sh_out("  fs mount [dev off size]   mount a volume (default: the SVCRT_FS_* layout)\r\n");
    sh_out("  fs unmount                drop the mount\r\n");
    sh_out("  fs format [dev off size]  erase and create an empty volume (destructive)\r\n");
    sh_out("  fs info                   volume size and used bytes\r\n");
    sh_out("  fs ls [dir]               list a directory, default /\r\n");
    sh_out("  fs wr <path> <text...>    write text to a file (creates or truncates)\r\n");
    sh_out("  fs rd <path> [len]        read a file back and hex dump it\r\n");
    sh_out("  fs put <path> <len>       receive <len> raw bytes and write them to <path>\r\n");
    sh_out("  fs rm <path>              remove a file\r\n");
    sh_out("  fs test                   write / read / remount / read / remove\r\n");
    sh_out("  fs err                    littlefs error of the last failed call\r\n");
    return -1;
}

/* Every fs call that fails keeps what littlefs actually returned; print that
 * number instead of guessing a cause from the outside. */
static int fs_fail(const char *what)
{
    int32 e = svcrt_fs_last_error();

    ark_shell_printf("\r\n%s: FAILED (littlefs error %d: %s)\r\n",
                     what, (int)e, svcrt_fs_error_name(e));
    return -1;
}

static void fs_dump(const uint8 *p, uint32 len)
{
    uint32 off = 0u;

    while(off < len)
    {
        uint32 chunk = len - off;
        uint32 j;

        if(chunk > FS_DUMP_COLS)
        {
            chunk = FS_DUMP_COLS;
        }

        ark_shell_printf(" %04X:", (unsigned)off);

        for(j = 0u; j < chunk; j++)
        {
            ark_shell_printf(" %02X", (unsigned)p[off + j]);
        }

        for(j = chunk; j < FS_DUMP_COLS; j++)
        {
            sh_out("   ");
        }

        sh_out("  ");

        for(j = 0u; j < chunk; j++)
        {
            uint32 c = (uint32)p[off + j];

            ark_shell_printf("%c", ((c >= 0x20u) && (c < 0x7Fu)) ? (char)c : '.');
        }

        sh_out("\r\n");
        off += chunk;
    }
}

/* argv[from..argc-1] joined with single spaces -> g_fs_scratch */
static int fs_collect_text(int argc, char *argv[], int from, uint32 *out_len)
{
    uint32 n = 0u;
    int i;

    for(i = from; i < argc; i++)
    {
        const char *p = argv[i];

        if(i > from)
        {
            if(n >= FS_SCRATCH_LEN)
            {
                return -1;
            }

            g_fs_scratch[n++] = (uint8)' ';
        }

        while(*p != '\0')
        {
            if(n >= FS_SCRATCH_LEN)
            {
                return -1;
            }

            g_fs_scratch[n++] = (uint8)(*p);
            p++;
        }
    }

    *out_len = n;
    return 0;
}

/* `fs <sub>` / `fs <sub> dev off size` -> volume description. Anything else
 * is a usage error: half a volume description is never filled in with a
 * default, because that would mount at an offset the operator did not name. */
static int fs_take_volume(int argc, char *argv[], const char **dev,
                          uint32 *off, uint32 *size)
{
    *dev  = SVCRT_FS_DEV_NAME;
    *off  = SVCRT_FS_BASE;
    *size = SVCRT_FS_SIZE;

    if(argc == 2)
    {
        return 0;
    }

    if(argc == 5)
    {
        *dev = argv[2];

        if((parse_u32(argv[3], off) != 0) || (parse_u32(argv[4], size) != 0))
        {
            return -1;
        }

        return 0;
    }

    return -1;
}

static int fs_list_cb(const char *name, uint32 size, uint8 is_dir, void *arg)
{
    uint32 *count = (uint32 *)arg;

    ark_shell_printf("  %-20s %8u%s\r\n", name, (unsigned)size,
                     (is_dir != 0u) ? "  <dir>" : "");
    (*count)++;
    return 0;
}

static int fs_do_ls(const char *dir)
{
    uint32 count = 0u;

    if(svcrt_fs_list(dir, fs_list_cb, &count) != 0)
    {
        return fs_fail("ls");
    }

    ark_shell_printf("  %u entr%s\r\n", (unsigned)count,
                     (count == 1u) ? "y" : "ies");
    return 0;
}

static int fs_do_info(void)
{
    uint32 total = 0u;
    uint32 used  = 0u;

    if(!svcrt_fs_mounted())
    {
        sh_out("\r\nnot mounted\r\n");
        return -1;
    }

    if(svcrt_fs_stat(&total, &used) != 0)
    {
        return fs_fail("stat");
    }

    ark_shell_printf("\r\nmounted  : yes (%s, offset 0x%X, size %u)\r\n",
                     SVCRT_FS_DEV_NAME, (unsigned)SVCRT_FS_BASE,
                     (unsigned)SVCRT_FS_SIZE);
    ark_shell_printf("total    : %u bytes (%u KiB)\r\n",
                     (unsigned)total, (unsigned)(total / 1024u));
    ark_shell_printf("used     : %u bytes (%u KiB, %u%%)\r\n",
                     (unsigned)used, (unsigned)(used / 1024u),
                     (unsigned)((total != 0u) ? ((used * 100u) / total) : 0u));
    ark_shell_printf("free     : %u bytes\r\n",
                     (unsigned)((total >= used) ? (total - used) : 0u));
    return 0;
}

/* Read FS_SCRATCH_LEN bytes from path and compare with the written pattern.
 * The comparison target is recomputed, not taken from the buffer that the
 * read just overwrote. */
static int fs_read_verify(const char *path)
{
    uint32 got = 0u;
    uint32 i;

    if(svcrt_fs_read_file(path, g_fs_scratch, FS_SCRATCH_LEN, &got) != 0)
    {
        return fs_fail("read");
    }

    if(got != FS_SCRATCH_LEN)
    {
        ark_shell_printf("\r\nread: short file, got %u of %u bytes\r\n",
                         (unsigned)got, (unsigned)FS_SCRATCH_LEN);
        return -1;
    }

    for(i = 0u; i < got; i++)
    {
        uint8 want = (uint8)((i * 17u + 3u) & 0xFFu);

        if(g_fs_scratch[i] != want)
        {
            ark_shell_printf("\r\nread: MISMATCH at %u: got 0x%02X want 0x%02X\r\n",
                             (unsigned)i, (unsigned)g_fs_scratch[i], (unsigned)want);
            return -1;
        }
    }

    ark_shell_printf("read   : ok, %u bytes verified\r\n", (unsigned)got);
    return 0;
}

static int fs_do_test(void)
{
    uint32 i;

    if(!svcrt_fs_mounted())
    {
        sh_out("\r\nnot mounted: fs mount first\r\n");
        return -1;
    }

    for(i = 0u; i < FS_SCRATCH_LEN; i++)
    {
        g_fs_scratch[i] = (uint8)((i * 17u + 3u) & 0xFFu);
    }

    if(svcrt_fs_write_file(FS_TEST_PATH, g_fs_scratch, FS_SCRATCH_LEN) != 0)
    {
        return fs_fail("write");
    }

    ark_shell_printf("\r\nwrite  : ok, %u bytes -> %s\r\n",
                     (unsigned)FS_SCRATCH_LEN, FS_TEST_PATH);

    if(fs_read_verify(FS_TEST_PATH) != 0)
    {
        return -1;
    }

    /* The remount is the point: it discards every cache in the volume and
     * rebuilds the file system state from the chip alone. */
    if(svcrt_fs_unmount() != 0)
    {
        return fs_fail("unmount");
    }

    if(svcrt_fs_mount_default() != 0)
    {
        return fs_fail("remount");
    }

    sh_out("remount: ok\r\n");

    if(fs_read_verify(FS_TEST_PATH) != 0)
    {
        return -1;
    }

    if(svcrt_fs_remove(FS_TEST_PATH) != 0)
    {
        return fs_fail("remove");
    }

    sh_out("remove : ok\r\n");
    sh_out("fs test: PASS\r\n");
    return 0;
}

static int cmd_fs(int argc, char *argv[])
{
    const char *sub = (argc >= 2) ? argv[1] : "";

    if(strcmp(sub, "mount") == 0)
    {
        const char *dev = 0;
        uint32 off = 0u;
        uint32 size = 0u;

        if(fs_take_volume(argc, argv, &dev, &off, &size) != 0)
        {
            return fs_usage();
        }

        if(svcrt_fs_mount(dev, off, size) != 0)
        {
            return fs_fail("mount");
        }

        ark_shell_printf("\r\nmounted %s at 0x%X, %u bytes\r\n",
                         dev, (unsigned)off, (unsigned)size);
        return 0;
    }

    if(strcmp(sub, "unmount") == 0)
    {
        if(svcrt_fs_unmount() != 0)
        {
            return fs_fail("unmount");
        }

        sh_out("\r\nunmounted\r\n");
        return 0;
    }

    if(strcmp(sub, "format") == 0)
    {
        const char *dev = 0;
        uint32 off = 0u;
        uint32 size = 0u;

        if(fs_take_volume(argc, argv, &dev, &off, &size) != 0)
        {
            return fs_usage();
        }

        ark_shell_printf("\r\nformatting %s at 0x%X, %u bytes - every file on it is lost\r\n",
                         dev, (unsigned)off, (unsigned)size);

        if(svcrt_fs_format(dev, off, size) != 0)
        {
            return fs_fail("format");
        }

        sh_out("format : ok, volume is empty and mounted\r\n");
        return 0;
    }

    if(strcmp(sub, "info") == 0)
    {
        return fs_do_info();
    }

    if(strcmp(sub, "ls") == 0)
    {
        return fs_do_ls((argc >= 3) ? argv[2] : "/");
    }

    if(strcmp(sub, "wr") == 0)
    {
        uint32 len = 0u;
        uint32 i;

        if((argc < 4) || (fs_collect_text(argc, argv, 3, &len) != 0))
        {
            return fs_usage();
        }

        if(!svcrt_fs_mounted())
        {
            sh_out("\r\nnot mounted\r\n");
            return -1;
        }

        if(svcrt_fs_write_file(argv[2], g_fs_scratch, len) != 0)
        {
            return fs_fail("write");
        }

        ark_shell_printf("\r\nwrote %u bytes to %s:", (unsigned)len, argv[2]);

        for(i = 0u; i < len; i++)
        {
            ark_shell_printf(" %02X", (unsigned)g_fs_scratch[i]);
        }

        sh_out("\r\n");
        return 0;
    }

    if(strcmp(sub, "rd") == 0)
    {
        uint32 len = FS_RD_DEFAULT;
        uint32 got = 0u;

        if(argc < 3)
        {
            return fs_usage();
        }

        if((argc >= 4) && (parse_u32(argv[3], &len) != 0))
        {
            return fs_usage();
        }

        if((len == 0u) || (len > FS_SCRATCH_LEN))
        {
            ark_shell_printf("\r\nlen must be 1..%u\r\n", (unsigned)FS_SCRATCH_LEN);
            return -1;
        }

        if(!svcrt_fs_mounted())
        {
            sh_out("\r\nnot mounted\r\n");
            return -1;
        }

        if(svcrt_fs_read_file(argv[2], g_fs_scratch, len, &got) != 0)
        {
            return fs_fail("read");
        }

        ark_shell_printf("\r\n%s: %u bytes\r\n", argv[2], (unsigned)got);
        fs_dump(g_fs_scratch, got);
        return 0;
    }

    if(strcmp(sub, "put") == 0)
    {
        int32  dev;
        uint32 total = 0u;
        uint32 sent = 0u;
        int32  rc = 0;

        if((argc < 4) || (parse_u32(argv[3], &total) != 0) || (total == 0u))
        {
            return fs_usage();
        }

        if(!svcrt_fs_mounted())
        {
            sh_out("\r\nnot mounted\r\n");
            return -1;
        }

        if(svcrt_fs_open_write(argv[2]) != 0)
        {
            return fs_fail("open for write");
        }

        dev = svcrt_shell_uart_open();

        /* Same two precautions the cfg window takes, for the same reasons:
         * drop what the console still holds *before* announcing readiness
         * (the CRLF that ended this very command leaves an LF behind, and one
         * stray byte shifts the whole stream), and own the reads for the
         * duration - a 20 KiB image cannot travel as command-line text. */
        {
            uint8 sink[32];
            uint32 spins = 0u;

            while((spins < 64u) &&
                  (svcrt_dev_read_internal(dev, sink, (int32)sizeof(sink)) > 0))
            {
                spins++;
            }
        }

        ark_shell_printf("\r\nfs put: %s, %u bytes in %u-byte chunks\r\n",
                         argv[2], (unsigned)total, (unsigned)FS_PUT_CHUNK);
        sh_out("fs put: one flow byte per chunk (0x06 = next, 0x15 = stop)\r\n");

        while(sent < total)
        {
            uint32 want = total - sent;
            uint8  ack = FS_PUT_ACK;

            if(want > FS_PUT_CHUNK)
            {
                want = FS_PUT_CHUNK;
            }

            /* cfg_recv_exact() is just "wait for exactly len bytes"; nothing
             * about it is cfg specific. */
            if(cfg_recv_exact(dev, g_fs_scratch, want, FS_PUT_TIMEOUT_MS) != 0)
            {
                ark_shell_printf("fs put: timed out at %u of %u bytes\r\n",
                                 (unsigned)sent, (unsigned)total);
                rc = -1;
                break;
            }

            if(svcrt_fs_write_next(g_fs_scratch, want) != 0)
            {
                ark_shell_printf("fs put: write failed (littlefs error %d: %s)\r\n",
                                 (int)svcrt_fs_last_error(),
                                 svcrt_fs_error_name(svcrt_fs_last_error()));
                rc = -1;
                break;
            }

            sent += want;

            /* ACK once the bytes are on the chip rather than once they were
             * received: a block program can be an erase, and a sender that ran
             * ahead would fill the 128-byte console FIFO and lose bytes it
             * would never hear about. */
            (void)svcrt_dev_write_internal(dev, &ack, 1);
        }

        if(svcrt_fs_close_write() != 0)
        {
            ark_shell_printf("fs put: close failed (littlefs error %d: %s)\r\n",
                             (int)svcrt_fs_last_error(),
                             svcrt_fs_error_name(svcrt_fs_last_error()));
            rc = -1;
        }

        if(rc == 0)
        {
            ark_shell_printf("fs put: ok, %u bytes in %s\r\n",
                             (unsigned)sent, argv[2]);
            return 0;
        }

        {
            uint8 nak = FS_PUT_NAK;

            (void)svcrt_dev_write_internal(dev, &nak, 1);
        }

        sh_out("fs put: failed, the file may be incomplete - remove it before retrying\r\n");
        return -1;
    }

    if(strcmp(sub, "rm") == 0)
    {
        if((argc != 3) || !svcrt_fs_mounted())
        {
            return fs_usage();
        }

        if(svcrt_fs_remove(argv[2]) != 0)
        {
            return fs_fail("remove");
        }

        ark_shell_printf("\r\nremoved %s\r\n", argv[2]);
        return 0;
    }

    if(strcmp(sub, "test") == 0)
    {
        return fs_do_test();
    }

    if(strcmp(sub, "err") == 0)
    {
        int32 e = svcrt_fs_last_error();

        ark_shell_printf("\r\nlast littlefs error: %d (%s)\r\n",
                         (int)e, svcrt_fs_error_name(e));
        return 0;
    }

    return fs_usage();
}


static int register_kernel_commands(void)
{
    int idx = g_cmd_count;
    int need = 17;

    if((idx + need) > ARK_SHELL_MAX_COMMANDS)
    {
        return -1;
    }

    g_cmd_table[idx++] = ARK_SHELL_CMD("info", cmd_info,
        "Kernel, partition and task capacity info", 1);
    g_cmd_table[idx++] = ARK_SHELL_CMD("app", cmd_app,
        "App slots: app [list | install <path> [slot] | start <slot> | stop <slot> | uninstall <slot>]", 4);
    g_cmd_table[idx++] = ARK_SHELL_CMD("drv", cmd_drv,
        "Driver slots: drv [list | start <slot> | stop <slot> | uninstall <slot>]", 4);
    g_cmd_table[idx++] = ARK_SHELL_CMD("task", cmd_task,
        "List kernel tasks, or dump one task frame: task <id>", 3);
    g_cmd_table[idx++] = ARK_SHELL_CMD("sched", cmd_sched,
        "Scheduler readiness self-check (0 = consistent)", 1);
    g_cmd_table[idx++] = ARK_SHELL_CMD("fault", cmd_fault,
        "Show recorded faults", 1);
    g_cmd_table[idx++] = ARK_SHELL_CMD("install", cmd_install,
        "Open a one-shot install window on the console UART: install [slot]", 2);

    g_cmd_table[idx++] = ARK_SHELL_CMD("log", cmd_log,
        "Get or set runtime log level: log [0..4]", 2);
    g_cmd_table[idx++] = ARK_SHELL_CMD("syncinfo", cmd_syncinfo,
        "Dump semaphore / mutex / condition tables with their waiters", 1);
    g_cmd_table[idx++] = ARK_SHELL_CMD("pool", cmd_pool,
        "Free space left in the image pool", 1);
    g_cmd_table[idx++] = ARK_SHELL_CMD("trace", svcrt_trace_shell_cmd,
        "Kernel event trace: trace [start | stop | reset | dump | mark <n>]", 3);
    g_cmd_table[idx++] = ARK_SHELL_CMD("cfg", cmd_cfg,
        "Device layout config: cfg [show | load | clear]", 2);
    g_cmd_table[idx++] = ARK_SHELL_CMD("blk", cmd_blk,
        "Block devices: blk [list | probe | rd | wr | erase | test]", 5);

    g_cmd_table[idx++] = ARK_SHELL_CMD("fs", cmd_fs,
        "File system: fs [mount | unmount | format | info | ls | wr | rd | put | rm | test | err]", 9);

    g_cmd_table[idx++] = ARK_SHELL_CMD("audit", cmd_audit,
        "Structure self-audit: partition / slots / tasks vs their invariants", 1);

    g_cmd_table[idx++] = ARK_SHELL_CMD("guard", cmd_guard,
        "Watchdog state and per-slot heartbeat contracts", 1);

    g_cmd_table[idx++] = ARK_SHELL_CMD("crash", cmd_crash,
        "Cross-reset crash journal: boots, per-slot fault counts and held slots", 1);

    g_cmd_table[idx].name = NULL;
    g_cmd_table[idx].func = NULL;
    g_cmd_table[idx].help = NULL;
    g_cmd_table[idx].max_args = 0;
    g_cmd_count = idx;
    return 0;
}

/* ============================================================
 * 控制台任务
 * ============================================================ */
static void svcrt_shell_task(void)
{
    /* ark_shell_init 内部会调用 platform_uart_init()，即打开控制台串口 */
    ark_shell_init(&g_shell, (uint32_t)SHELL_DEV_ARG);

    if(register_kernel_commands() != 0)
    {
        sh_out("shell: command table full, kernel commands unavailable\r\n");
    }

    sh_out("SVCrtOS console ready. Type 'help'.\r\n");

    for(;;)
    {
        uint32 budget = (uint32)SHELL_RX_DRAIN_MAX;
        int    ch;

        /* Drain a bounded burst per round instead of a single byte.  One
         * byte per 2 ms caps the console at 500 B/s while the host pushes
         * a whole line in well under a millisecond: the receive FIFO fills
         * up, svcrt_fifo_write() refuses the tail - the CR included - the
         * command never terminates and the console looks dead even though
         * the task is still scheduled and still transmitting.  Bounded, so
         * a human typing can never keep the task off the CPU. */
        while(budget-- > 0u)
        {
            ch = platform_uart_recv();
            if(ch < 0)
            {
                break;
            }
            ark_shell_process_byte(&g_shell, (unsigned char)ch);
        }

        /* A full line buffer means the byte stream was cut somewhere (RX
         * bytes dropped, or a host that never sent CR): such a line can
         * never be recognised, and every later byte only piles onto the
         * wreckage.  Drop the fragment and say so, instead of swallowing
         * every command from then on. */
        if(g_shell.editor.len >= (ARK_SHELL_LINE_SIZE - 1))
        {
            line_editor_clear(&g_shell.editor);
            g_shell.state = STATE_NORMAL;
            sh_out("\r\nconsole: line too long, discarded\r\n");
        }

        /* 让出 CPU：控制台优先级最低，不能让交互拖住业务任务 */
        svcrt_task_wait_internal((uint32)SHELL_TASK_PERIOD_MS);
    }
}

int32 svcrt_shell_init(void)
{
    return svcrt_task_register(svcrt_shell_task,
                               g_shell_stack,
                               (uint32)sizeof(g_shell_stack),
                               (uint8)SHELL_TASK_PRIORITY,
                               (uint32)SHELL_TASK_PERIOD_MS);
}

/**
* @brief Drop every user command registered by one task.
* @param task_id owner task id; 0 means "kernel", which owns nothing to drop
* @return number of entries released
* @details Called when a task is torn down (App stop / uninstall / overwrite,
*          fault kill, thread exit). Without this the name stays reserved
*          forever and a later image placed over the same address range would
*          pass the liveness check while pointing at a different function.
*/
int32 svcrt_shell_release_task(uint32 task_id)
{
    uint32 i;
    int32  dropped = 0;

    if(task_id == 0u)
    {
        return 0;
    }

    for(i = 0u; i < SVCRT_USHELL_MAX_CMDS; i++)
    {
        if((g_ushell[i].in_use == 0u) || (g_ushell[i].owner_task != task_id))
        {
            continue;
        }

        ushell_drop(i);
        dropped++;
    }

    return dropped;
}

#else   /* SHELL_ENABLE == 0 */

int32 svcrt_shell_ext_register(const svcrt_ushell_cmd_t *cmd)
{
    (void)cmd;
    return SVCRT_USHELL_ERR_OFF;
}

int32 svcrt_shell_ext_unregister(const char *name)
{
    (void)name;
    return SVCRT_USHELL_ERR_OFF;
}

int32 svcrt_shell_release_task(uint32 task_id)
{
    (void)task_id;
    return 0;       /* nothing was ever registered: the shell is compiled out */
}


int32 svcrt_shell_print_n(const char *msg, uint32 len)
{
    (void)msg;
    (void)len;
    return SVCRT_USHELL_ERR_OFF;
}


int32 svcrt_shell_init(void)
{
    return 0;
}

#endif
