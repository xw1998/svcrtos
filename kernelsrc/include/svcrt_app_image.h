/**
* @file svcrt_app_image.h
* @brief SVCrtOS App 镜像文件格式（打包工具与 Loader 共用）
* @details App 镜像 = 固定 256 字节头 + 代码/数据段原始内容（负载）+ 重定位表。
*          镜像由 App 工程编译产物转换而来（见 tools/pack_app.py），
*          Loader 按本结构解析、校验、重定位并写入镜像池。
*
*          三段式内部布局（偏移均相对「镜像起始」）：
*              [0, 256)                                镜像头
*              [256, 256 + reloc_count*4)              重定位表（每项 4 字节）
*              [payload_offset, ...)                   负载（链接基址 = nominal_base）
*
*          重定位表放在负载**之前**不是随意选择：安装是流式的（从串口边收边写
*          Flash），只有先拿到重定位表，内核才能在负载字节到达时就地打补丁。
*          表若放在负载之后，就得先把整镜像落到 Flash 再回头改——而 Flash
*          只能把 1 写成 0、不能反向改，那条路走不通。
*          表项按偏移升序排列，因此流式打补丁只需要一个前进指针，无需缓存整表。
*
*          重定位表由打包工具用「同一份负载链接四次后逐字节比对」生成：
*              A = 标称基址；B = ROM 基址 +delta；C = RAM 基址 +delta；
*              D（验证）= ROM 基址 +2*delta、RAM 基址 +delta
*          A→B 恰好平移 delta 而 A→C 不变的字是 ROM 类表项；A→C 平移 delta
*          而 A→B 不变的是 RAM 类表项。把生成出来的表应用回 A 必须逐字节
*          等于 D，否则打包直接失败。
*          因此「镜像能不能搬到任意 2 的幂对齐落点」这件事在打包阶段就被证明，
*          而不是留到板子上随机崩溃。
*
* @note 本文件不含任何具体地址信息，App 与内核均可包含。
*/

#ifndef __SVCRT_APP_IMAGE_H__
#define __SVCRT_APP_IMAGE_H__

#include "svcrt_types.h"

/** @brief App 镜像头魔数 "SVCA" */
#define SVCRT_APP_MAGIC           (0x53564341u)

/** @brief App 镜像头固定长度（字节） */
#define SVCRT_APP_HEADER_SIZE     (256u)

/** @brief 镜像类型 */
#define SVCRT_APP_TYPE_APP        (1u)
#define SVCRT_APP_TYPE_DRIVER     (2u)

/** @brief 镜像头 flags 位定义（见 svcrt_app_header_t.flags）
 *  @note 该字段落在头部 offset 96（原 reserved 区首 4 字节）。旧镜像该位置
 *        恒为 0，等价于「不设任何标志」，因此新增本字段不破坏旧镜像。 */
#define SVCRT_APP_FLAG_AUTOSTART  (1u << 0)   /* 开机扫描认定后被自动启动 */

/** @brief 未设置任何标志 */
#define SVCRT_APP_FLAG_NONE       (0u)

/**
* @brief 镜像提交状态（svcrt_app_header_t.state）
* @details 搬移一个镜像时先在新落点写一份 UNCOMMITTED 副本，校验通过后
*          只把这一个字段改写为 VALID（单字编程，原子），最后才擦除旧副本。
*          上电扫描见到 UNCOMMITTED 一律丢弃；见到同 image_id 的两份 VALID
*          则保留地址较低的那份。两条规则合起来使断电恢复幂等且确定，
*          因此不需要额外的日志扇区。
*/
#define SVCRT_APP_STATE_VALID       (0u)      /* 已提交，可用（旧镜像恒为该值） */
#define SVCRT_APP_STATE_UNCOMMITTED (1u)      /* 未提交，上电必须丢弃 */

