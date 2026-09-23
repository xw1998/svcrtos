/**
* @brief SVCrtOS 小程序示例 - 常驻文件系统、装载进 RAM 执行
* @details 这个程序演示「小程序」这条路：
*            1. 它以普通 .svcapp 文件的形式躺在文件系统里，不占分区、不占槽位；
*            2. "mini run /mini/mini_demo.app" 时内核把它读进 RAM 执行；
*            3. MiniMain() 返回后内核立刻归还代码块与 RAM 块，同一份文件
*               可以反复 run，每次都是新装载。
*
*          为了把「代码块也被回收了」这件事做实，本示例跑完就退出，
*          而不是像 App 那样常驻心跳循环。
*/

#include "svcrt.h"

/* ---- 控制台小工具 ---------------------------------------------------- */

static int32 g_con = -1;

/* 一次 svcrt_dev_write 是一次完整的 SVC 往返，所以整串一次写完，
 * 不要按字节写。 */
static void put(const char *s)
{
    int32 len = 0;

    while(s[len] != '\0')
    {
        len++;
    }
    if(len > 0)
    {
        (void)svcrt_dev_write(g_con, (void *)s, len);
    }
}

static void put_u32(uint32 v)
{
    char buf[12];
    int32 i = 12;

    buf[--i] = '\0';
    do
    {
        buf[--i] = (char)('0' + (v % 10u));
        v /= 10u;
    } while(v != 0u);

    put(&buf[i]);
}

/* ---- 程序主体 -------------------------------------------------------- */

void MiniMain(void)
{
    uint32 i;

    g_con = svcrt_dev_open("COM1", 115200);

    put("\r\n[mini] hello from a program that lives in the filesystem\r\n");
    put("[mini] loaded at run time, tick = ");
    put_u32(svcrt_get_time_ms());
    put(" ms\r\n");

    for(i = 0u; i < 3u; i++)
    {
        put("[mini] loop ");
        put_u32(i);
        put("\r\n");
        svcrt_task_wait(300);
    }

    /* 返回 = 结束。内核在小程序退出钩子里归还代码块与 RAM 块，
     * "mini stat" 之后应该看到它已经不在了。 */
    put("[mini] done, returning to release the two RAM blocks\r\n");
}
