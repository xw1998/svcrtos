# -*- coding: utf-8 -*-
"""
修复 15：MPU 路径中的确定缺陷（默认仍保持关闭）

背景：整块 MPU 隔离当前是“未接线”状态（板级 SVCRT_USE_MPU=0，整段代码不参与
编译）。这一批**不编造**隔离策略——那需要上板标定（数据窗口、外设放行、
MemManage 行为），静态检查证明不了。只修其中确定成立的两处缺陷，并把
“启用前必须补齐什么”写成可执行的清单，避免下一个人以为打开开关就能用。

缺陷1（未定义行为）：
  svcrt_port_mpu_set_region 用
      for(reg_idx = 4; reg_idx < 32; reg_idx++) if((1 << (reg_idx + 1)) >= size)
  找区域大小对应的 SIZE 域。当 size > 2^31（或循环走到底 reg_idx==31）时会出现
  `1 << 32`，对 32 位 int 是未定义行为；同样 `~((1 << (1 + rom_region)) - 1)`
  也有这个风险。改用专门的换算函数：上限封到 2GB、全用无符号移位、循环上界
  留出余量，任何输入都不会触发 32 位移位。

缺陷2（高编号区域残留）：
  svcrt_port_mpu_init / svcrt_port_mpu_reset 会清 0~7 共 8 个区域，
  但每次任务切换走的 svcrt_port_mpu_set_app 只写前 4 个区域。切到新任务时
  4~7 号区域保留上一次的配置，形成越权窗口。改为写入时同时清掉 4~7。
"""
import os
import shutil

ROOT = r'D:\工作\git_project\svcrtos_new'
BK = r'C:\Users\14905\OneDrive\Documents\lingxi-claw\20260911-13-47-19-445\backup_fix6'

FILES = [
    'kernelsrc/port/arm/cortex-m3/svcrt_port.c',
    'kernelsrc/port/arm/cortex-m4/svcrt_port.c',
    'kernelsrc/src/svcrt_mpu.c',
]


def detect_enc(raw):
    try:
        raw.decode('utf-8')
        return 'utf-8'
    except UnicodeDecodeError:
        return 'gbk'


class Doc(object):
    def __init__(self, rel):
        self.rel = rel
        self.p = os.path.join(ROOT, rel.replace('/', os.sep))
        self.raw = open(self.p, 'rb').read()
        self.enc = detect_enc(self.raw)

    def sub(self, old, new, label, count=1, all=False):
        for eol in ('\r\n', '\n'):
            o = old.replace('\n', eol).encode(self.enc)
            n = new.replace('\n', eol).encode(self.enc)
            hit = self.raw.count(o)
            if all:
                if hit > 0:
                    self.raw = self.raw.replace(o, n)
                    print('  ok(%s)x%d: %s' % (repr(eol), hit, label))
                    return
            elif hit == count:
                self.raw = self.raw.replace(o, n)
                print('  ok(%s): %s' % (repr(eol), label))
                return
        raise AssertionError('[%s/%s] %s: 未找到匹配' % (self.rel, self.enc, label))

    def save(self):
        open(self.p, 'wb').write(self.raw)


# ---------------------------------------------- 区域大小换算函数（插在 set_region 前）
SIZE_HELPER_OLD = """void svcrt_port_mpu_set_region(uint32 rom_addr, uint32 rom_size, uint32 ram_addr, uint32 ram_size)
{"""

SIZE_HELPER_NEW = """/* 换算覆盖 size 字节所需的最小 MPU 区域对应的 SIZE 域值（RASR bits[5:1]）。
 * MPU 区域大小必须是 2 的幂且不小于 32 字节，SIZE = log2(区域大小) - 1。
 * 原实现用 for(reg_idx = 4; reg_idx < 32; reg_idx++)
 *   if((1 << (reg_idx + 1)) >= size)
 * 来找这个值：size > 2^31 时会走到 reg_idx == 31，算出 `1 << 32`，
 * 对 32 位 int 是未定义行为。这里改为：
 *   - 入参先封顶到 2GB（32 位地址空间的一半，实际工程不可能超过）；
 *   - 全程无符号移位，移位量最大 31；
 *   - 循环上界留出余量，任何输入都不会产生 32 位移位。 */
static uint32 svcrt_mpu_size_field(uint32 size)
{
    uint32 field = 4u;                  /* 最小区域 32 字节 */

    if(size > 0x80000000u)
    {
        size = 0x80000000u;
    }

    while((field < 30u) && ((1u << (field + 1u)) < size))
    {
        field++;
    }

    return field;
}

void svcrt_port_mpu_set_region(uint32 rom_addr, uint32 rom_size, uint32 ram_addr, uint32 ram_size)
{"""

# ---------------------------------------------------------------- set_region 主体
REGION_OLD = """    uint32 ram_asr = 0x12060001;
    uint32 rom_asr = 0x06020001;
    int32  reg_idx;
    uint8  rom_region = 31;
    uint8  ram_region = 31;

    if(MPU->TYPE == 0)
    {
        return;
    }

    for(reg_idx = 4; reg_idx < 32; reg_idx++)
    {
        if((1 << (reg_idx + 1)) >= rom_size)
        {
            rom_region = reg_idx;
            break;
        }
    }

    for(reg_idx = 4; reg_idx < 32; reg_idx++)
    {
        if((1 << (reg_idx + 1)) >= ram_size)
        {
            ram_region = reg_idx;
            break;
        }
    }

    rom_addr = rom_addr & ~((1 << (1 + rom_region)) - 1);
    ram_addr = ram_addr & ~((1 << (1 + ram_region)) - 1);
"""

