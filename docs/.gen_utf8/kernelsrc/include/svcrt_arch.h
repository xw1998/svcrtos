/**
* @brief SVCrtOS 架构抽象层 - 架构族 / 核心描述与派生能力宏
* @details 本文件是内核与具体指令集之间的第一层抽象：
*          1) 用「架构族 + 具体核心」两级描述目标 CPU，取代单一 SVCRT_CPU_ARCH 数值
*          2) 由核心派生出能力宏（FPU / MPU / 特权级 / SVC 号位宽等），
*             内核据此决定功能开关与数据结构，而非直接判断芯片型号
*          3) 定义架构相关的公共数据类型（MPU 上下文）
*
*          【新增架构的步骤】
*          1. 在本文件增加架构族与核心宏，并补全能力派生分支
*          2. 在 kernelsrc/port/<族>/<核心>/ 下实现 svcrt_hal.h 声明的全部接口
*          3. 上下文切换汇编、系统调用号提取、MPU 写入是三个架构强相关点
*          详见 kernelsrc/port/README.md
*
* @note 本文件不得包含任何芯片头文件，不得直接操作寄存器。
*/

#ifndef __SVCRT_ARCH_H__
#define __SVCRT_ARCH_H__

#include "svcrt_types.h"

/* ============================================================
 * 架构族
 * @brief 指令集族，决定系统调用机制、特权模型、内存保护的总体形态
 * ============================================================ */
#define SVCRT_ARCH_FAMILY_ARM        (1)    /* ARM Cortex-M 系列          */
#define SVCRT_ARCH_FAMILY_RISCV      (2)    /* RISC-V（RV32/RV64）        */
#define SVCRT_ARCH_FAMILY_LOONGARCH  (3)    /* 龙芯 LoongArch（LA32/LA64）*/

/* ============================================================
 * 具体核心
 * @brief 编码规则：高位字节为架构族，低位标识具体核心
 * ============================================================ */
#define SVCRT_CPU_CORE_CORTEX_M3     (0x0101)
#define SVCRT_CPU_CORE_CORTEX_M4     (0x0102)
#define SVCRT_CPU_CORE_CORTEX_M7     (0x0103)

#define SVCRT_CPU_CORE_RV32IMAC      (0x0201)   /* 32 位 RISC-V，机器/用户模式 */
#define SVCRT_CPU_CORE_RV64GC        (0x0202)   /* 64 λ RISC-V                 */

#define SVCRT_CPU_CORE_LA32          (0x0301)   /* 32 λ LoongArch              */
#define SVCRT_CPU_CORE_LA64          (0x0302)   /* 64 λ LoongArch              */

/* ============================================================
 * 兼容旧版配置：SVCRT_CPU_ARCH 取值
 * @brief 历史工程与板级配置使用 0/1/2 选择 Cortex-M 核心，
 *        此处保留原语义，映射到上面的核心编码。
 *        新工程可直接定义 SVCRT_ARCH_CORE 进行选择。
 * ============================================================ */
#define SVCRT_ARCH_CORTEX_M3         (0)
#define SVCRT_ARCH_CORTEX_M4         (1)
#define SVCRT_ARCH_CORTEX_M7         (2)

#ifndef SVCRT_CPU_ARCH
#define SVCRT_CPU_ARCH               SVCRT_ARCH_CORTEX_M4
#endif

/* ============================================================
 * 目标核心推导
 * @brief 优先使用 SVCRT_ARCH_CORE；未定义时按旧版 SVCRT_CPU_ARCH 推导
 * ============================================================ */
#ifndef SVCRT_ARCH_CORE
#if (SVCRT_CPU_ARCH == SVCRT_ARCH_CORTEX_M3)
#define SVCRT_ARCH_CORE              SVCRT_CPU_CORE_CORTEX_M3
#elif (SVCRT_CPU_ARCH == SVCRT_ARCH_CORTEX_M4)
#define SVCRT_ARCH_CORE              SVCRT_CPU_CORE_CORTEX_M4
#elif (SVCRT_CPU_ARCH == SVCRT_ARCH_CORTEX_M7)
#define SVCRT_ARCH_CORE              SVCRT_CPU_CORE_CORTEX_M7
#else
#error "SVCRT_CPU_ARCH 取值非法（仅支持 0/1/2），或请直接定义 SVCRT_ARCH_CORE"
#endif
#endif

/* ============================================================
 * 架构族推导
 * ============================================================ */
#define SVCRT_ARCH_FAMILY_OF(core)   ((core) >> 8)

#ifndef SVCRT_ARCH_FAMILY
#define SVCRT_ARCH_FAMILY            SVCRT_ARCH_FAMILY_OF(SVCRT_ARCH_CORE)
#endif

#define SVCRT_ARCH_IS_ARM            (SVCRT_ARCH_FAMILY == SVCRT_ARCH_FAMILY_ARM)
#define SVCRT_ARCH_IS_RISCV          (SVCRT_ARCH_FAMILY == SVCRT_ARCH_FAMILY_RISCV)
#define SVCRT_ARCH_IS_LOONGARCH      (SVCRT_ARCH_FAMILY == SVCRT_ARCH_FAMILY_LOONGARCH)

