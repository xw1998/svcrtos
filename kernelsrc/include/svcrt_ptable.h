/**
* @file svcrt_ptable.h
* @brief SVCrtOS 分区表运行期接口（内核侧）
* @details 把 config/svcrt_partition.h 中的编译期布局，在启动时镜像到共享内存的
*          svcrt_partition_table_t 中，供内核、Loader、App 在运行期统一读取。
*
*          设计原则：
*          1) 只有一个地址源头（config/svcrt_partition.h），本模块负责"翻译"到运行期；
*          2) 只有内核工程包含 svcrt_partition.h，Loader / App 通过
*             SVC 0x18 子命令 1 取得分区表地址后只读使用；
*          3) 槽位状态由本模块集中维护，避免多处直接改写共享内存。
*
* @note 本文件属于内核内部接口。
*/

#ifndef __SVCRT_PTABLE_H__
#define __SVCRT_PTABLE_H__

#include "svcrt_types.h"
#include "svcrt_share.h"
#include "svcrt_app_image.h"

#if (defined(__cplusplus))
extern "C" {
#endif

/**
* @brief 初始化共享内存中的分区表
* @details 用编译期布局填充布局字段，并把所有槽位状态清为空。
*          必须在任务调度启动之前调用（建议在 svcrt_kernel_init 阶段）。
*/
void svcrt_ptable_init(void);

/**
* @brief 获取分区表指针
* @return 指向共享内存中分区表的指针；未初始化时同样返回有效地址
*/
svcrt_partition_table_t *svcrt_ptable_get(void);

/**
* @brief 读取指定槽位 Flash 中的 App 镜像头
* @param slot 槽位号（0 ~ app_max_count-1）
* @param out  输出镜像头（可为 0，仅做存在性校验）
* @return 0=成功且镜像有效，-1=槽位非法，-2=镜像头魔数错误
* @note Flash 已被映射到地址空间，可直接按地址读取。
*/
int32 svcrt_ptable_read_header(uint32 slot, svcrt_app_header_t *out);

/**
* @brief 更新槽位运行期状态
* @param slot    槽位号
* @param state   SVCRT_APP_SLOT_x
* @param entry   入口地址（直接赋值，清空槽位时传 0）
* @param task_id 任务号（直接赋值，未启动时传 0）
* @return 0=成功，-1=槽位非法
*/
int32 svcrt_ptable_set_slot(uint32 slot, uint32 state, uint32 entry, uint32 task_id);

#if (defined(__cplusplus))
}
#endif

#endif /* __SVCRT_PTABLE_H__ */
