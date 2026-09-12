# -*- coding: utf-8 -*-
"""
Round-15 documentation update.

1) docs/死代码与未接线审计.md
   - refresh 3.1 / 3.2 / 3.3 / 3.4 / 3.5 so they describe the current tree
   - append section 7 (fifth round: what changed, what is verified, what is not)
2) new doc: docs/安装协议与上板标定清单.md  (ACK protocol + MPU on-board checklist)
3) docs/README.md: add the new doc to the index
"""
import os, sys

REPO = r"D:/工作/git_project/svcrtos_new"
DOCS = os.path.join(REPO, 'docs')
AUDIT = os.path.join(DOCS, '死代码与未接线审计.md')
NEWDOC = os.path.join(DOCS, '安装协议与上板标定清单.md')
README = os.path.join(DOCS, 'README.md')


def read(p):
    b = open(p, 'rb').read()
    enc = 'utf-8'
    try:
        b.decode('utf-8')
    except UnicodeDecodeError:
        enc = 'gbk'
    t = b.decode(enc)
    return t, enc, ('\r\n' if '\r\n' in t else '\n')


def write(p, text, enc):
    open(p, 'wb').write(text.encode(enc))


def replace_once(text, old, new, label):
    if text.count(old) != 1:
        print('!! anchor %s count=%d' % (label, text.count(old)))
        return text, False
    return text.replace(old, new, 1), True


