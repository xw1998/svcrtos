/**
* @file svcrt_loader.h
* @brief SVCrtOS App 镜像加载器（分区内加载 / 校验 / 启动 / 搬移 / 卸载）
* @details 负责把 App 镜像写入池内空闲空间，并按镜像头信息校验后拉起为任务。
*          镜像格式见 svcrt_app_image.h；分区布局运行期从共享内存分区表获取
*          （见 svcrt_ptable.h），本模块不硬编码任何地址。
*
*          v4 起，镜像自带重定位表，落点不再需要链接期钉死：
*            - 分配：从池底往上找第一段足够长的**已擦除**空间（首适配），
*              因此镜像天然按实际长度紧邻排列，密度等于实际占用之和；
*            - 搬移：镜像可被整体搬到别处（重定位表按 delta 打补丁），
*              使卸载后的死区能够被回收；
*            - 事务：新副本先以 UNCOMMITTED 写入并校验，通过后只改
*              state 一个字提交，最后才擦旧副本。任何时刻掉电，
*              上电扫描都能依 UNCOMMITTED / 重复 image_id 两条规则恢复到
*              确定状态，因此不需要日志扇区。
*
*          分配不依赖任何元数据：Flash 里「连续 0xFF 段」就是可用空间，
*          活镜像必然含非 0xFF 字节（镜像头魔数），所以物理扫描同时完成了
*          「找空位」与「避开已装镜像」两件事，重启后无需恢复分配器状态。
*
*          支持两种来源：
*          1) 内存缓冲区（svcrt_loader_load_buffer）——预留给内核自测 / 后续 OTA 复用；
*          2) 设备流式读取（svcrt_loader_load_dev）——从 UART / SD 等设备接收。
*
* @note 本文件属于内核内部接口；对外经 SVC 0x18（APP_MGR）暴露给用户态。
*/

#ifndef __SVCRT_LOADER_H__
#define __SVCRT_LOADER_H__

#include "svcrt_types.h"
#include "svcrt_app_image.h"

