/**
* @brief SVCrtOS App SDK - 越权探针镜像（内存保护的反向验证）
* @details 这不是示例，是**故意违规**的镜像：它和普通 App 一样以非特权态任务
*          运行，但启动后立刻做两件被 MPU 禁止的事，用来确认三件事——
*            1. 保护真的拦得住（而不是只在文档里写着）
*            2. 拦下来之后故障被如实记录（shell 的故障环 / 崩溃日志）
*            3. 整机不受影响（内核、其它槽、shell 都还活着）
*
*          probe 1: 写内核私有 RAM 里的一个字（不在本 App 的 MPU 窗口内）
*                   期望 MemManage（DACCVIOL）
*          probe 2: 非特权态直接写系统控制块（SCB->SHCSR）
*                   期望 HardFault（特权寄存器不可写）
*
*          探针只跑一次就转入长睡：越权会杀掉本任务，内核按终局策略记账
*          （crash 计数 +1；同一槽连续 3 次则整槽 INVALID + held）。
*          本镜像**故意不带心跳**，所以「整机还活着」不能在它自己的输出里
*          看，要在 shell 侧看：`fault` 有记录、`app list` 有计数、shell 本身
*          敲得动。
*
*          地址是写死的：App 侧不许 include 分区头（布局只能经 SVC 0x18
*          取得），而这是一个只用来打一下的探针，不是可移植代码。
*/

#include "svcrt.h"

/* 内核私有 RAM（chip RAM 里 share(8K) 之后、内核自己用的一段）：
 * 它明确不在本 App 的 RAM 窗口里，也不在共享窗口里。 */
#define APP_BAD_KERNEL_RAM      ((volatile uint32 *)0x20003000u)

/* SCB->SHCSR：只有特权态能写。 */
#define APP_BAD_SCB_SHCSR       ((volatile uint32 *)0xE000ED24u)

static void bad_puts(int32 con, const char *s)
{
    int32 len = 0;

    while(s[len] != '\0')
    {
        len++;
    }
    if((con >= 0) && (len > 0))
    {
        (void)svcrt_dev_write(con, (void *)s, len);
    }
}

void AppMain(void)
{
    int32 con;
    int32 n;

    con = svcrt_dev_open("COM1", 0);

    /* 分阶段探，用内核的故障累计数当「走到哪一步了」的标记：越权会杀掉本任务，
     * 重启后这个计数是跨重启保留的，而 App 自己没有任何可持久化的地方。
     * 这样做是为了让两次故障各自留下现场：若一路撞下去，第三次就把整槽禁掉，
     * 而 fault 环又会在随后的复位里被清空 —— 什么都看不到。 */
    n = svcrt_fault_record_count();

    if(n <= 0)
    {
        bad_puts(con, "\r\nAPP_BAD: privilege violation probe (stage 1/2)\r\n");
        bad_puts(con, "APP_BAD: probe1 = write kernel RAM 0x20003000 (expect MemManage)\r\n");

        /* probe 1：越权写内核 RAM。这一句不会返回。 */
        *APP_BAD_KERNEL_RAM = 0xA5A5A5A5u;

        bad_puts(con, "APP_BAD: probe1 NOT blocked - kernel RAM is writable\r\n");
    }

    /* 一条越权在故障环里会留下两条记录（MEMFAULT + 它引发的 RECOVER），
     * 所以「stage 1 已经跑过」对应的是 n >= 2，不是 n >= 1。 */
    if(n <= 2)
    {
        bad_puts(con, "APP_BAD: privilege violation probe (stage 2/2)\r\n");
        bad_puts(con, "APP_BAD: probe2 = write SCB->SHCSR (expect HardFault)\r\n");

        /* probe 2：非特权态写系统控制块。同样不会返回。 */
        *APP_BAD_SCB_SHCSR = 0u;

        bad_puts(con, "APP_BAD: probe2 NOT blocked - privileged register is writable\r\n");
    }

    bad_puts(con, "APP_BAD: both probes done, idle\r\n");

    while(1)
    {
        (void)svcrt_task_wait(1000u);
    }
}
