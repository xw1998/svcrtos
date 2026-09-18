/**
* @brief mdk_trace 插桩组件配置 - STM32F401 / SVCrtOS
* @details 由工程直接包含（include path 里有 config/stm32f401），决定插桩事件
*          「怎么离开芯片」。本板只焊了 SWD 两线（没有 SWO），所以选 swd 后端：
*          事件在目标侧压缩进 RAM 环形缓冲，主机用 mdkdebug 的 trace_swd_read
*          增量搬走，未读区永不被覆盖 —— 只要平均搬运速度跟得上，录制零丢失。
*
*          为什么不选 buff：buff 写满会覆盖最旧的记录，只能看到最后一个窗口；
*          无缝录制要的正是「一条都不丢、一直录下去」，两者需求相反。
*          见 mdkdebug 的 trace_guide(topic="swd_seamless")。
*
*          其余旋钮走组件默认值（在 mdk_trace_config_default.h 里）：
*            MDK_TRACE_SWD_BYTES        8192   环字节数 = 静态 RAM 开销
*            MDK_TRACE_SWD_TS_SHIFT     0      时间戳 = DWT 周期，11.9ns/拍
*            MDK_TRACE_SWD_CLEAR_ON_INIT 1     上电清环（无缝流的唯一安全默认）
*/

#ifndef MDK_TRACE_CONFIG_H
#define MDK_TRACE_CONFIG_H

/* 总开关。置 0 则整个组件编译成空函数，一个字节的代码都不留。 */
#define MDK_TRACE_ENABLE              1

/* 后端：SWD 无缝流（压缩入环 + 主机增量搬走）。 */
#define MDK_TRACE_BACKEND_SWD         1

/* 主频：把目标侧周期换算成时间要用到，84MHz。 */
#define MDK_TRACE_CPU_HZ              84000000

/* 异常现场转储：fault handler 里多存一份 pc/lr/sp/xpsr/hfsr/mmfar/bfar。
 * 一块刚跑飞的板子，第一个问题就是「PC 停在哪」，这份现场最值钱。 */
#define MDK_TRACE_FAULT_FRAME         1

#endif /* MDK_TRACE_CONFIG_H */
