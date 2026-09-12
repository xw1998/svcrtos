/**
* @file svcrt_mpu.c
* @brief SVCrtOS 内存保护单元管理（内核侧唯一入口）
* @details 内核只调用本文件接口，MPU 寄存器操作全部在 port 层
*          （svcrt_port_mpu_*），移植到新架构只需实现 port 层。
*          板级配置 SVCRT_USE_MPU=0 时本文件整体不参与编译。
*
* @warning 打开隔离（SVCRT_USE_MPU=1）之前，必须为每个任务填充
*          TCB 里的 MPU 区域上下文（svcrt_task_t.mpu，见 svcrt_task.h）：
*          当前 svcrt_task_register() 只把它清零，区域全 0 = 全部禁用，
*          非特权任务访问任何地址都会触发 MemManage。
*          需要连同"任务代码区 / 数据区 / 是否放行外设区"的权限策略一起确定。
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
