# -*- coding: utf-8 -*-
"""
P0-3 修复：.svcapp 安装路径整体错位 256 字节

问题（已确认）：
  槽位内物理布局是   [槽位基址: 256 字节镜像头][槽位基址+256: 负载]
  但 App/驱动工程的 .sct 把负载链接到了「槽位基址」本身
  （gen_scatter.py 用 APP_SLOT0_BASE / DRIVER_POOL_BASE 作为 ROM 基址），
  Loader 计算入口也是 base + entry_offset（少加一个头长）。

  结果：
    - 入口地址指向魔数字节（0x53564341），一启动就取指异常；
    - 负载整体后移 256 字节，所有绝对地址引用（.data 拷贝地址、
      函数指针表、字面量池里的常量地址）全部指向错误位置。
  此前之所以“看着能跑”，是因为裸镜像路径（APP_ALLOW_RAW_IMAGE=1，默认开启）
  把负载直接烧在槽位基址，绕开了带头的安装路径。

修复（行业惯例，与 MCUboot 一致）：
  「负载的链接基址 = 槽位基址 + 镜像头长度」，全链路统一：
   1) config 新增 APP_IMAGE_HEADER_SIZE（唯一源头）；
   2) gen_scatter.py：app/driver 默认按“槽位基址 + 头长”生成 .sct，
      并保留 --raw 生成开发调试用的裸镜像布局；
   3) pack_app.py：期望负载基址 = 槽位基址 + 头长，load_addr 仍写槽位基址；
   4) 内核 svcrt_loader_entry_addr：入口 = 分区基址 + 头长 + entry_offset。
"""
import os
import shutil

ROOT = r'D:\工作\git_project\svcrtos_new'
BK = r'C:\Users\14905\OneDrive\Documents\lingxi-claw\20260911-13-47-19-445\backup_p0_fault'

FILES = [
    'config/svcrt_partition.h',
    'tools/gen_scatter.py',
    'tools/pack_app.py',
    'kernelsrc/src/svcrt_loader.c',
]


def do_backup():
    os.makedirs(BK, exist_ok=True)
    for rel in FILES:
        shutil.copy2(os.path.join(ROOT, rel.replace('/', os.sep)),
                     os.path.join(BK, rel.replace('/', '__')))


class Doc(object):
    def __init__(self, rel, enc='utf-8'):
        self.rel = rel
        self.enc = enc
        self.p = os.path.join(ROOT, rel.replace('/', os.sep))
        self.raw = open(self.p, 'rb').read()
        self.nl = b'\r\n' if b'\r\n' in self.raw else b'\n'

    def t(self, s):
        s = s.replace('\n', '\r\n') if self.nl == b'\r\n' else s
        return s.encode(self.enc)

    def sub(self, old, new, label, count=1):
        ob, nb = self.t(old), self.t(new)
        n = self.raw.count(ob)
        assert n == count, '[%s] %s: 期望 %d 次，实际 %d 次' % (self.rel, label, count, n)
        self.raw = self.raw.replace(ob, nb)
        print('  ok:', label)

    def save(self):
        open(self.p, 'wb').write(self.raw)


# ================================================================ 1. config
CFG_OLD = """/* ============================================================
 * 六、开发调试策略"""

CFG_NEW = """/* ============================================================
 * 七、镜像在槽位内的布局（安装路径 vs 开发调试路径）
 * @details .svcapp 镜像 = 固定 256 字节头 + 负载，整块写入槽位：
 *
 *              槽位基址                          槽位基址+256
 *              +------------------+-------------------------------+
 *              | 镜像头 (256 字节) |      负载（代码 / 只读数据）   |
 *              +------------------+-------------------------------+
 *
 *          因此「负载的链接基址」= 槽位基址 + APP_IMAGE_HEADER_SIZE，
 *          各 App / 驱动工程的 .sct 必须按该基址生成
 *          （tools/gen_scatter.py --target app|driver 的默认布局）。
 *          entry_offset 是入口相对「负载起始」的偏移，内核计算入口地址时
 *          会先加上本值（见 svcrt_loader_entry_addr）。
 * @note 开发调试用的裸镜像路径（APP_ALLOW_RAW_IMAGE=1）没有镜像头，负载
 *       直接位于槽位基址，其 .sct 用 tools/gen_scatter.py --raw 单独生成，
 *       两条路径的二进制互不通用。
 * ============================================================ */
#define APP_IMAGE_HEADER_SIZE (256)          /* .svcapp 镜像头长度，必须与 svcrt_app_image.h 一致 */

/* ============================================================
 * 六、开发调试策略"""