#if (defined(__cplusplus))
extern "C" {
#endif

/* ---- 错误码（成功返回槽位号，>=0；失败返回负值） ---- */
#define SVCRT_LOADER_ERR_PARAM    (-1)   /* 参数非法 */
#define SVCRT_LOADER_ERR_MAGIC    (-2)   /* 镜像头魔数错误 */
#define SVCRT_LOADER_ERR_COMPAT   (-3)   /* 硬件兼容签名不匹配 */
#define SVCRT_LOADER_ERR_SIZE     (-4)   /* 镜像长度非法或超出可用空间 */
#define SVCRT_LOADER_ERR_CRC      (-5)   /* CRC32 校验失败 */
#define SVCRT_LOADER_ERR_FLASH    (-6)   /* Flash 擦写失败 */
#define SVCRT_LOADER_ERR_NO_SLOT  (-7)   /* 无空闲槽位记录 */
#define SVCRT_LOADER_ERR_TASK     (-8)   /* 任务注册失败 */
#define SVCRT_LOADER_ERR_STATE    (-9)   /* 槽位状态不允许该操作 */
#define SVCRT_LOADER_ERR_ADDR     (-10)  /* 落点非法（越界 / 未按分配粒度对齐） */
#define SVCRT_LOADER_ERR_ENTRY    (-11)  /* 镜像头 entry_offset 越界（入口不在负载范围内） */
#define SVCRT_LOADER_ERR_OCCUPIED (-12)  /* 目标区间与已登记槽位重叠 */
#define SVCRT_LOADER_ERR_RELOC    (-13)  /* 重定位表非法，或修补后校验不一致 */
#define SVCRT_LOADER_ERR_NOSPACE  (-14)  /* 池内没有足够大的已擦除空间 */
#define SVCRT_LOADER_ERR_BUSY     (-15)  /* 镜像正在运行，不允许搬移或卸载 */
#define SVCRT_LOADER_ERR_VERSION  (-16)  /* 版本回灌：同 image_id 且版本号不高于已装版本 */
#define SVCRT_LOADER_ERR_DUP      (-17)  /* 重复副本：同一 image_id 且版本与内容完全相同的镜像已在池内 */

/**
* @brief 从内存缓冲区加载完整 App 镜像
* @param image     指向镜像起始（含 256 字节头）的缓冲区
* @param image_len 缓冲区长度（字节），必须 >= 镜像总长
* @return 成功返回槽位号（>=0），失败返回 SVCRT_LOADER_ERR_x
* @details 与设备流式路径共用同一套判定：校验头 -> 找空闲区 -> 登记槽位 ->
*          应用重定位并写入 -> CRC 复核 -> 事务提交 -> 置 LOADED。
*/
int32 svcrt_loader_load_buffer(const uint8 *image, uint32 image_len);

/**
* @brief 从设备流式加载完整镜像（自行同步并读取镜像头）
* @param dev       已打开的设备句柄（数据从镜像头之前开始）
* @param image_len 期望的镜像总长（含头）；传 0 表示由镜像头中的 image_size 决定
* @return 成功返回槽位号（>=0），失败返回 SVCRT_LOADER_ERR_x
* @note 边读边写 Flash，仅使用固定大小分块缓冲，不要求整镜像驻留 RAM。
*/
int32 svcrt_loader_load_dev(int32 dev, uint32 image_len);

/**
* @brief 扫描整个镜像池，把 Flash 中已存在且校验通过的镜像标记为 LOADED
* @return 有效（可启动）的 App 数量
* @details 槽位状态原本只存在于共享 RAM，重启后会丢失；本函数在启动时按
*          镜像头魔数 + 硬件兼容签名 + CRC32 重新认定槽位，使“先烧录镜像、
*          再上电运行”的最小闭环成立。
*
*          扫描按分配粒度（SVCRT_POOL_ALLOC_UNIT）线性遍历整个池：
*          遇到带头镜像则按头的 type 归类为 App / 驱动；遇到裸镜像则查编译期
*          开发槽位表（SVCRT_DEV_SLOTn_*）判定类型与跨度。
*          同时执行断电恢复：
*            - state = UNCOMMITTED 的副本一律作废（搬移中途掉电）；
*            - 同一 image_id 出现两份有效副本时保留地址较低的那份。
*          两者都登记为 INVALID，随后由 svcrt_loader_reclaim() 擦除回收。
*/
uint32 svcrt_loader_scan(void);

/**
* @brief 扫描镜像池，返回其中的驱动数量
* @return 有效（可启动）的驱动数量，0=无有效驱动
* @details 与 svcrt_loader_scan() 共用同一份扫描结果（同一次扫描的驱动视图），
*          因此两者不区分调用顺序，可各自独立调用。
*/
uint32 svcrt_loader_scan_driver(void);

/* ============================================================
 * 固定槽位模式（layout_mode == SVCRT_LAYOUT_MODE_FIXED）
 * ============================================================ */

/**
* @brief 指定下一次安装落在配置槽表的哪一条（-1 = 让内核按类型自己挑）
* @param index 配置槽表下标（0 .. svcrt_layout_slot_count()-1），-1 表示不指定
* @details 只在固定槽位模式下起作用：这条提示只对**下一次**安装有效，
*          安装一开始就被消费掉（无论成功与否），不会泄漏到后续安装。
*          自动选址模式忽略它（同时也会消费掉，避免留下一个陈旧的值）。
* @note 安装窗口由 shell 调用，提示也由 shell 设置；本接口不加锁，
*       调用者必须保证同一时刻只有一次安装在进行。
*/
void  svcrt_loader_slot_hint_set(int32 index);

/** @brief 读取当前的安装槽位提示（-1 = 未指定）。 @see svcrt_loader_slot_hint_set */
int32 svcrt_loader_slot_hint_get(void);

/**
* @brief 回收死区：把「不含活镜像的扇区」擦成 0xFF，使空间重新可用
* @return 本次擦除的扇区数；负值为 SVCRT_LOADER_ERR_x
* @details Flash 擦除粒度是整扇区，因此一个扇区里只要有活镜像就不能擦。
*          本函数对每个含死字节的扇区执行：
*            1) 扇区内已无活镜像 -> 直接擦；
*            2) 扇区内仍有活镜像 -> 先把这些镜像搬到池内最低的已擦除空位
*               （应用重定位，走 UNCOMMITTED -> VALID 事务），再擦。
*          搬不动的镜像（正在运行、或没有足够空位）会让该扇区保留死区，
*          函数跳过它并继续处理其它扇区。卸载/安装后调用一次即可。
*/
int32 svcrt_loader_reclaim(void);

/**
* @brief 查询池内还剩多少可装镜像的物理空间
* @param p_largest 非空时写入最长的一段连续空闲字节数
* @return 所有 0xFF 连续段的总字节数
*/
uint32 svcrt_loader_pool_free(uint32 *p_largest);

/**
* @brief 卸载一个镜像：停止任务、注销记录，然后回收它占用的空间
* @param slot 槽位记录号
* @return 0=成功，负值为 SVCRT_LOADER_ERR_x
* @details 正在运行的任务会被先停止（镜像字节仍在 Flash，可再次启动）；
*          随后注销槽位记录并调用 svcrt_loader_reclaim() 尽量把空间收回来。
* @note 卸载是破坏性操作：擦除后镜像不可恢复，需要重新安装。
*/
int32 svcrt_loader_uninstall(uint32 slot);

/**
* @brief 启动指定驱动槽位的驱动
* @param slot 驱动槽位号
* @return 成功返回任务号（>0），失败返回 SVCRT_LOADER_ERR_x
* @details 栈从该镜像分配到的 RAM 块顶部切出；RAM 块由伙伴分配器按镜像头
*          声明的 ram_size 现算，因此不同镜像永不共用 RAM。
*/
int32 svcrt_loader_start_driver_slot(uint32 slot);

/**
* @brief 驱动槽位的人工重试启动（语义同 svcrt_loader_start_manual）
* @param slot 槽位号
* @return 成功返回任务号（>0），失败返回 SVCRT_LOADER_ERR_x
*/
int32 svcrt_loader_start_driver_manual(uint32 slot);

/**
* @brief 启动 0 号驱动槽（兼容包装）
* @return 成功返回任务号（>0），失败返回 SVCRT_LOADER_ERR_x
*/
int32 svcrt_loader_start_driver(void);

/**
* @brief 停止指定驱动槽位的任务（镜像仍保留在 Flash）
* @param slot 驱动槽位号
* @return 0=成功，负值为 SVCRT_LOADER_ERR_x
*/
int32 svcrt_loader_stop_driver_slot(uint32 slot);

/**
* @brief 查询驱动槽位状态
* @param slot 驱动槽位号
* @return SVCRT_APP_SLOT_x；槽位非法返回 0xffffffff
*/
uint32 svcrt_loader_state_driver(uint32 slot);

/**
* @brief 从设备流式加载（镜像头已由调用方读出）
* @param dev       已打开的设备句柄，位置正好在镜像头之后
* @param p_hdr     已读出的镜像头（调用方已完成魔数同步）
* @param image_len 期望镜像总长（含头），传 0 表示由镜像头决定
* @return 成功返回槽位号（>=0），失败返回 SVCRT_LOADER_ERR_x
* @details 本函数是**唯一的安装实现**，其余安装入口均为其薄包装。流程：
*          1) 校验头部（魔数 / 类型 / 兼容签名 / 长度 / 入口 / 重定位表）；
*          2) 在池内找一段足够大的已擦除空间（首适配，天然紧邻排列）；
*          3) 登记槽位（INSTALLING）；
*          4) 写入镜像头（state=UNCOMMITTED）；
*          5) 流式接收重定位表并落盘；
*          6) 流式接收负载，逐块按 delta 打补丁后落盘（每块回一个 ACK）；
*          7) CRC32 复核（头按 crc32/state 归零计算）；
*          8) 唯一的一个字写入：state = VALID，提交完成；
*          9) 置 LOADED 并清零故障计数，返回槽位号；自启标志不在本函数内
*             写入，由调用方（安装模块）按镜像头 flags 填槽位表。
*          供安装任务使用：先逐字节同步到镜像头魔数，再把头交本函数延续。
*/
int32 svcrt_loader_load_dev_hdr(int32 dev, const svcrt_app_header_t *p_hdr, uint32 image_len);

/**
* @brief 从设备安装驱动镜像（自行读头）
* @param dev       已打开的设备句柄，位置在镜像头之前
* @param image_len 期望镜像总长（含头），传 0 表示由镜像头决定
* @return 成功返回驱动槽位号（>=0），失败返回 SVCRT_LOADER_ERR_x
* @note 镜像头的 type 必须为 SVCRT_APP_TYPE_DRIVER。
*/
int32 svcrt_loader_load_driver(int32 dev, uint32 image_len);

/**
* @brief 从设备安装驱动镜像（镜像头已由调用方读出）
* @return 成功返回驱动槽位号（>=0），失败返回 SVCRT_LOADER_ERR_x
* @details 校验 hdr.type 为 DRIVER 后转交 svcrt_loader_load_dev_hdr()。
*/
int32 svcrt_loader_load_driver_dev(int32 dev, const svcrt_app_header_t *p_hdr, uint32 image_len);

/**
* @brief 按“崩溃重启上限”策略处理 App 任务故障
* @param task_id 发生故障的任务号
* @return 1=已达上限并已禁用该 App，0=未达上限（调用方应恢复/重启该 App），
*         -1=不属于任何槽位（内核任务，调用方沿用默认处理）
* @details 计数按槽位累计（从分区表反查 task_id 归属）：故障一次加一，
*          达到 APP_CRASH_RESTART_MAX 后将该槽位置为 INVALID 并让任务脱离调度。
*          重新安装镜像时计数清零。
* @note 计数由 svcrt_crash 记账：共享 RAM 里的 slot_crash_cnt 只反映本次上电，
*       跨复位活下来的那份在共享 RAM 尾部的 UNINIT 区（CRASH_LOG_BASE）里，
*       开机由 svcrt_crash_scan_apply() 采用，因此“崩溃导致整机复位”的启动环
*       也能被挡住（上限一到即保持禁用、不再自启）。只有掉电才清零。
*/
int32 svcrt_loader_on_fault(int32 task_id);

/**
* @brief 把已加载的槽位拉起为任务（按槽位类型自动分流 App / 驱动）
* @param slot 槽位号
* @return 成功返回任务号（>0），失败返回 SVCRT_LOADER_ERR_x
* @note 这个门不接受被内核禁用的槽位：禁用的意思是“除非有人明确要求，
*       否则别再自己跑起来”。控制台的人工重试用 svcrt_loader_start_manual()。
*/
int32 svcrt_loader_start(uint32 slot);

/**
* @brief 安装前的版本把关：同一 image_id 的已装副本不允许被「不更高」的版本覆盖
* @param p_hdr 待安装镜像的镜像头（调用方已完成魔数同步与基本校验）
* @return 0=可以安装；负值为 SVCRT_LOADER_ERR_x
* @details 三态判定，只按「同一 image_id」比对（image_id 是负载身份，
*          同一应用两次打包只有版本号不同、image_id 相同）：
*          - 版本号高于已装最高版本 -> 放行（升级）；
*          - 版本号相同且 image_size / crc32 也相同 -> ERR_DUP（重复副本）；
*          - 其余（降级回灌、同版本重建）-> ERR_VERSION。
*          只统计 state 为 LOADED / RUNNING 的副本：被崩溃终局策略禁用
*          （INVALID）的副本不算「已装」——重装同一版本正是恢复手段。
* @note 安装路径在写入任何字节之前调用本函数；上电扫描与搬移不调用。
*/
int32 svcrt_loader_check_install(const svcrt_app_header_t *p_hdr);

/**
* @brief 控制台口径的启动：与 svcrt_loader_start() 是同一道门，多一个人工重试
* @param slot 槽位号
* @return 成功返回任务号（>0），失败返回 SVCRT_LOADER_ERR_x
* @details 若该槽位是被崩溃终局策略禁用的，先用启动扫描的同一套校验
*          （svcrt_loader_accept）复验镜像：镜像仍然可用才清掉禁用与计数
*          （这是操作者说“重新开始”）；复验不过就保持禁用并返回 ERR_STATE。
*          悄悄放行一个已经损坏的镜像，比它被禁用更糟。
*/
int32 svcrt_loader_start_manual(uint32 slot);

/**
* @brief 停止槽位对应的任务（镜像仍保留在 Flash）
* @param slot 槽位号
* @return 0=成功，负值为 SVCRT_LOADER_ERR_x
*/
int32 svcrt_loader_stop(uint32 slot);

/**
* @brief 查询槽位状态
* @param slot 槽位号
* @return SVCRT_APP_SLOT_x；槽位非法返回 0xffffffff
*/
uint32 svcrt_loader_state(uint32 slot);

/**
* @brief 按各驱动槽的自启标志批量启动驱动
* @return 实际启动的驱动数量
* @details 自启标志在扫描时由镜像头 flags 回填（裸镜像取开发槽位表配置）。
*          未设自启的驱动不会被拉起，需由上层（如 shell 的 drv start）显式启动。
*          必须在 svcrt_loader_scan_driver() 之后调用。
*/
uint32 svcrt_loader_start_autostart_driver(void);

/**
* @brief 按各 App 槽的自启标志批量启动 App
* @return 实际启动的 App 数量
* @details 与 svcrt_loader_start_autostart_driver() 同口径。
*          必须在 svcrt_loader_scan() 之后调用。
*/
uint32 svcrt_loader_start_autostart(void);

#if (defined(__cplusplus))
}
#endif