REGION_NEW = """    uint32 ram_asr = 0x12060001;
    uint32 rom_asr = 0x06020001;
    uint32 rom_region;
    uint32 ram_region;

    if(MPU->TYPE == 0)
    {
        return;
    }

    rom_region = svcrt_mpu_size_field(rom_size);
    ram_region = svcrt_mpu_size_field(ram_size);

    /* 区域基址必须按区域大小对齐：向下取整到 2^(rom_region+1) 的边界。
     * svcrt_mpu_size_field 保证 rom_region/ram_region <= 30，
     * 因此这里的移位量最大 31，不会出现 1<<32。 */
    rom_addr = rom_addr & ~((1u << (1u + rom_region)) - 1u);
    ram_addr = ram_addr & ~((1u << (1u + ram_region)) - 1u);
"""

# ------------------------------------------------------------------ set_app 清高位
SETAPP_OLD = """    SVCRT_DMB();
    MPU->CTRL = 0;

    for(rnr = 0; rnr < 4; rnr++)
    {
        MPU->RNR  = rnr;
        MPU->RBAR = p_mpu->region_base[rnr];
        MPU->RASR = p_mpu->region_attr[rnr];
    }

    MPU->CTRL = MPU_CTRL_ENABLE_Msk | MPU_CTRL_PRIVDEFENA_Msk;
    SVCRT_DSB();
    SVCRT_ISB();
}"""

SETAPP_NEW = """    SVCRT_DMB();
    MPU->CTRL = 0;

    /* init/reset 清的是 0~7 共 8 个区域，切换时只写前 4 个会留下残影：
     * 4~7 号区域仍保留上一个任务的配置，形成越权窗口。这里一并清掉。 */
    for(rnr = 0; rnr < 8; rnr++)
    {
        MPU->RNR = rnr;

        if(rnr < (int32)SVCRT_MPU_REGION_MAX)
        {
            MPU->RBAR = p_mpu->region_base[rnr];
            MPU->RASR = p_mpu->region_attr[rnr];
        }
        else
        {
            MPU->RBAR = 0;
            MPU->RASR = 0;
        }
    }

    MPU->CTRL = MPU_CTRL_ENABLE_Msk | MPU_CTRL_PRIVDEFENA_Msk;
    SVCRT_DSB();
    SVCRT_ISB();
}"""

# ------------------------------------------------------- svcrt_mpu.c 启用前清单
WARN_OLD = """* @warning 打开隔离（SVCRT_USE_MPU=1）之前，必须为每个任务填充
*          TCB 里的 MPU 区域上下文（svcrt_task_t.mpu，见 svcrt_task.h）：
*          当前 svcrt_task_register() 只把它清零，区域全 0 = 全部禁用，
*          非特权任务访问任何地址都会触发 MemManage。
*          需要连同"任务代码区 / 数据区 / 是否放行外设区"的权限策略一起确定。"""

WARN_NEW = """* @warning 【未完成：打开开关之前必须先补齐下面 4 项】
*          当前板级配置是 SVCRT_USE_MPU=0，本文件整段不参与编译，
*          也就是说下面这些空白**没有被任何一次构建覆盖过**。
*
*          1) 任务 MPU 上下文无人填充。svcrt_task_register() 只把 TCB.mpu
*             清零（区域全 0 = 全部禁用），而上下文切换处
*             （svcrt_task.c 的 svcrt_mpu_set_app 调用）会把这份空上下文
*             原样写进硬件 —— 非特权任务访问任何地址都会触发 MemManage。
*             启用前需要在这里补一个 build 函数，按“代码区 + 数据区 +
*             外设区放行”生成区域表，并在 svcrt_task_register() 与
*             loader 装载任务处调用。
*
*          2) 数据区不能直接用 TCB.ram_start/ram_size。那两项记录的是任务栈
*             （栈底指针 + 字节数），既不等于任务的全局变量区，大小也不是
*             2 的幂、基址也未必对齐，而 MPU 区域要求二者都满足。
*             正确来源是分区配置：App 的 RW/ZI 由 .sct 放在 APP_RAM_BASE
*             起的整个 APP_RAM 窗口里，驱动同理放在 DRIVER_RAM。
*
*          3) 外设区（0x40000000 起）需要单列策略。非特权任务要访问外设
*             寄存器必须显式放行，放行范围与权限（只读/读写）按板子确定，
*             不能沿用内核的 PRIVDEFENA。
*
*          4) 以上三点都必须上板标定：MemManage 的实际触发点、区域边界、
*             外设放行范围，静态检查一个都证明不了。
*
*          因此：保持默认关闭。补齐并通过上板验证之前，不要打开
*          SVCRT_USE_MPU。"""


def main():
    os.makedirs(BK, exist_ok=True)
    for rel in FILES:
        src = os.path.join(ROOT, rel.replace('/', os.sep))
        shutil.copy2(src, os.path.join(BK, rel.replace('/', '__')))
    print('备份完成 -> %s' % BK)

    for rel in ('kernelsrc/port/arm/cortex-m3/svcrt_port.c',
                'kernelsrc/port/arm/cortex-m4/svcrt_port.c'):
        d = Doc(rel)
        d.sub(SIZE_HELPER_OLD, SIZE_HELPER_NEW, '新增区域大小换算函数（消除 1<<32 UB）')
        d.sub(REGION_OLD, REGION_NEW, 'set_region 改用换算函数')
        d.sub(SETAPP_OLD, SETAPP_NEW, 'set_app 同时清 4~7 号区域')
        d.save()

    d = Doc('kernelsrc/src/svcrt_mpu.c')
    d.sub(WARN_OLD, WARN_NEW, '启用前必须补齐的清单')
    d.save()

    print('OK')


if __name__ == '__main__':
    main()
