/**
* @file svcrt_mpu.c
* @brief SVCrtOS 内存保护单元管理（内核侧唯一入口）
* @details 内核只调用本文件接口，MPU 寄存器操作全部在 port 层
*          （svcrt_port_mpu_*），移植到新架构只需实现 port 层。
*          板级配置 SVCRT_USE_MPU=0 时本文件整体不参与编译。
*
* @warning 【未完成：打开开关之前必须先补齐下面 4 项】
*          当前板级配置是 SVCRT_USE_MPU=0，本文件整段不参与编译，
*          也就是说下面这些空白**没有被任何一次构建覆盖过**。
*
*          1) 任务 MPU 上下文无人填充。svcrt_task_register() 只把 TCB.mpu
*             清零（区域全 0 = 全部禁用），而上下文切换处
*             （svcrt_task.c 的 svcrt_mpu_set_app 调用）会把这份空上下文
*             原样写进硬件 —— 非特权任务访问任何地址都会触发 MemManage。
*             启用前需要在这里补一个 build 函数，按“代码区 + 数据区 +
*             外设区放行”生成区域表，并在 svcrt_task_register() 与
*             loader 装载任务处调用。
*
*          2) 数据区不能直接用 TCB.ram_start/ram_size。那两项记录的是任务栈
*             （栈底指针 + 字节数），既不等于任务的全局变量区，大小也不是
*             2 的幂、基址也未必对齐，而 MPU 区域要求二者都满足。
*             正确来源是分区配置：App 的 RW/ZI 由 .sct 放在 APP_RAM_BASE
*             起的整个 APP_RAM 窗口里，驱动同理放在 DRIVER_RAM。
*
*          3) 外设区（0x40000000 起）需要单列策略。非特权任务要访问外设
*             寄存器必须显式放行，放行范围与权限（只读/读写）按板子确定，
*             不能沿用内核的 PRIVDEFENA。
*
*          4) 以上三点都必须上板标定：MemManage 的实际触发点、区域边界、
*             外设放行范围，静态检查一个都证明不了。
*
*          因此：保持默认关闭。补齐并通过上板验证之前，不要打开
*          SVCRT_USE_MPU。
*
* @author xw
* @date 2026.09.12
*/

#include "svcrt_mpu.h"

#if (SVCRT_USE_MPU == 1)

void svcrt_mpu_module_init(void)
{
    svcrt_port_mpu_init();
}

void svcrt_mpu_set_app(svcrt_task_t *p_app)
{
    if(p_app == 0)
    {
        /* 切到空闲/无任务上下文时不改动 MPU 配置 */
        return;
    }

    svcrt_port_mpu_set_app(&p_app->mpu);
}

void svcrt_mpu_reset(void)
{
    svcrt_port_mpu_reset();
}

#endif