/* ============================================================
 * 容器层共用件（对内核内其他装载路径开放）
 * @details 下面两个函数不是为外部调用者"导出"的便利接口，而是**唯一实现**：
 *          镜像头与重定位表的合法性规则、MOVW/MOVT 的编码细节，都必须只有
 *          一份。小程序（svcrt_mini.c）走的是完全不同的装载路径（文件系统 ->
 *          RAM，而不是串口 -> Flash 池），但它解析的是同一种容器，因此复用
 *          这两个函数，而不是另写一套。
 * ============================================================ */

/**
* @brief 校验镜像头：字段级约束 + 重定位表表体合法性
* @param p_hdr      镜像头（调用方已读进可寻址内存）
* @param p_rel      重定位表表体地址；传 0 表示表体此刻还不在可寻址的地方，
*                   此时只校字段，调用方必须在表体可用后补一次带地址的调用
* @param types_mask 接受的镜像类型位掩码（`SVCRT_APP_TYPE_MASK_x`）
* @return 0=通过，负值为 SVCRT_LOADER_ERR_x
* @note 表体校验覆盖：条目数上限、编码版本、`payload_offset` 自洽、每个条目的
*       对齐要求与「落在负载范围内」、以及偏移必须升序。这些规则只能有一份，
*       否则「池内安装」与「小程序装载」会对同一个文件给出不同判定。
*/
int32 svcrt_loader_check_header(const svcrt_app_header_t *p_hdr,
                                const uint32 *p_rel,
                                uint32 types_mask);