def main():
    ok = True
    text, enc, eol = read(AUDIT)

    # ---- 3.1 MPU -----------------------------------------------------------
    text, r = replace_once(text,
        "### 3.1 隔离（MPU / 非特权）当前是关闭的",
        "### 3.1 隔离（MPU / 非特权）代码路径已完整，开关仍是关的\n\n"
        "> 第五轮更新：区域构建、端口编码、任务切换重放已全部接好（见第七节 7.1）。\n"
        "> 板级 `SVCRT_USE_MPU` / `SVCRT_USE_PRIV` **仍为 0**，原因不再是\"没写\"，\n"
        "> 而是\"没有硬件标定\"——打开它之前必须先按第 7.4 节的清单在板上量一遍。",
        '3.1')
    ok &= r

    # ---- 3.2 spinlock ------------------------------------------------------
    text, r = replace_once(text,
        "### 3.2 自旋锁整套是未接线状态",
        "### 3.2 自旋锁已接线（第五轮）\n\n"
        "> `svcrt_ptable_set_slot()` 与 `svcrt_ptable_get_slot()` 已用自旋锁保护槽位三元组，\n"
        "> `svcrt_loader_start()` 改为原子读取 state/entry，`SVCRT_SPINLOCK_INIT` 的 owner\n"
        "> 初值修回 `0xffffffff`。下面这段记录的是**修复前**的状态，保留作背景。",
        '3.2')
    ok &= r

    # ---- 3.3 dead code list is stale ---------------------------------------
    text, r = replace_once(text,
        "### 3.3 零引用的函数清单（内核 / 板级）",
        "### 3.3 零引用的函数清单（内核 / 板级）\n\n"
        "> 第五轮事实核对：下表前三行**在当前源码树里已经不存在**——`svcrt_sched_switch()`、\n"
        "> `svcrt_fault_record_read_internal()`、`svcrt_port_enable_fpu()` 已经在前几轮被清掉，\n"
        "> 它们只残留在陈旧的 `docs/.gen_utf8/` 副本里。也就是说本表**已经滞后于代码**，\n"
        "> 读它之前请先跑一遍 `patch/deadcode_audit.py` 重新生成。",
        '3.3')
    ok &= r

    # ---- 3.4 stale copy removed -------------------------------------------
    text, r = replace_once(text,
        "### 3.4 仓库里仍有一份陈旧副本",
        "### 3.4 陈旧副本已删除（提交 96d8b4f）",
        '3.4')
    ok &= r
    text, r = replace_once(text,
        "- 它是早期版本的 `svcrt_board.c` / `svcrt_init.c` 混合体（内容是老代码）；\n"
        "- **不在任何 `.uvprojx` 的文件列表里，也不在 include 路径里**，所以不影响真实构建；\n"
        "- 但会被 IDE / 全局搜索 / 后续维护者误读，属于上一轮\"清地雷文件\"的遗漏。",
        "- 它曾是早期版本的 `svcrt_board.c` / `svcrt_init.c` 混合体（内容是老代码）；\n"
        "- 它从来不在任何 `.uvprojx` 的文件列表里，也不在 include 路径里，所以没有影响真实构建；\n"
        "- 但会被 IDE / 全局搜索 / 后续维护者误读，属于\"清地雷文件\"的遗漏。\n"
        "- **第五轮已确认该目录已从工作树中删除**，无需再处理。",
        '3.4 body')
    ok &= r

    # ---- 3.5 audit blind spot changed --------------------------------------
    text, r = replace_once(text,
        "`deadcode_audit.py` 编不进两个文件，因此结果里由它们引用的符号会被误报：\n\n"
        "- `kernelsrc/sdk/app_sdk/svcrt_oslib.c`：含 `__svc`，需要 AC5 才能编；\n"
        "- `kernelsrc/port/arm/cortex-m4/svcrt_context.S`：ARM 汇编语法，需要 armasm。",
        "`deadcode_audit.py` 编不进两个文件，因此结果里由它们引用的符号会被误报：\n\n"
        "- `kernelsrc/sdk/app_sdk/svcrt_oslib.c`：历史上含 `__svc`，需要 AC5 才能编；\n"
        "  **第五轮起该文件在 AC5 / AC6 下都能编**（见 7.1），这个盲区可以消掉了；\n"
        "- `kernelsrc/port/arm/cortex-m4/svcrt_context.S`：ARM 汇编语法，需要 armasm。\n"
        "  实测 AC6 用 `armclang -masm=armasm` 也能汇编，只有 A1950W 弃用告警。",
        '3.5')
    ok &= r

    # ---- 6.2 no longer fully true -----------------------------------------
    text, r = replace_once(text,
        "- 5.2 列出的其余缺口（MPU 四项、串口 ORE、自旋锁未接线、死代码清单、陈旧副本、\n"
        "  git 索引跟踪构建产物、AC5→AC6）**全部依旧**。",
        "- 5.2 列出的其余缺口（MPU 四项、串口 ORE、自旋锁未接线、死代码清单、陈旧副本、\n"
        "  git 索引跟踪构建产物、AC5→AC6）**当时全部依旧**——其中除死代码清单外，\n"
        "  **已在第五轮处理，见第七节**。",
        '6.2')
    ok &= r

    text = text.rstrip() + eol + eol + SECTION7.replace('\n', eol)
    write(AUDIT, text, enc)
    print('OK  审计文档已刷新并追加第七节')

    newdoc = NEWDOC_CONTENT.replace('\n', eol)
    write(NEWDOC, newdoc, 'utf-8')
    print('OK  新建', os.path.basename(NEWDOC))

    rd, renc, reol = read(README)
    if '安装协议与上板标定清单' in rd:
        print('    README 已含该条目，跳过')
    else:
        anchor = '| 文档 | 内容 |'
        i = rd.find(anchor)
        if i < 0:
            print('!! README 索引锚点未找到')
            ok = False
        else:
            j = rd.find('\n', i + len(anchor))
            row = reol + '| [安装协议与上板标定清单](安装协议与上板标定清单.md) | .svcapp 传输 ACK 流控协议；打开 MPU 前必须做的上板标定清单 |'
            rd = rd[:j] + row + rd[j:]
            write(README, rd, renc)
            print('OK  README 索引已补一行')
    return 0 if ok else 1