def patch_config():
    d = Doc('config/svcrt_partition.h')
    d.sub(CFG_OLD, CFG_NEW, '新增镜像布局说明与头长宏')
    d.save()


# ================================================================ 2. gen_scatter
GS_DOC_OLD = """    3. 生成的 .sct 属于构建产物，建议放在 build/ 且不纳入版本库。
\"\"\""""

GS_DOC_NEW = """    3. 生成的 .sct 属于构建产物，建议放在 build/ 且不纳入版本库；
    4. app / driver 默认按「.svcapp 安装路径」生成：负载链接基址 =
       槽位基址 + APP_IMAGE_HEADER_SIZE（槽位前 256 字节留给镜像头）；
       加 --raw 则按「开发调试裸镜像路径」生成，负载直接位于槽位基址。
\"\"\""""

GS_GEN_OLD = """def gen_target(layout, target, out_path):
    v = layout.v
    d = os.path.dirname(os.path.abspath(out_path))
    if d and not os.path.isdir(d):
        os.makedirs(d)

    ccm = (v("CHIP_CCM_BASE"), v("CHIP_CCM_SIZE"))
    with open(out_path, "w", encoding="utf-8") as out:
        if target == "kernel":
            emit("KERNEL", v("KERNEL_BASE"), v("KERNEL_SIZE"),
                 v("KERNEL_RAM_BASE"), v("KERNEL_RAM_SIZE"), out, ccm=ccm)
        elif target == "driver":
            emit("DRIVER", v("DRIVER_POOL_BASE"), v("DRIVER_POOL_SIZE"),
                 v("DRIVER_RAM_BASE"), v("DRIVER_RAM_SIZE"), out,
                 stack_size=v("DRIVER_TASK_STACK_SIZE"))
        elif target == "app":
            emit("APP", v("APP_SLOT0_BASE"), v("APP_SLOT0_SIZE"),
                 v("APP_RAM_BASE"), v("APP_RAM_SIZE"), out,
                 stack_size=v("APP_TASK_STACK_SIZE"))
        elif target == "boot":
            if v("BOOT_SIZE") <= 0:
                raise SystemExit("BOOT_SIZE 为 0，未划分 Bootloader 区（如需 Boot，请先在配置头中设置 BOOT_SIZE）")
            emit("BOOT", v("BOOT_BASE"), v("BOOT_SIZE"),
                 v("BOOT_RAM_BASE"), v("BOOT_RAM_SIZE"), out)
        else:
            raise SystemExit("未知 target: %s" % target)
    print("[gen_scatter] %-7s -> %s" % (target, out_path))"""