/**
* @brief 给一段负载打重定位补丁，并给出本段可安全落盘的末尾
* @param buf       装载缓冲（对应镜像内偏移 buf_off 起的连续 len 字节）
* @param buf_off   该缓冲在**镜像内**的起始偏移（不是内存地址）
* @param len       缓冲长度
* @param p_hdr     镜像头（提供表项数与两条标称基址）
* @param p_rel     重定位表表体（可以是整表，也可以是一段滑动窗口，只要
*                  `p_rel[idx]` 是第 idx 条全局条目）
* @param p_idx     表前进指针（跨调用必须复用同一个变量）
* @param delta_rom 给 ROM 类表项叠加的增量
* @param delta_ram 给 RAM 类表项叠加的增量
* @return 本段可以落盘的末尾偏移（镜像内）。正常等于 buf_off + len；若段尾
*         恰好切开一条 8 字节指令类表项，则等于该表项起始偏移，调用方必须把
*         这段之后的字节留到下一段开头一起处理。
* @note 表项按偏移升序，所以只需要一个前进指针；但返回的"可落盘末尾"不是
*       可选的：忽略它会让被切开的表项在下一次调用里落到
*       「偏移落在本段之前」那条防御分支上而**静默丢掉一次修补**。
*/
uint32 svcrt_loader_reloc_apply(uint8 *buf, uint32 buf_off, uint32 len,
                               const svcrt_app_header_t *p_hdr,
                               const uint32 *p_rel,
                               uint32 *p_idx,
                               uint32 delta_rom, uint32 delta_ram);