SECTION7 = """## 七、第五轮补齐（2026-09-12，提交 369fd34）

这一轮的目标是"把上一轮没闭环的收掉，并且把注释改成不会再有编码风险的写法"。

### 7.1 已改并验证

| 项 | 做法 | 验证到哪一层 |
|----|------|-------------|
| **AC5 → AC6 就绪** | 新增 `kernelsrc/include/svcrt_svc_call.h`。AC5 下 `SVCRT_SVC_DECL_*` 展开成与原来完全一致的 `__svc(n)` 声明；AC6/GCC/Clang 下展开成 `static inline` 包装，用内联汇编发 `svc #n`，参数用显式寄存器变量钉在 r0/r1/r2，返回值从 r0 取回。改造 16 处声明：`svcrt_oslib.c`(11)、`svcrt_drv_oslib.c`(4)、`svcrt_driver_sdk.h`(1) | AC5 `armcc` 与 AC6 `armclang` **都做了语法校验并通过**；UV4 重建 BLED_APP / BLED_DRV 0 Error 0 Warning |
| **驱动示例注释** | 5 个 `example_{i2c,adc,gpio,spi,uart}_drv.c` 的注释早已被破坏成 `?`（中文全部丢失），按脚本重写为英文，**逐文件比对"剥离注释后的代码必须完全一致"** | 5 个文件代码 100% 一致；全仓 `??` 扫描残留 = 0 |
| **端口汇编注释** | `kernelsrc/port/arm/cortex-m4/svcrt_context.S` 同样被破坏（13 处 `?`），按行重写为英文，与 M3 端口对齐 | 只动注释行；AC6 `armclang -masm=armasm` 可汇编 |
| **宏重定义告警** | `svcrt_board_config.h` 里 `APP_ALLOW_RAW_IMAGE` 漏了 `#undef`，与 `config/svcrt_partition.h` 的默认定义冲突，产生 `-Wmacro-redefined` | AC6 复检该告警消失 |
| **MPU 隔离** | `svcrt_mpu.c/h` 重写：区域窗口全部由 `config/svcrt_partition.h` 派生；AP 位修回 `0x13060001`（数据区非特权可读写）；新增 `svcrt_port_mpu_encode()` / `svcrt_port_mpu_set_idle()`；cfg / loader / task 三处接线 | AC6 默认配置与强制 `SVCRT_USE_MPU=1` 两种配置都语法校验通过 |
| **串口 ACK 流控** | `svcrt_loader_stream_payload()` 头写入后回 ACK，之后每 512B 块回 ACK；新增 `tools/send_image.py`（pyserial，`--no-ack` 保持旧行为） | 语法校验通过；脚本语法检查通过 |
| **ptable 自旋锁** | 槽位三元组加锁保护，新增 `svcrt_ptable_get_slot()`，`svcrt_loader_start()` 原子读取 state/entry | AC5/AC6 校验 + 全量链接通过 |
| **仓库卫生** | `docs/.gen_utf8/**` 与各工程 `MDK-ARM` 下的构建产物共 278 个文件 `git rm --cached`（**只移出索引，本地文件保留**）；`.uvprojx/.uvoptx` 全部保留在索引里 | `git ls-files -i -c --exclude-standard` 在上述目录下已归零 |

**编码约定（本轮新增，此后一律遵守）**：新写与重写的注释统一用英文（ASCII）。
纯 ASCII 文本在 GBK 和 UTF-8 下字节完全相同，因此后续补丁脚本不会再因为编码判断出错而
把中文写成 `?`——本轮修掉的 6 个文件正是历史上被这样写坏的。

### 7.2 本轮**没有**改、仍然存在的缺口

- **死代码清单没有重新生成**。3.3 那张表已确认滞后（前三行在源码树里已不存在），
  要得到准确清单需要在当前树上重跑 `patch/deadcode_audit.py`，本轮没有重跑。
- **自旋锁只在 `svcrt_ptable` 一处使用**。`SVCRT_USE_SPINLOCK` 的整体设计意图
  （多核 / 跨核互斥）仍然只是一个预留能力，没有第二个使用点。
- **驱动示例仍是骨架**。5 个示例里的 `TODO(chip)` 位置全部未实现，它们本来就是模板文件。
- **`tools/send_image.py` 未与任何固件端联调**，主机侧与设备侧只对齐了纸面协议。

### 7.3 本轮未做的验证（不要当成已验证）

- **仍未上板**。MPU 隔离、ACK 流控、自旋锁、HAL 时基这四项**一行硬件证据都没有**，
  证据链只有"AC5 + AC6 语法校验"和"UV4 全量链接 0 Error 0 Warning"两层。
- 内联汇编版 SVC 包装（AC6 分支）**从未在真实 AC6 工程里生成过机器码并运行**，
  只做了语法校验；`svc #n` 的立即数编码与 AC5 `__svc` 是否真的一致，
  依赖的是同一套 Thumb 编码规则，不是实测。
- BLED_APP / BLED_DRV 的 `Program Size` 只有 272 / 608 字节，说明它们链接的是
  **骨架应用/驱动**，不代表真实负载下的行为。

### 7.4 打开 MPU 之前必须做的事

见 [安装协议与上板标定清单](安装协议与上板标定清单.md)。核心是三件：
先确认 MemManage 真的会触发、再确认放行窗口够用、最后才把 `SVCRT_USE_MPU` 置 1。
"""


