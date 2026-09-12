/**
* @brief SVCrtOS 故障记录模块（内核内部头文件）
* @details 环形记录系统故障（HardFault/栈溢出/任务恢复等），
*          应用可通过 SVC 查询与读取，用于事后诊断。
*          记录操作中断安全，可在 HardFault 处理路径中调用。
*/

#ifndef __SVCRT_FAULT_H__
#define __SVCRT_FAULT_H__

#include "svcrt_def.h"
#include "svcrt_task.h"
#include "svcrt_config.h"

typedef enum {
    SVCRT_FAULT_HARDFAULT = 1,      /* HardFault 异常 */
    SVCRT_FAULT_STACKOVF  = 2,      /* 任务栈溢出 */
    SVCRT_FAULT_TASKKILL  = 3,      /* 任务被终止 */
    SVCRT_FAULT_RECOVER   = 4,      /* 任务故障恢复 */
    SVCRT_FAULT_SCHEDLOCK = 5,      /* 调度器锁定期间调用阻塞接口（编程错误） */
    SVCRT_FAULT_APPDISABLED = 6,     /* App 连续故障达上限被禁用（不再重启） */
    SVCRT_FAULT_NOSLOT     = 7,      /* 任务表已满或参数非法，任务注册失败 */
    SVCRT_FAULT_MEMFAULT   = 8,      /* MemManage：MPU 违规，应用越权访问 */
    SVCRT_FAULT_BUSFAULT   = 9,      /* BusFault：非法总线访问 */
    SVCRT_FAULT_USGFAULT   = 10      /* UsageFault：未定义指令 / 非法状态 */,
    SVCRT_FAULT_INSTALLFAIL = 11     /* 安装写入失败（镜像已收全但落盘失败） */
} svcrt_fault_type_t;

/* 单条故障记录（3 个字，与 svcrt_fault_record_read 的 out3 对应） */
typedef struct {
    uint32 type;                    /* svcrt_fault_type_t */
    int32  task_id;                 /* 关联任务号（0=内核/中断上下文） */
    uint32 tick;                    /* 故障发生时的系统 tick */
} svcrt_fault_record_t;

void  svcrt_fault_module_init(void);
void  svcrt_fault_record(uint32 type, int32 task_id);
const svcrt_fault_record_t *svcrt_fault_record_get(int32 index);   /* index 从 0 开始，越界返回 0 */
int32 svcrt_fault_record_count_internal(void);

#endif