/**
* @brief 算镜像头部（前 256 字节）的 CRC，供流式装载方接续计算
* @param p_hdr 镜像头（调用方已读进可寻址内存）
* @return 只覆盖头部 256 字节的 CRC 值（crc32 / state / runtime_ram_base 三个
*         字段按四个 0 字节代入）
* @note 完整镜像 CRC = 本值继续喂入「重定位表」与「负载」两段**标称形态**的字节。
*       小程序的负载是边读边落 RAM、边打补丁的，所以只能在收到字节的那一刻
*       算 CRC，不能在打完补丁之后再回读——回读的是 relocation 之后的形态，
*       与打包工具算的标称形态必然不同。
*/
uint32 svcrt_loader_crc_header(const svcrt_app_header_t *p_hdr);

/**
* @brief 硬停同一 RAM 窗口内的所有任务（镜像级停止）
* @param task_id 该镜像的主任务号（1 起）
* @note RAM 窗口就是归属判据：镜像的代码块/数据块由装载方交给主任务，
*       它自己创建的线程其栈必须落在同一块内（由内核的创建校验保证），
*       内核内置任务住在内核 RAM 区，永远不会匹配。
*       池内卸载与小程序停止必须用同一份判据，不能各写一套。
*/
void svcrt_loader_halt_image(uint32 task_id);

#endif /* __SVCRT_LOADER_H__ */