NEWDOC_CONTENT = """# SVCrtOS 安装协议与上板标定清单

本文记录两件**只能在硬件上完成**的事，代码侧已经就位，但没有任何实测数据：

1. `.svcapp` 镜像传输的 **ACK 流控协议**（设备侧 / 主机侧各自要做什么）；
2. 打开 MPU 隔离（`SVCRT_USE_MPU = 1`）之前必须走一遍的**上板标定清单**。

> 诚实声明：本文描述的是**设计意图与待测项**，不是"已验证的结论"。
> 截至提交 369fd34，MPU 与 ACK 流控都只通过了语法校验与链接，
> **从未在硬件上运行过**。

---

## 一、为什么需要 ACK 流控

### 1.1 问题：flash 擦写期间 CPU 取指停摆

`.svcapp` 是先写进 App 槽位、再启动的，所以传输过程中必然穿插 flash 擦除与编程。

STM32F4 在 flash 擦除 / 编程期间：

- 总线对 CPU 取指是**阻塞**的，CPU 无法执行任何指令，中断服务程序也不会被取到；
- 但 UART 外设仍在接收，硬件 FIFO 只有 1 字节；
- 因此主机在这段时间里继续灌数据，就会触发 **ORE（Overrun Error）**，字节直接丢失。

这不是软件写得不好，是这块芯片的物理限制：**设备端没有任何办法把这段窗口里的数据收下来**。
唯一的出路是让主机**在设备忙的时候不要发**。

### 1.2 协议

设备端（`svcrt_loader_stream_payload()`）：

```
主机                                设备
 |  ---- 256B 镜像头 ----------->   |  校验头（magic / 长度 / CRC）
 |  <----------- ACK(0x06) -------  |  头校验通过
 |  ---- 512B 数据块 ----------->   |
 |                                 |  擦除 + 编程（CPU 停摆，收不到任何字节）
 |  <----------- ACK(0x06) -------  |  本块写成功
 |  ---- 512B 数据块 ----------->   |
 |            ...                   |
 |  <----------- 最终结果 ---------  |  整镜像 CRC 校验结果
```

要点：

- **ACK 是单字节 `0x06`**，只在"上一段已经落盘"之后发出；
- 主机**收到 ACK 才发下一段**，因此线上的数据流天然与设备忙窗口错开；
- 头也要 ACK：头校验失败时设备直接回错误码结束，主机不必再灌 512KB；
- 超时策略由主机决定；`tools/send_image.py` 里的做法是等待 ACK 超时即中止并报错，
  不做静默重传（重传会把半写状态搞复杂）。

### 1.3 兼容旧行为

`tools/send_image.py --no-ack` 关闭节流，退回"一头灌到底"的老行为。
用途只有一个：**验证"没有 ACK 的旧主机"在什么数据量下会丢字节**。
正常使用请**不要**加这个参数。

### 1.4 上板待测项

- [ ] 大镜像（接近 `APP_SLOT_SIZE`）连续传输，统计丢字节数 = 0；
- [ ] 人为把 UART 波特率提到 921600，确认仍不丢（ACK 有往返延迟，可能反而更慢，
      需要测出"不丢字节前提下的最高可用波特率"）；
- [ ] ACK 超时判定的合理阈值（当前是主机侧拍脑袋定的，需要用实测往返时间收敛）；
- [ ] 传输中途拔线 / 复位，确认设备端不会把半写镜像当成可运行镜像启动。

---

## 二、打开 MPU 之前的标定清单

`SVCRT_USE_MPU` 与 `SVCRT_USE_PRIV` 在 `board/stm32f427/svcrt_board_config.h` 里
都是 0。**代码路径已经完整**，之所以不打开，是因为下面每一项都需要硬件实测，
静态推理不足以保证不把系统打死：

### 2.1 先确认 MemManage 真的会触发

- [ ] 把 `SVCRT_USE_MPU` 置 1，App 任务里故意写一段越界地址，确认进的是
      MemManage（不是 HardFault，也不是"什么都没发生"）；
- [ ] 确认 `svcrt_fault` 记录里能读到这次违规（type / task_id / tick）；
- [ ] 确认"重启 3 次崩溃禁用"的策略真的在 MemManage 路径上生效。

> 如果这一步拿不到预期结果，**不要往下走**——说明区域编码或异常优先级有问题。

### 2.2 再确认放行窗口够用

设备端的外设访问窗口定义在 `config/svcrt_partition.h`（派生宏）并被板级
`svcrt_board_config.h` 覆盖成：

| 窗口 | 基址 | 大小 | 用途 |
|------|------|------|------|
| `SVCRT_MPU_PERIPH_BASE` | `0x40000000` | 512K | APB1 / APB2 / AHB1 |
| `SVCRT_MPU_PERIPH2_BASE` | `0x50000000` | 512K | AHB2（USB OTG FS/HS） |

- [ ] 逐个确认 App / Driver 实际用到的外设都落在这两个窗口里（尤其是 **AHB2 上的
      USB OTG**，以及任何落到 `0x60000000`（FMC）或 `0xE0000000`（内核私有）的访问）；
- [ ] 确认窗口大小同时满足"是 2 的幂"和"基址按大小对齐"——这两条已有编译期断言，
      但**窗口内容是否够用是不能静态断言的**；
- [ ] 确认放行属性：数据区必须是"特权读写 + 非特权读写"（AP = `0b011`），
      代码区是"特权读写 + 非特权只读"（AP = `0b110`）。
      历史上这里被改反过一次（改成 `0b010`），会导致非特权任务写自己的 RAM 就进
      MemManage——上板时请先复现"任务能正常写自己的栈"再做别的。

### 2.3 最后才打开

- [ ] 顺序固定：**先只开 MPU（`SVCRT_USE_MPU=1`，`SVCRT_USE_PRIV=0`）**，
      确认原有关闭 MPU 时的行为完全不变；
- [ ] 再开 `SVCRT_USE_PRIV=1`，把任务真正降为非特权，观察首次违规点；
- [ ] 记录每一处"非特权任务被拦下来"的位置，逐个判断是"窗口没配全"
      还是"任务真的越权了"；
- [ ] 以上全部通过后，才把板级默认值改成 1 并提交。

### 2.4 已知的、无法在这里解决的边界

- MPU 区域数量（ARMv7-M 是 8 个）是硬约束，窗口策略再改也不能超过它；
- 内核任务（`KERNEL_RAM_SIZE = 88K`，不是 2 的幂）的 RAM 窗口退化成
  "CHIP_RAM 向上取 2 的幂"，因此内核任务实际拿到的可访问范围**比它自己的 RAM 大**。
  这是有意的取舍（内核是可信代码），但要知道它存在。
"""


if __name__ == '__main__':
    sys.exit(main())