/**
* @brief 重定位表条目编码
* @details 每项是一个 32 位字：低 2 位是类型，高位是「相对镜像起始」的偏移。
*          表项按偏移升序排列。
*
*          为什么需要两种类型：镜像里既有指向**自身代码/只读数据**的绝对地址，
*          也有指向**自己的 RAM 窗口**的绝对地址。前者随 Flash 落点平移，
*          后者随 RAM 块基址平移，两者增量不同，必须分开标记。
*            kind = 0（ROM）：装载时 word += (落点 + 负载偏移 - nominal_base)
*            kind = 1（RAM）：装载时 word += (RAM 块基址 - nominal_ram_base)
*          之所以 ROM 增量还要加上「负载偏移」：负载被链接在 nominal_base 上，
*          运行期却落在 落点+payload_offset——重定位表本身把负载往后推了
*          reloc_count*4 字节，这部分位移必须计入，否则代码里的自引用会整体
*          偏出一个表长。
*          两个基址都在镜像头里声明，因此单个镜像被整体搬移时只需要
*          对 ROM 类表项再叠加一个增量（新落点 - 旧落点），RAM 类不动。
*
*          @note 偏移低 2 位用来放类型，因此偏移必须按「半字」为单位存放：
*                表项 = (offset / 2) << 2 | kind。Thumb 的 32 位指令只保证
*                半字对齐，MOVW/MOVT 指令对完全可能落在 2 mod 4 的偏移上；
*                若按字节直接存放偏移，低 2 位会被类型覆盖，这类表项会静默
*                少 2 字节（内存里的表是对的，写进镜像再读出来才错）。
*/
#define SVCRT_APP_RELOC_KIND_MASK  (0x3u)
#define SVCRT_APP_RELOC_KIND_ROM   (0u)
#define SVCRT_APP_RELOC_KIND_RAM   (1u)

/** @brief MOVW/MOVT 立即数对里的 ROM 类地址（表项占 8 字节：两条指令） */
#define SVCRT_APP_RELOC_KIND_ROM_MOVW  (2u)

/** @brief MOVW/MOVT 立即数对里的 RAM 类地址（表项占 8 字节：两条指令） */
#define SVCRT_APP_RELOC_KIND_RAM_MOVW  (3u)

/** @brief 重定位表编码版本：偏移按字节存放（已废弃，低 2 位会被类型截断） */
#define SVCRT_APP_RELOC_KIND_32    (0u)

/** @brief 重定位表编码版本：偏移按半字存放（当前版本） */
#define SVCRT_APP_RELOC_KIND_HALF  (1u)

/** @brief 内核唯一接受的表编码版本 */
#define SVCRT_APP_RELOC_KIND_CUR   (SVCRT_APP_RELOC_KIND_HALF)

/** @brief 表项里偏移的存放单位（字节）：2 = 按半字 */
#define SVCRT_APP_RELOC_OFF_UNIT   (2u)

/** @brief 从条目取出偏移（相对镜像起始，按半字存放，恒为偶数） */
#define SVCRT_APP_RELOC_OFF(e) \
    (((e) & ~SVCRT_APP_RELOC_KIND_MASK) >> 1)

/** @brief 由偏移与类型组装条目（与 SVCRT_APP_RELOC_OFF 互逆） */
#define SVCRT_APP_RELOC_ENTRY(off, kind) \
    ((((uint32)(off) / SVCRT_APP_RELOC_OFF_UNIT) << 2) | \
     ((uint32)(kind) & SVCRT_APP_RELOC_KIND_MASK))

/** @brief 从条目取出类型 */
#define SVCRT_APP_RELOC_KIND(e)    ((e) & SVCRT_APP_RELOC_KIND_MASK)

/** @brief 一条重定位表目占用的字节数 */
#define SVCRT_APP_RELOC_SIZE       (4u)

/** @brief MOVW/MOVT 表项覆盖的字节数（两条 4 字节指令） */
#define SVCRT_APP_RELOC_MOV_SIZE   (8u)

/** @brief 重定位表无需 CRC 归零的字段，故整表参与校验；本宏仅为可读性 */
#define SVCRT_APP_RELOC_NONE       (0u)

/** @brief 重定位表条目数上限（内核用于边界校验；表本身不驻留 RAM）
 *  @note 表项按「需要修补的字」计数，不是按镜像字节数。4 KB 表 = 1024 个
 *        条目，对 1 MB 池里最大的镜像（768 KB 负载）也留出足够余量。 */
#define SVCRT_APP_RELOC_MAX        (1024u)

/** @brief 重定位表字节数上限（= 条目上限 x 每项字节数） */
#define SVCRT_APP_RELOC_TABLE_MAX  (SVCRT_APP_RELOC_MAX * SVCRT_APP_RELOC_SIZE)

/** @brief 重定位表在镜像内的固定偏移（紧跟镜像头） */
#define SVCRT_APP_RELOC_OFFSET     (SVCRT_APP_HEADER_SIZE)

/**
* @brief 负载相对镜像起始的偏移（= 头 + 重定位表）
* @param p_hdr 指向 svcrt_app_header_t 的指针
*/
#define SVCRT_APP_PAYLOAD_OFFSET(p_hdr) \
    (SVCRT_APP_HEADER_SIZE + ((p_hdr)->reloc_count * SVCRT_APP_RELOC_SIZE))

/**
* @brief 镜像总长度（头 + 重定位表 + 负载）
* @param p_hdr 指向 svcrt_app_header_t 的指针
*/
#define SVCRT_APP_TOTAL_LEN(p_hdr) \
    (SVCRT_APP_PAYLOAD_OFFSET(p_hdr) + (p_hdr)->image_size)