GS_GEN_NEW = '''def gen_target(layout, target, out_path, raw=False):
    """生成一个映像的分散加载文件

    raw=False（默认）：按 .svcapp 安装路径——槽位前 APP_IMAGE_HEADER_SIZE 字节
        留给镜像头，负载链接基址 = 槽位基址 + 头长；Loader 按同一契约计算入口。
    raw=True        ：按开发调试的裸镜像路径——无镜像头，负载直接位于槽位基址。
    """
    v = layout.v
    d = os.path.dirname(os.path.abspath(out_path))
    if d and not os.path.isdir(d):
        os.makedirs(d)

    def image_layout(base, size):
        """把「槽位基址 / 槽位大小」换算成「负载链接基址 / 负载可用大小」"""
        if raw:
            return base, size
        hdr = v("APP_IMAGE_HEADER_SIZE")
        if size <= hdr:
            raise SystemExit("分区过小：0x%X 字节装不下 %d 字节镜像头" % (size, hdr))
        return base + hdr, size - hdr

    ccm = (v("CHIP_CCM_BASE"), v("CHIP_CCM_SIZE"))
    with open(out_path, "w", encoding="utf-8") as out:
        if target == "kernel":
            emit("KERNEL", v("KERNEL_BASE"), v("KERNEL_SIZE"),
                 v("KERNEL_RAM_BASE"), v("KERNEL_RAM_SIZE"), out, ccm=ccm)
        elif target == "driver":
            base, size = image_layout(v("DRIVER_POOL_BASE"), v("DRIVER_POOL_SIZE"))
            emit("DRIVER", base, size,
                 v("DRIVER_RAM_BASE"), v("DRIVER_RAM_SIZE"), out,
                 stack_size=v("DRIVER_TASK_STACK_SIZE"))
        elif target == "app":
            base, size = image_layout(v("APP_SLOT0_BASE"), v("APP_SLOT0_SIZE"))
            emit("APP", base, size,
                 v("APP_RAM_BASE"), v("APP_RAM_SIZE"), out,
                 stack_size=v("APP_TASK_STACK_SIZE"))
        elif target == "boot":
            if v("BOOT_SIZE") <= 0:
                raise SystemExit("BOOT_SIZE 为 0，未划分 Bootloader 区（如需 Boot，请先在配置头中设置 BOOT_SIZE）")
            emit("BOOT", v("BOOT_BASE"), v("BOOT_SIZE"),
                 v("BOOT_RAM_BASE"), v("BOOT_RAM_SIZE"), out)
        else:
            raise SystemExit("未知 target: %s" % target)
    print("[gen_scatter] %-7s%-6s -> %s" % (target, " (raw)" if raw else "", out_path))'''

GS_CLI_OLD = """    ap.add_argument("--raw", action="store_true","""
GS_CLI_OLD2 = """    ap.add_argument("--check", action="store_true", help="仅校验布局")"""
GS_CLI_NEW2 = """    ap.add_argument("--raw", action="store_true",
                    help="生成开发调试用的裸镜像布局（负载直接位于槽位基址，无镜像头）")
    ap.add_argument("--check", action="store_true", help="仅校验布局")"""

GS_ALL_OLD = """    if args.target == "all":
        for t, name in (("kernel", "kernel.sct"), ("driver", "driver.sct"),
                        ("app", "app.sct")):
            gen_target(layout, t, os.path.join(args.output, name))
        if layout.v("BOOT_SIZE") > 0:
            gen_target(layout, "boot", os.path.join(args.output, "boot.sct"))
    else:
        gen_target(layout, args.target, args.output)
    return 0"""

GS_ALL_NEW = """    if args.target == "all":
        for t, name in (("kernel", "kernel.sct"), ("driver", "driver.sct"),
                        ("app", "app.sct")):
            gen_target(layout, t, os.path.join(args.output, name))
        # 开发调试旁路：同一份配置再生成一套“裸镜像”布局，
        # 供“固定地址烧录 + MDK 下断点调试 App”使用。
        gen_target(layout, "app", os.path.join(args.output, "app_dev.sct"), raw=True)
        if layout.v("DRIVER_POOL_SIZE") > 0:
            gen_target(layout, "driver", os.path.join(args.output, "driver_dev.sct"), raw=True)
        if layout.v("BOOT_SIZE") > 0:
            gen_target(layout, "boot", os.path.join(args.output, "boot.sct"))
    else:
        gen_target(layout, args.target, args.output, raw=args.raw)
    return 0"""


def patch_gen_scatter():
    d = Doc('tools/gen_scatter.py')
    d.sub(GS_DOC_OLD, GS_DOC_NEW, 'docstring 补充布局契约')
    d.sub(GS_GEN_OLD, GS_GEN_NEW, 'gen_target 支持镜像头偏移与 --raw')
    d.sub(GS_CLI_OLD2, GS_CLI_NEW2, '新增 --raw 选项')
    d.sub(GS_ALL_OLD, GS_ALL_NEW, 'target all 同时生成裸镜像布局')
    d.save()


