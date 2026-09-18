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
*            info / app / drv / task / fault / install
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
#include "svcrt_ptable.h"
#include "svcrt_log.h"
#include "svcrt_share.h"
#include "svcrt_task.h"
#include "svcrt_trace.h"
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

        if(svcrt_loader_start(slot) > 0)
        {
            sh_out("app: started\r\n");
        }
        else
        {
            sh_out("app: start failed (check state with 'app list', reason in 'fault')\r\n");
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

    if((argc >= 2) && (strcmp(argv[1], "list") != 0))
    {
        sh_out("usage: app [list | start <slot> | stop <slot> | uninstall <slot>]\r\n");
        return -1;
    }

    /* 落点是运行期分配出来的，所以列出来的是真实地址与真实占用字节数，
     * 而不是链接期的固定槽位。 */
    ark_shell_printf("\r\nid  type  state     auto  task  crash  base        size    ram         entry\r\n");
    for(i = 0u; (i < pt->slot_max) && (i < SVCRT_SLOT_ARRAY_MAX); i++)
    {
        if(pt->slot_type[i] != SVCRT_SLOT_APP)
        {
            continue;
        }

        ark_shell_printf(" %u   %-4s  %-8s  %-4s  %-4u  %-5u  0x%08X  %-6u  0x%08X  0x%08X\r\n",
                         i,
                         slot_type_name(pt->slot_type[i]),
                         slot_state_name(pt->slot_state[i]),
                         (pt->slot_autostart[i] != 0u) ? "yes" : "no",
                         pt->slot_task_id[i],
                         pt->slot_crash_cnt[i],
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

        if(svcrt_loader_start_driver_slot(slot) > 0)
        {
            sh_out("drv: started\r\n");
        }
        else
        {
            sh_out("drv: start failed (check state with 'drv list', reason in 'fault')\r\n");
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

    ark_shell_printf("\r\nid  type  state     auto  task  crash  base        size    ram         entry\r\n");
    for(i = 0u; (i < pt->slot_max) && (i < SVCRT_SLOT_ARRAY_MAX); i++)
    {
        if(pt->slot_type[i] != SVCRT_SLOT_DRIVER)
        {
            continue;
        }

        ark_shell_printf(" %u   %-4s  %-8s  %-4s  %-4u  %-5u  0x%08X  %-6u  0x%08X  0x%08X\r\n",
                         i,
                         slot_type_name(pt->slot_type[i]),
                         slot_state_name(pt->slot_state[i]),
                         (pt->slot_autostart[i] != 0u) ? "yes" : "no",
                         pt->slot_task_id[i],
                         pt->slot_crash_cnt[i],
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

    (void)argc;
    (void)argv;

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

    (void)argc;
    (void)argv;

    sh_out("\r\ninstall: waiting for one .svcapp image on " SHELL_DEV_NAME "\r\n");
    sh_out("install: send the file now; console input is ignored while waiting\r\n");

    slot = svcrt_installer_run_once(svcrt_shell_uart_open(),
                                    (uint32)SHELL_INSTALL_TIMEOUT_MS);

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
    uint32 k;

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
        g_ushell[i].name[0] = '\0';
        g_ushell[i].help[0] = '\0';
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

    return 0;
}

static int register_kernel_commands(void)
{
    int idx = g_cmd_count;
    int need = 9;

    if((idx + need) > ARK_SHELL_MAX_COMMANDS)
    {
        return -1;
    }

    g_cmd_table[idx++] = ARK_SHELL_CMD("info", cmd_info,
        "Kernel, partition and task capacity info", 1);
    g_cmd_table[idx++] = ARK_SHELL_CMD("app", cmd_app,
        "App slots: app [list | start <slot> | stop <slot> | uninstall <slot>]", 4);
    g_cmd_table[idx++] = ARK_SHELL_CMD("drv", cmd_drv,
        "Driver slots: drv [list | start <slot> | stop <slot> | uninstall <slot>]", 4);
    g_cmd_table[idx++] = ARK_SHELL_CMD("task", cmd_task,
        "List kernel tasks", 3);
    g_cmd_table[idx++] = ARK_SHELL_CMD("fault", cmd_fault,
        "Show recorded faults", 1);
    g_cmd_table[idx++] = ARK_SHELL_CMD("install", cmd_install,
        "Open a one-shot install window on the console UART", 1);

    g_cmd_table[idx++] = ARK_SHELL_CMD("log", cmd_log,
        "Get or set runtime log level: log [0..4]", 2);
    g_cmd_table[idx++] = ARK_SHELL_CMD("pool", cmd_pool,
        "Free space left in the image pool", 1);
    g_cmd_table[idx++] = ARK_SHELL_CMD("trace", svcrt_trace_shell_cmd,
        "Kernel event trace: trace [start | stop | reset | dump | mark <n>]", 3);

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
        /* 非阻塞：一次最多处理一个字节，没数据立刻返回 */
        ark_shell_run(&g_shell);

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
