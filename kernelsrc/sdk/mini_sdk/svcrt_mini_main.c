/**
* @brief SVCrtOS 小程序 SDK - 入口骨架
* @details 小程序由内核的装载器从文件系统读进 RAM 执行（见
*          docs/小程序设计.md），因此它：
*            - 不需要关心自己被放在哪里：镜像里的地址由重定位表在装载时改好；
*            - 没有固定的分区槽位，退出后内核立刻归还代码块与 RAM 块。
*
*          MiniMain() 返回即视为「程序结束」：这里调用 svcrt_thread_exit()
*          把自己交给内核回收，之后再被调度到也不会继续跑。
*/

#include "svcrt.h"

SVCRT_WEAK void MiniMain(void)
{
}

int main(void)
{
    MiniMain();

    /* 一次性程序：跑完就退。内核在任务退出钩子里归还两块 RAM，
     * 下一次 "mini run <path>" 会重新从文件系统装载。 */
    svcrt_thread_exit();

    /* svcrt_thread_exit() 不会再返回；这一句只是防御性的兜底，
     * 保证就算退出路径出了意外也不会跑到别的代码上。 */
    for(;;)
    {
    }
}