# ================================================================ 3. pack_app
PA_DOC_OLD = """镜像格式见 kernelsrc/include/svcrt_app_image.h，CRC32 算法与内核
svcrt_loader.c 使用的 zlib/IEEE 802.3 反射多项式完全一致（可分段累积）。"""

PA_DOC_NEW = """镜像格式见 kernelsrc/include/svcrt_app_image.h，CRC32 算法与内核
svcrt_loader.c 使用的 zlib/IEEE 802.3 反射多项式完全一致（可分段累积）。

槽位内布局契约（与 tools/gen_scatter.py、内核 svcrt_loader.c 三方一致）：
    [槽位基址]  镜像头 256 字节
    [槽位基址 + 256]  负载（代码/只读数据）
所以 App / 驱动工程的 .sct 必须按「负载基址 = 槽位基址 + 镜像头长度」生成
（gen_scatter.py --target app|driver 的默认布局），本工具会据此校验链接基址。
镜像头里的 load_addr 记录的是「目标槽位基址」，不是负载基址。"""

PA_PACK_OLD = """    v = layout.v
    img_type = TYPE_APP if args.type == "app" else TYPE_DRIVER
    slot = SLOT_MACRO[args.type]
    slot_base = v(slot + "_BASE")
    slot_size = v(slot + "_SIZE")"""

PA_PACK_NEW = """    v = layout.v
    img_type = TYPE_APP if args.type == "app" else TYPE_DRIVER
    slot = SLOT_MACRO[args.type]
    slot_base = v(slot + "_BASE")
    slot_size = v(slot + "_SIZE")

    # 负载的链接基址 = 槽位基址 + 镜像头长度（与 gen_scatter.py 的默认布局一致）
    header_size = v("APP_IMAGE_HEADER_SIZE")
    if header_size != HEADER_SIZE:
        raise SystemExit("配置头 APP_IMAGE_HEADER_SIZE = %d 与打包工具的 HEADER_SIZE = %d 不一致"
                         % (header_size, HEADER_SIZE))
    payload_base = slot_base + header_size"""

PA_BIN_OLD = """        base = args.load_addr if args.load_addr is not None else slot_base"""
PA_BIN_NEW = """        base = args.load_addr if args.load_addr is not None else payload_base"""

PA_CHK_OLD = """    if base != slot_base:
        raise SystemExit("镜像基址 0x%08X 与分区配置中 %s_BASE = 0x%08X 不一致。\\n"
                         "        请确认该工程的 .sct 由 tools/gen_scatter.py 生成且配置头一致。"
                         % (base, slot, slot_base))"""

PA_CHK_NEW = """    if base != payload_base:
        raise SystemExit("镜像链接基址 0x%08X 与 %s_BASE + 镜像头 %d 字节 = 0x%08X 不一致。\\n"
                         "        该工程的 .sct 必须按“负载基址 = 槽位基址 + 镜像头长度”生成：\\n"
                         "        python tools/gen_scatter.py --target %s --output <out.sct>\\n"
                         "        （若确实要生成开发调试用的裸镜像，请用 --raw 布局并改用 --bin 打包）"
                         % (base, slot, header_size, payload_base, args.type))"""

PA_LEN_OLD = """    if len(payload) + HEADER_SIZE > slot_size:
        raise SystemExit("镜像过大：%d 字节 + 头 %d 超出槽位容量 %d 字节"
                         % (len(payload), HEADER_SIZE, slot_size))"""
PA_LEN_NEW = """    if header_size + len(payload) > slot_size:
        raise SystemExit("镜像过大：%d 字节 + 头 %d 超出槽位容量 %d 字节"
                         % (len(payload), header_size, slot_size))"""

PA_HDR_OLD = """    header = build_header(img_type, hw_compat, version, len(payload), entry_offset,
                          slot_base, 0, manifest)"""
