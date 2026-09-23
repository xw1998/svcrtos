/**
* @file svcrt_ptable.h
* @brief SVCrtOS 分区表运行期接口（内核侧）
* @details 把 config/svcrt_partition.h 中的编译期布局，在启动时镜像到共享内存的
*          svcrt_partition_table_t 中，供内核、Loader、App 在运行期统一读取；
*          并集中维护统一镜像池的**空闲区分配**与镜像 RAM 池的**伙伴分配**。
*
*          设计原则：
*          1) 只有一个地址源头（config/svcrt_partition.h），本模块负责"翻译"到运行期；
*          2) 只有内核工程包含 svcrt_partition.h，Loader / App 通过
*             SVC 0x18 子命令 1 取得分区表地址后只读使用；
*          3) 槽位状态由本模块集中维护，避免多处直接改写共享内存；
*          4) 空闲空间不额外维护派生结构（位图/链表），而是每次从槽位记录现算：
*             记录数上限是编译期常量（SLOT_MAX <= 16），排序后扫缺口既便宜
*             又不会出现"派生结构与记录不同步"这类难查的错。
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
* @details 用编译期布局填充布局字段，并把所有槽位记录清为空。
*          必须在任务调度启动之前调用（建议在 svcrt_kernel_init 阶段）。
*/
void svcrt_ptable_init(void);

/* Reset only the slot records; the layout fields published by
 * svcrt_layout_init() are preserved (see svcrt_ptable.c). */
void svcrt_ptable_clear_slots(void);

/**
* @brief 获取分区表指针
* @return 指向共享内存中分区表的指针；未初始化时同样返回有效地址
*/
svcrt_partition_table_t *svcrt_ptable_get(void);

/**
* @brief 校验一段镜像区间是否落在可分配范围内
* @param base 镜像起始地址
* @param size 镜像占用的字节数
* @return 0=合法；-1=越界或未按分配粒度对齐
* @details 只检查「池内 + 不越过压实余量 + 落点按 SVCRT_POOL_ALLOC_UNIT 对齐」。
*          是否与其它镜像重叠由 svcrt_ptable_alloc 判定。
*/
int32 svcrt_ptable_range_check(uint32 base, uint32 size);

/**
* @brief 在池里找一段能放下 size 字节的空闲区间（首适配）
* @param size      需要的字节数
* @param p_base    输出落点
* @param p_aligned 输出落点是否是「2 的幂对齐落点」（1=是，可以构造精确 MPU 窗口；
*                  0=落点只是为了塞进缺口，ROM 窗口要退化为包含窗口）
* @return 0=找到；-1=没有足够大的空闲区间
* @details 落点策略（对应「优先精确隔离、退让给容量」）：
*            1. 优先把落点向上取整到 2^n（2^n >= 区间长度）后放置，使镜像
*               自身的跨度成为 2 的幂且基址按该跨度对齐 —— MPU 单区域可精确覆盖；
*            2. 只有当对齐落点放不下而缺口起点放得下时，才用缺口起点做落点，
*               并把 p_aligned 置 0。
*/
int32 svcrt_ptable_find_free(uint32 size, uint32 *p_base, uint32 *p_aligned);

/**
* @brief 在池内登记一段镜像区间（不碰 Flash）
* @param type SVCRT_SLOT_APP / SVCRT_SLOT_DRIVER
* @param base 镜像起始地址
* @param size 镜像占用的字节数
* @return 成功返回槽位记录号（>=0），失败返回 -1（区间非法/重叠/无空闲记录）
* @details 登记成功的记录状态为 INSTALLING：写入完成前不可启动，掉电则由扫描
*          依 CRC 判定为 INVALID。同一区间重复登记会复用原记录（覆盖安装）。
*          安装失败时调用方必须 svcrt_ptable_free()。
*/
int32 svcrt_ptable_alloc(uint32 type, uint32 base, uint32 size);

/**
* @brief 登记一段「不参与分配」的区间（开发期裸镜像）
* @param type  SVCRT_SLOT_APP / SVCRT_SLOT_DRIVER
* @param base  区间起始地址
* @param size  区间字节数
* @param entry 入口地址（裸镜像入口就是区间基址 | Thumb）
* @return 槽位记录号（>=0）或 -1
* @details 状态置为 SVCRT_APP_SLOT_RAW：分配器把它当已占用，压实逻辑跳过它。
*/
int32 svcrt_ptable_alloc_raw(uint32 type, uint32 base, uint32 size, uint32 entry);