/**
* @brief App 镜像头（固定 256 字节）
* @note CRC 覆盖范围：本头结构体（crc32 与 state 两个字段按 0 参与计算）
*       + 负载 + 重定位表。之所以把 state 排除在外，是因为它必须能在写入
*       之后被单独改写（提交动作），而 CRC 不能因此失效。
*       crc32 字段自身的偏移见 SVCRT_APP_OFF_CRC32，state 见 SVCRT_APP_OFF_STATE。
*/
typedef struct {
    uint32 magic;               /* SVCRT_APP_MAGIC */
    uint32 type;                /* SVCRT_APP_TYPE_x */
    uint32 hw_compat_id;        /* 与内核的 SVCRT_HW_COMPAT_ID 比对 */
    uint32 version;             /* 镜像版本号（供后续升级/回滚使用） */

    uint32 image_size;          /* 负载长度（不含头与重定位表） */
    uint32 entry_offset;        /* 入口相对「负载起始」的偏移（通常 0x00000008） */
    uint32 nominal_base;        /* 负载的 ROM 标称基址：打包时负载被链接到的地址
                                   (= 池基址 + 镜像头长)。内核按
                                   「落点 + payload_offset - 本字段」算 ROM 增量 */
    uint32 crc32;               /* 镜像校验值（计算时本字段按 0） */

    uint8  signature[64];       /* 预留：数字签名 */
    uint32 flags;               /* SVCRT_APP_FLAG_x；0 表示不自启（旧镜像即为此值） */
    uint32 state;               /* SVCRT_APP_STATE_x（计算 CRC 时按 0） */
    uint32 image_id;            /* 负载身份 = 负载的 crc32；用于重复副本判定 */
    uint32 ram_size;            /* 镜像运行所需 RAM 字节数（伙伴分配器的依据） */
    uint32 reloc_offset;        /* 重定位表相对镜像起始的偏移，恒为 SVCRT_APP_RELOC_OFFSET */
    uint32 reloc_count;         /* 重定位表条目数（必须 <= SVCRT_APP_RELOC_MAX） */
    uint32 reloc_kind;          /* 重定位表编码版本，当前恒为 SVCRT_APP_RELOC_KIND_32 */
    uint32 payload_offset;      /* 负载相对镜像起始的偏移，必须 = 头长 + reloc_count*4 */
    uint32 nominal_ram_base;    /* RAM 重定位标称基址：镜像 RW/ZI 的链接基址。
                                   实际 RAM 块由内核按 ram_size 分配，两者之差即 RAM 增量 */
    uint8  reserved[124];       /* 预留：填充至 256 字节 */
} svcrt_app_header_t;

/** @brief crc32 字段在头内的偏移（Loader 计算 CRC 时要把它按 0 处理） */
#define SVCRT_APP_OFF_CRC32       (28u)

/** @brief state 字段在头内的偏移（同上） */
#define SVCRT_APP_OFF_STATE       (100u)

/**
* @brief 镜像头尺寸静态校验
* @details 头结构与 SVCRT_APP_HEADER_SIZE 必须严格一致：打包工具与 Loader 分别按
*          “结构体布局”和“固定长度宏”解释镜像，一旦不一致就会静默错位。
*          此断言让不一致在编译期直接暴露。
*/
typedef char svcrt_app_header_size_check[
    (sizeof(svcrt_app_header_t) == SVCRT_APP_HEADER_SIZE) ? 1 : -1];

/**
* @brief 关键字段偏移静态校验
* @details Loader 计算 CRC 时按「偏移切片」而不是整体结构体来累加 CRC
*          （crc32 与 state 两个字段要按 0 参与），因此这些偏移一旦漂移
*          就会静默算出错误的校验值。这里逐个钉死。
*/
typedef char svcrt_app_off_crc32_check[
    (((uint32)&(((svcrt_app_header_t *)0)->crc32)) == SVCRT_APP_OFF_CRC32) ? 1 : -1];
typedef char svcrt_app_off_state_check[
    (((uint32)&(((svcrt_app_header_t *)0)->state)) == SVCRT_APP_OFF_STATE) ? 1 : -1];

#if (defined(__cplusplus))
extern "C" {
#endif

/**
* @brief 计算 CRC32（与打包工具使用同一算法：IEEE 802.3 反射多项式）
* @param data 数据指针
* @param len  数据长度（字节）
* @param crc  前一段的 CRC 值（首段传 0）
* @return 累积的 CRC32 值
*/
uint32 svcrt_crc32(const void *data, uint32 len, uint32 crc);

#if (defined(__cplusplus))
}
#endif

#endif /* __SVCRT_APP_IMAGE_H__ */