PA_HDR_NEW = """    # 镜像头里的 load_addr = 目标槽位基址（内核据此校验镜像是否装到了正确的槽位）
    header = build_header(img_type, hw_compat, version, len(payload), entry_offset,
                          slot_base, 0, manifest)"""

PA_PRINT_OLD = """    print("[pack] 槽位基址    : 0x%08X（%s_BASE，容量 %d KB）" % (slot_base, slot, slot_size // 1024))"""
PA_PRINT_NEW = """    print("[pack] 槽位基址    : 0x%08X（%s_BASE，容量 %d KB）" % (slot_base, slot, slot_size // 1024))
    print("[pack] 负载基址    : 0x%08X（= 槽位基址 + 镜像头 %d 字节）" % (payload_base, header_size))"""

PA_INFO_OLD = """    print("入口地址 : 0x%08X" % (h["load_addr"] + h["entry_offset"]))"""
PA_INFO_NEW = """    print("负载基址 : 0x%08X" % (h["load_addr"] + HEADER_SIZE))
    print("入口地址 : 0x%08X" % (h["load_addr"] + HEADER_SIZE + h["entry_offset"]))"""


def patch_pack_app():
    d = Doc('tools/pack_app.py')
    d.sub(PA_DOC_OLD, PA_DOC_NEW, 'docstring 补充布局契约')
    d.sub(PA_PACK_OLD, PA_PACK_NEW, '计算负载基址')
    d.sub(PA_BIN_OLD, PA_BIN_NEW, 'bin 分支默认负载基址')
    d.sub(PA_CHK_OLD, PA_CHK_NEW, '链接基址校验')
    d.sub(PA_LEN_OLD, PA_LEN_NEW, '长度检查用配置值')
    d.sub(PA_HDR_OLD, PA_HDR_NEW, 'load_addr 语义注释')
    d.sub(PA_PRINT_OLD, PA_PRINT_NEW, '打印负载基址')
    d.sub(PA_INFO_OLD, PA_INFO_NEW, 'info 显示负载/入口地址')
    d.save()


# ================================================================ 4. loader.c
LD_OLD = """/* 计算镜像入口地址（ARM 需置 Thumb 位） */
static uint32 svcrt_loader_entry_addr(uint32 slot_base, uint32 entry_offset)
{
    uint32 entry = slot_base + entry_offset;"""

LD_NEW = """/* 计算镜像入口地址（ARM 需置 Thumb 位）
 * 槽位内布局为 [镜像头 256 字节][负载]，负载的链接基址 = 槽位基址 + SVCRT_APP_HEADER_SIZE，
 * entry_offset 是入口相对“负载起始”的偏移，所以入口 = 分区基址 + 头长 + entry_offset。
 * （裸镜像调试路径不经过本函数：它没有镜像头，入口直接取分区基址，
 *   见 svcrt_loader_identify 里的 APP_ALLOW_RAW_IMAGE 分支） */
static uint32 svcrt_loader_entry_addr(uint32 slot_base, uint32 entry_offset)
{
    uint32 entry = slot_base + SVCRT_APP_HEADER_SIZE + entry_offset;"""

LD_C_OLD = """ *   1) 安装路径：带 256 字节头 + CRC32 的 .svcapp，入口 = 基址 + entry_offset；"""

LD_C_NEW = """ *   1) 安装路径：带 256 字节头 + CRC32 的 .svcapp，
 *      入口 = 分区基址 + 镜像头长度 + entry_offset（负载链接基址另有 +256，见 gen_scatter）；"""


def patch_loader():
    d = Doc('kernelsrc/src/svcrt_loader.c')
    d.sub(LD_OLD, LD_NEW, '入口计算补镜像头长')
    d.sub(LD_C_OLD, LD_C_NEW, '注释更新')
    d.save()


if __name__ == '__main__':
    do_backup()
    print('--- config/svcrt_partition.h ---'); patch_config()
    print('--- tools/gen_scatter.py ---'); patch_gen_scatter()
    print('--- tools/pack_app.py ---'); patch_pack_app()
    print('--- kernelsrc/src/svcrt_loader.c ---'); patch_loader()
    print('OK')