/**
* @brief 释放一条槽位记录（同时释放它占用的 Flash 区间与 RAM 块）
* @param slot 槽位记录号
*/
void svcrt_ptable_free(uint32 slot);

/**
* @brief 把一条记录的 Flash 落点改到别处（搬移镜像时用）
* @param slot     槽位记录号
* @param new_base 新落点（必须已通过区间校验且不与其它记录重叠）
* @return 0=成功，-1=失败
* @details 搬移时镜像内容被重新写到新落点，入口地址也要跟着平移，
*          由调用方负责（entry 不在这里算，因为只有 loader 知道负载偏移）。
*/
int32 svcrt_ptable_move(uint32 slot, uint32 new_base);

/**
* @brief 查找区间内的任意地址所属的槽位记录
* @param addr   待查地址
* @param p_base 输出该记录的起始地址（可为 0）
* @param p_size 输出该记录的字节数（可为 0）
* @return 槽位记录号（>=0），未命中返回 -1
*/
int32 svcrt_ptable_find_addr(uint32 addr, uint32 *p_base, uint32 *p_size);

/**
* @brief 查找任务号对应的槽位记录
* @return 槽位记录号（>=0），未命中返回 -1
*/
int32 svcrt_ptable_find_task(uint32 task_id);

/**
* @brief 读取槽位记录的类型 / 基址 / 字节数
* @return 0=成功，-1=槽位非法
*/
int32 svcrt_ptable_slot_info(uint32 slot, uint32 *p_type, uint32 *p_base, uint32 *p_size);

/**
* @brief 读取指定槽位 Flash 中的镜像头
* @param slot 槽位记录号
* @param out  输出镜像头（可为 0，仅做存在性校验）
* @return 0=成功且镜像有效，-1=槽位非法或该槽位是裸镜像，-2=镜像头魔数错误
* @note Flash 已被映射到地址空间，可直接按地址读取。
*/
int32 svcrt_ptable_read_header(uint32 slot, svcrt_app_header_t *out);

/**
* @brief 更新槽位运行期状态
* @param slot    槽位记录号
* @param state   SVCRT_APP_SLOT_x
* @param entry   入口地址（直接赋值，清空槽位时传 0）
* @param task_id 任务号（直接赋值，未启动时传 0）
* @return 0=成功，-1=槽位非法
* @note 三个字段在自旋锁保护下作为一组更新：故障处理路径（可能运行在异常
*       上下文）与安装任务都可能同时改写同一个槽位，否则读者会看到
*       state/entry/task_id 互相不匹配的中间态。
*/
int32 svcrt_ptable_set_slot(uint32 slot, uint32 state, uint32 entry, uint32 task_id);

/**
* @brief 原子读取一个槽位的状态三元组
* @param slot     槽位记录号
* @param p_state  输出状态（可为 0）
* @param p_entry  输出入口地址（可为 0）
* @param p_task_id 输出任务号（可为 0）
* @return 0=成功，-1=槽位非法
*/
int32 svcrt_ptable_get_slot(uint32 slot, uint32 *p_state, uint32 *p_entry, uint32 *p_task_id);

/**
* @brief 从镜像 RAM 池里按 2 的幂分配一块
* @param bytes 镜像头声明的 ram_size
* @param p_base 输出块基址
* @param p_size 输出块大小（2 的幂字节数）
* @return 0=成功，-1=没有合适的块
* @details 块大小 = max(2^n >= bytes, slot_ram_min_block)，并受 slot_ram_max_block 限制。
*          落点必须按块大小对齐 —— 否则单个 MPU region 覆盖不了整块 RAM。
*/
int32 svcrt_ptable_ram_alloc(uint32 bytes, uint32 *p_base, uint32 *p_size);

/**
* @brief 按指定基址预留一块 RAM（复现「已固化在镜像头里的那一个块」）
* @param base 要预留的块基址（必须按 size 对齐）
* @param size 块大小（必须是 2 的幂）
* @return 0=成功，-1=越界 / 未对齐 / 与已占用的块重叠
* @details 与 svcrt_ptable_ram_alloc() 的唯一区别是地址不是现算的、而是人给的。
*          存在的理由：RAM 重定位在安装时已经按某个基址固化进了 Flash，
*          上电重建必须原样占用同一个基址 —— 重新分配会给出另一个地址，
*          镜像里已固化的绝对地址就全部作废（一启动就 MemManage）。
*/
int32 svcrt_ptable_ram_reserve(uint32 base, uint32 size);

