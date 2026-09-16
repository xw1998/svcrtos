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
#include "svcrt_share.h"
#include "svcrt_task.h"
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
        default:                        return "?";
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
static int cmd_info(int argc, char *argv[])
{
    svcrt_partition_table_t *pt = svcrt_ptable_get();

    (void)argc;
    (void)argv;

    ark_shell_printf("\r\nSVCrtOS kernel shell\r\n");
    ark_shell_printf("build     : " __DATE__ " " __TIME__ "\r\n");
    ark_shell_printf("partition : ABI v%u, hw 0x%08X\r\n",
                     (uint32)pt->version, pt->hw_compat_id);
    ark_shell_printf("kernel    : flash 0x%08X +%uK, ram 0x%08X +%uK\r\n",
                     pt->kernel_base, pt->kernel_size / 1024u,
                     pt->kernel_ram_base, pt->kernel_ram_size / 1024u);
    ark_shell_printf("driver    : %u slot x %uK @ 0x%08X, ram %uK/slot\r\n",
                     pt->driver_max_count, pt->driver_slot_size / 1024u,
                     pt->driver_pool_base, pt->driver_slot_ram_size / 1024u);
    ark_shell_printf("app       : %u slot x %uK @ 0x%08X, ram %uK/slot\r\n",
                     pt->app_max_count, pt->app_slot_size / 1024u,
                     pt->app_user_base, pt->app_slot_ram_size / 1024u);
    ark_shell_printf("tasks     : %d used / %u max\r\n",
                     (int)svcrt_task_count, (uint32)SVCRT_TASK_MAX_NUM);
    ark_shell_printf("tick      : %u ms, period %u us\r\n",
                     svcrt_kernel_get_time(), (uint32)SVCRT_TICK_PERIOD_US);
    return 0;
}

/* ============================================================
 * app：App 槽位列表 / 启动 / 停止
 * ============================================================ */
static int cmd_app(int argc, char *argv[])
{
    svcrt_partition_table_t *pt = svcrt_ptable_get();
    uint32 i;
    uint32 slot;

    if((argc >= 2) && (strcmp(argv[1], "start") == 0))
    {
        if((argc < 3) || (parse_u32(argv[2], &slot) != 0) || (slot >= pt->app_max_count))
        {
            sh_out("usage: app start <slot>\r\n");
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
        if((argc < 3) || (parse_u32(argv[2], &slot) != 0) || (slot >= pt->app_max_count))
        {
            sh_out("usage: app stop <slot>\r\n");
            return -1;
        }

        if(svcrt_loader_stop(slot) == 0)
        {
            sh_out("app: stopped, slot released\r\n");
        }
        else
        {
            sh_out("app: stop failed (slot must be LOADED/RUNNING)\r\n");
        }
        return 0;
    }

    if((argc >= 2) && (strcmp(argv[1], "list") != 0))
    {
        sh_out("usage: app [list | start <slot> | stop <slot>]\r\n");
        return -1;
    }

    ark_shell_printf("\r\nslot  state     auto  task  crash  entry\r\n");
    for(i = 0u; i < pt->app_max_count; i++)
    {
        ark_shell_printf(" %u    %-8s  %-4s  %-4u  %-5u  0x%08X\r\n",
                         i,
                         slot_state_name(pt->slot_state[i]),
                         (pt->slot_autostart[i] != 0u) ? "yes" : "no",
                         pt->slot_task_id[i],
                         pt->slot_crash_cnt[i],
                         pt->slot_entry[i]);
    }
    return 0;
}

/* ============================================================
 * drv：驱动槽位列表 / 启动 / 停止
 * ============================================================ */
static int cmd_drv(int argc, char *argv[])
{
    svcrt_partition_table_t *pt = svcrt_ptable_get();
    uint32 i;
    uint32 slot;

    if((argc >= 2) && (strcmp(argv[1], "start") == 0))
    {
        if((argc < 3) || (parse_u32(argv[2], &slot) != 0) || (slot >= pt->driver_max_count))
        {
            sh_out("usage: drv start <slot>\r\n");
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
        if((argc < 3) || (parse_u32(argv[2], &slot) != 0) || (slot >= pt->driver_max_count))
        {
            sh_out("usage: drv stop <slot>\r\n");
            return -1;
        }

        if(svcrt_loader_stop_driver_slot(slot) == 0)
        {
            sh_out("drv: stopped, slot released\r\n");
        }
        else
        {
            sh_out("drv: stop failed (slot must be LOADED/RUNNING)\r\n");
        }
        return 0;
    }

    if((argc >= 2) && (strcmp(argv[1], "list") != 0))
    {
        sh_out("usage: drv [list | start <slot> | stop <slot>]\r\n");
        return -1;
    }

    ark_shell_printf("\r\nslot  state     auto  task  crash  entry\r\n");
    for(i = 0u; i < pt->driver_max_count; i++)
    {
        ark_shell_printf(" %u    %-8s  %-4s  %-4u  %-5u  0x%08X\r\n",
                         i,
                         slot_state_name(pt->driver_slot_state[i]),
                         (pt->driver_slot_autostart[i] != 0u) ? "yes" : "no",
                         pt->driver_slot_task_id[i],
                         pt->driver_slot_crash_cnt[i],
                         pt->driver_slot_entry[i]);
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
static int register_kernel_commands(void)
{
    int idx = g_cmd_count;
    int need = 6;

    if((idx + need) > ARK_SHELL_MAX_COMMANDS)
    {
        return -1;
    }

    g_cmd_table[idx++] = ARK_SHELL_CMD("info", cmd_info,
        "Kernel, partition and task capacity info", 1);
    g_cmd_table[idx++] = ARK_SHELL_CMD("app", cmd_app,
        "App slots: app [list | start <slot> | stop <slot>]", 3);
    g_cmd_table[idx++] = ARK_SHELL_CMD("drv", cmd_drv,
        "Driver slots: drv [list | start <slot> | stop <slot>]", 3);
    g_cmd_table[idx++] = ARK_SHELL_CMD("task", cmd_task,
        "List kernel tasks", 1);
    g_cmd_table[idx++] = ARK_SHELL_CMD("fault", cmd_fault,
        "Show recorded faults", 1);
    g_cmd_table[idx++] = ARK_SHELL_CMD("install", cmd_install,
        "Open a one-shot install window on the console UART", 1);

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

int32 svcrt_shell_init(void)
{
    return 0;
}

#endif
