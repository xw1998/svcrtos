/**
* @file svcrt_mini.h
* @brief SVCrtOS 小程序（MiniApp）：文件系统里的负载按需 load 进 RAM 执行
* @details 小程序与 App 的关系，一句话说清：
*          - 容器相同：同一种镜像（256 字节头 + 重定位表 + 负载），同一种入口约定，
*            同一个 App SDK（oslib 直接复用）。差别只在镜像头里的 type 字段。
*          - 交付方式不同：App 要先「安装」——由加载器写进 Flash 镜像池、占一个
*            槽位、绑定一块 RAM；小程序**不安装**，它就是文件系统里的一个文件，
*            运行期现读出来、现借一块 RAM、跑完（或停掉）就把这块 RAM 还回池。
*          - 代价是明确的：每次运行都要重读一遍负载，代码只能从 RAM 执行（无法
*            XIP），所以比 App 多花「一次装载时间 + 一块等于 代码+RAM 的池内存」。
*
*          内存下限（这是数学下限，不是实现水平）：无 MMU 的机器上没有 ELF 那种
*          「按页映射、只映射真正用到的部分」；ELF 在 MCU 上的等价物是 XIP——
*          代码留在 Flash 里原地执行，而现有 App 模型已经在用 XIP。既然要求
*          「load 进内存再执行」，代码就必须占 RAM，于是占用是两块：
*              pow2_ceil(align8(负载长度))  +  pow2_ceil(镜像声明的 RAM 需求)
*          **分成两块而不是塞进一个 pow2 块**：池的分配粒度就是 2 的幂，把两者
*          合并成 pow2_ceil(代码 + RAM) 会在最常见的「代码远小于 RAM」时接近
*          翻倍——代码 1KB + RAM 8KB 会要一块 16KB，而两块只要 1KB + 8KB。
*          两块各自对齐自己的大小，正好各占一个 MPU 区域（代码窗 / 数据窗），
*          数据窗也就不用再迁就代码窗的「可执行」属性。
*          再往下只剩两处可省（合并两块的写法省不掉它们，见上）：
*            1. 头与重定位表不落 RAM（读完即丢；负载直接读进代码块基址）；
*            2. 不设第二份中转缓冲（读一段、打一段补丁）。
*
* @author xw
* @date 2026.09.23
*/
#ifndef __SVCRT_MINI_H__
#define __SVCRT_MINI_H__

#include "svcrt_def.h"

/* 只有这一个错误码是本路径独有的；其余全部沿用 SVCRT_LOADER_ERR_x
 * （MAGIC / COMPAT / SIZE / CRC / RELOC / ENTRY / TASK / STATE / NOSPACE / BUSY），
 * 免得同一种错误在两条装载路径上报出两个名字。 */
#define SVCRT_MINI_ERR_FS      (-18)   /* 读镜像文件失败：原因见 svcrt_fs_last_error() */

/** @brief 一次小程序装载的结果（svcrt_mini_info() 的输出） */
typedef struct {
    uint32 task_id;      /* 主任务号（1 起）；0 = 没有在跑 */
    uint32 code_base;    /* 代码块基址（2 的幂对齐） */
    uint32 code_block;   /* 代码块字节数（2 的幂，>= code_size） */
    uint32 code_size;    /* 负载长度（镜像头里的 image_size） */
    uint32 ram_base;     /* RAM 块基址 = 镜像 RW/ZI 与栈的运行起点 */
    uint32 ram_size;     /* RAM 块字节数（2 的幂，含 RW/ZI + 栈） */
    uint32 entry;        /* 入口地址（已按 Cortex-M 约定带上 Thumb 位） */
    uint32 crc;          /* 本次装载算出的镜像 CRC（与镜像头里的值一致） */
} svcrt_mini_info_t;

/**
* @brief 从文件系统装载并启动一个小程序（已有在跑/正在装则拒绝）
* @param path 串口侧路径：文件系统卷内的绝对路径，如 "/mini/hello.svcm"
* @return 0=已启动；负值为 SVCRT_LOADER_ERR_x 或 SVCRT_MINI_ERR_FS
* @note 同一时刻只允许一个：小程序借的是「一块」池内存，两个并行跑会让
*       第二个把第一个的代码区覆盖掉，而这种错现场查不出来。
*       **不会**静默共用、也不会挤掉正在跑的那个——直接报 BUSY。
* @note 装载期间持有调度器锁（与 SVC 层访问文件系统同一套约定）：文件系统
*       只有一份缓存，两个任务交错读同一个文件会把彼此的窗口顶偏。
*/
int32 svcrt_mini_run(const char *path);

/**
* @brief 停止正在跑的小程序并归还它的 RAM 块
* @return 0=已停止；SVCRT_LOADER_ERR_STATE=本来就没有在跑
* @details 停止顺序不能反：先把镜像（含它自己创建的线程）踢出调度，再归还块。
*          反过来的话，块可能立刻被下一个装载者拿去写代码，而原任务还在跑。
*/
int32 svcrt_mini_stop(void);

/**
* @brief 读出当前小程序的状态
* @return 0=有在跑的（*p_out 有效）；-1=没有在跑（*p_out 已清零）
*/
int32 svcrt_mini_info(svcrt_mini_info_t *p_out);

/**
* @brief 读出「单个小程序能占用的最大块字节数」这个运行期上限
* @return 当前上限（字节，2 的幂）
* @details 默认值就是编译期的 SLOT_RAM_MAX_BLOCK。这个上限**可以在运行期收紧**
*          （shell: mini limit <字节>），但**不能放宽到超过 SLOT_RAM_MAX_BLOCK**：
*          那个常量同时还是池单块物理上限、App 镜像校验上限与 MPU 能给出的数据
*          窗口上限（SVCRT_MPU_RAM_BLOCK_MAX）。往上放会掉进 svcrt_mpu.c 的
*          「内核 RAM 兜底窗口」分支——那是一种会静默把任务放进错误窗口的失败，
*          不应该由一次调参触发。要真正放开天花板，得改 SLOT_RAM_MAX_BLOCK
*          并同时满足池与 MPU 的静态断言。
*/
uint32 svcrt_mini_max_bytes(void);

/**
* @brief 设置单个小程序的体积上限（块的字节数）
* @param bytes 2 的幂，范围 [SLOT_RAM_MIN_BLOCK, SLOT_RAM_MAX_BLOCK]；0 = 恢复编译期默认
* @return 0=已生效；SVCRT_LOADER_ERR_PARAM=不在范围内或不是 2 的幂（不静默夹取）
* @note 只影响**下一次**装载；已经在跑的小程序不受影响（它的块已经借出去了）。
*/
int32 svcrt_mini_set_max_bytes(uint32 bytes);

/**
* @brief 任务退出路径的回收钩子（由 svcrt_task_kill_internal 调用）
* @param task_id 正在退出的任务号
* @return 1=这是小程序的主任务（本函数已停掉它的线程并归还块）；
*         0=与本模块无关（不是小程序，或退出的是小程序内部的子线程）
* @details 主任务退出 = 整个小程序结束。子线程退出不动块：小程序可能只是
*          收掉了一个 worker，主任务还要继续跑。
*/
int32 svcrt_mini_on_task_exit(uint32 task_id);

#endif /* __SVCRT_MINI_H__ */