#if (SVCRT_ARCH_IS_ARM)
#define SVCRT_ARCH_NAME              "arm"
#define SVCRT_ARCH_LITTLE_ENDIAN     (1)
#elif (SVCRT_ARCH_IS_RISCV)
#define SVCRT_ARCH_NAME              "riscv"
#define SVCRT_ARCH_LITTLE_ENDIAN     (1)
#elif (SVCRT_ARCH_IS_LOONGARCH)
#define SVCRT_ARCH_NAME              "loongarch"
#define SVCRT_ARCH_LITTLE_ENDIAN     (1)
#else
#error "未知架构族，请在 svcrt_arch.h 中补充"
#endif

/* ============================================================
 * 能力派生
 * @brief 内核通过下列宏决定功能开关的默认值，
 *        板级配置仍可显式覆盖（#undef 后重新定义）。
 * ============================================================ */
#if (SVCRT_ARCH_CORE == SVCRT_CPU_CORE_CORTEX_M3)
#define SVCRT_ARCH_HAS_FPU           (0)
#define SVCRT_ARCH_HAS_MPU           (1)
#define SVCRT_ARCH_HAS_PRIV          (1)
#define SVCRT_ARCH_SVC_NUM_BITS      (8)     /* SVC 指令携带 8 位立即数 */
#elif (SVCRT_ARCH_CORE == SVCRT_CPU_CORE_CORTEX_M4)
#define SVCRT_ARCH_HAS_FPU           (1)
#define SVCRT_ARCH_HAS_MPU           (1)
#define SVCRT_ARCH_HAS_PRIV          (1)
#define SVCRT_ARCH_SVC_NUM_BITS      (8)
#elif (SVCRT_ARCH_CORE == SVCRT_CPU_CORE_CORTEX_M7)
#define SVCRT_ARCH_HAS_FPU           (1)
#define SVCRT_ARCH_HAS_MPU           (1)
#define SVCRT_ARCH_HAS_PRIV          (1)
#define SVCRT_ARCH_SVC_NUM_BITS      (8)
#elif (SVCRT_ARCH_CORE == SVCRT_CPU_CORE_RV32IMAC)
#define SVCRT_ARCH_HAS_FPU           (0)
#define SVCRT_ARCH_HAS_MPU           (1)     /* 由 PMP 提供物理内存保护 */
#define SVCRT_ARCH_HAS_PRIV          (1)
#define SVCRT_ARCH_SVC_NUM_BITS      (-1)    /* ecall 无立即数，号由寄存器传递 */
#elif (SVCRT_ARCH_CORE == SVCRT_CPU_CORE_RV64GC)
#define SVCRT_ARCH_HAS_FPU           (1)
#define SVCRT_ARCH_HAS_MPU           (1)
#define SVCRT_ARCH_HAS_PRIV          (1)
#define SVCRT_ARCH_SVC_NUM_BITS      (-1)
#elif (SVCRT_ARCH_CORE == SVCRT_CPU_CORE_LA32)
#define SVCRT_ARCH_HAS_FPU           (0)
#define SVCRT_ARCH_HAS_MPU           (1)     /* 由页表 / 地址窗口提供隔离 */
#define SVCRT_ARCH_HAS_PRIV          (1)
#define SVCRT_ARCH_SVC_NUM_BITS      (15)    /* syscall 指令携带 15 位 code */
#elif (SVCRT_ARCH_CORE == SVCRT_CPU_CORE_LA64)
#define SVCRT_ARCH_HAS_FPU           (1)
#define SVCRT_ARCH_HAS_MPU           (1)
#define SVCRT_ARCH_HAS_PRIV          (1)
#define SVCRT_ARCH_SVC_NUM_BITS      (15)
#else
#error "未知 CPU 核心，请在 svcrt_arch.h 中补充能力派生"
#endif

/* 系统调用参数个数上限（a0~a3 / r0~r3），内核按此约定存取 */
#define SVCRT_ARCH_SVC_ARG_MAX       (4)

/* ============================================================
 * MPU 内存保护区域描述（架构无关的上下文容器）
 * @brief 内核 TCB 中按任务保存一份，任务切换时由 port 层写回硬件。
 *        不同架构只需约定这两组寄存器的语义：
 *        - region_base：区域基址 / 地址寄存器（ARM: RBAR，RISC-V: pmpaddr）
 *        - region_attr：区域属性 / 配置寄存器（ARM: RASR，RISC-V: pmpcfg）
 *        查表数量由 SVCRT_MPU_REGION_MAX 决定，可被板级配置覆盖。
 * ============================================================ */
#ifndef SVCRT_MPU_REGION_MAX
#define SVCRT_MPU_REGION_MAX         (8)
#endif

typedef struct {
    uint32 region_base[SVCRT_MPU_REGION_MAX];
    uint32 region_attr[SVCRT_MPU_REGION_MAX];
} svcrt_arch_mpu_t;

#endif