/**
* @brief 把 RAM 块记到槽位记录上
* @return 0=成功，-1=槽位非法
*/
int32 svcrt_ptable_ram_bind(uint32 slot, uint32 base, uint32 size);

/**
* @brief 读出一条记录已绑定的 RAM 块
* @return 0=成功，-1=槽位非法（未绑定时 *p_base / *p_size 输出 0）
*/
int32 svcrt_ptable_ram_info(uint32 slot, uint32 *p_base, uint32 *p_size);

/**
* @brief 借出**小程序的代码块与 RAM 块**（两块一起借、一起记账，同一把锁内）
* @param code_bytes 代码块需要的字节数（负载长度按 8 字节上取整，调用方已算好）
* @param ram_bytes  RAM 块需要的字节数（镜像声明的 RAM 需求，本身就是 2 的幂）
* @param p_code_base / p_code_size 输出代码块基址与字节数（2 的幂）
* @param p_ram_base  / p_ram_size  输出 RAM 块基址与字节数（2 的幂）
* @return 0=成功；-1=没有合适的块 / 参数非法 / 已经有一个在跑
* @details 与 svcrt_ptable_ram_alloc() 的区别：那个只算地址、记账要靠调用方
*          再调 ram_bind() 挂到某个槽位上；而小程序**没有槽位（它在文件系统里，
*          不在镜像池里）**，所以分配与记账必须在同一把锁里完成，否则两次
*          分配会拿到同一个块。借用期间两块对伙伴分配器都表现为占用
*          （见 svcrt_pt_collect）。
* @details 为什么两块而不是一块：池的分配粒度是 2 的幂，把代码与 RAM 塞进
*          同一个 pow2 块会让「代码远小于 RAM」的常见情形接近翻倍
*          （代码 1KB + RAM 8KB -> 一块 16KB，而两块只要 1KB + 8KB）。
*          两块各自对齐自己的大小，正好各占一个 MPU 区域。
* @note 两块**同生同死**：要么都借到、要么一块都不借（先给代码块找地方，
*       找不到合适的 RAM 块就把代码块的记账一并放弃）；没有「半借」状态，
*       否则归还路径要处理一种谁也没见过的组合。
* @note 同一时刻只允许一个：再借一次直接返回 -1，不静默共用一块。
*/
int32 svcrt_ptable_mini_alloc(uint32 code_bytes, uint32 ram_bytes,
                              uint32 *p_code_base, uint32 *p_code_size,
                              uint32 *p_ram_base, uint32 *p_ram_size);

/**
* @brief 归还小程序的两块（把 mini_code_* / mini_ram_* 全部清回 0）
* @return 0=成功，-1=本就没借（如实报错，不假装归还成功）
* @details 只清记账，不擦内存内容，也不会失败：调用方（装载失败路径与任务
*          退出路径）必须无论如何都把账还回去，否则每跑一次小程序漏两块 RAM。
*/
int32 svcrt_ptable_mini_free(void);

/**
* @brief 读出小程序的两块（调试 / shell 展示）
* @return 0=成功（未借出时对应输出为 0），-1=四个指针全为空
* @note 允许逐个传 0：只关心 RAM 块的调用者不必准备四个变量。
*/
int32 svcrt_ptable_mini_info(uint32 *p_code_base, uint32 *p_code_size,
                             uint32 *p_ram_base, uint32 *p_ram_size);

/**
* @brief 取池的统计值（供 shell 的 info 命令展示）
* @param p_used  已占用字节数（不含压实余量与裸镜像？含全部占用，便于对账）
* @param p_total 池可分配总字节数（= pool_usable_size）
* @param p_frag  空闲区间个数（>1 表示存在碎片）
* @return 0=成功
*/
int32 svcrt_ptable_stats(uint32 *p_used, uint32 *p_total, uint32 *p_frag);

#if (defined(__cplusplus))
}
#endif

#endif /* __SVCRT_PTABLE_H__ */
