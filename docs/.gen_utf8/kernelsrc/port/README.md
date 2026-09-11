# SVCrtOS 移植层（port）架构说明与移植指南

本目录存放**架构相关**实现。内核（`kernelsrc/src/`、`kernelsrc/include/`）不包含任何架构指令、
不出现任何寄存器名称，也不感知任何栈帧布局；两者之间只有 `svcrt_hal.h` 一份接口契约。

## 1. 目录约定

```
kernelsrc/port/
├── README.md                       # 本文件
├── arm/
│   ├── cortex-m3/                  # svcrt_port.c + svcrt_context.S
│   └── cortex-m4/
├── riscv/                          # 规划中
│   ├── rv32imac/
│   └── rv64gc/
└── loongarch/                      # 规划中
    ├── la32/
    └── la64/
```

命名规则：`port/<架构族>/<具体核心>/`，与 `svcrt_arch.h` 中的
`SVCRT_ARCH_FAMILY_xxx` / `SVCRT_CPU_CORE_xxx` 一一对应。
一个工程**只能**加入一个核心目录的源文件（各核心实现同名函数）。

## 2. 编译期选择

在板级配置文件（`-DSVCRT_BOARD_CONFIG=...` 指向）中二选一：

```c
/* 方式一（推荐，跨架构）：直接指定核心 */
#define SVCRT_ARCH_CORE           SVCRT_CPU_CORE_RV32IMAC

/* 方式二（兼容旧工程，仅 Cortex-M）：0=M3 1=M4 2=M7 */
#define SVCRT_CPU_ARCH            SVCRT_ARCH_CORTEX_M4
```

`svcrt_arch.h` 会据此派生出：

| 派生宏 | 含义 |
|---|---|
| `SVCRT_ARCH_FAMILY` | 架构族（ARM / RISCV / LOONGARCH） |
| `SVCRT_ARCH_NAME` | 架构族字符串，便于日志与编译期诊断 |
| `SVCRT_ARCH_HAS_FPU / HAS_MPU / HAS_PRIV` | 能力默认值，供 `svcrt_config.h` 派生功能开关 |
| `SVCRT_ARCH_SVC_NUM_BITS` | 系统调用号位宽（8 = 指令立即数；-1 = 由寄存器传递） |
| `SVCRT_ARCH_SVC_ARG_MAX` | 系统调用参数个数上限（4） |
| `svcrt_arch_mpu_t` | 架构无关的 MPU 区域上下文容器 |

## 3. 内核与架构的四个耦合点（移植时只需实现这四类）

### 3.1 系统调用入口

内核只调用三个接口，绝不触碰栈帧字段：

```c
uint32 svcrt_port_syscall_num(void *p_exc_ctx);              /* 取系统调用号 */
uint32 svcrt_port_svc_get_arg(void *p_exc_ctx, uint32 idx);  /* 取第 idx 个参数 */
void   svcrt_port_svc_set_ret(void *p_exc_ctx, uint32 value);/* 写回返回值 */
```

内核侧通过宏 `SVCRT_SVC_NUM / SVCRT_SVC_ARG / SVCRT_SVC_RET` 使用它们，
`SVC_Server()` 的入参是不透明的 `void *`。

| 架构 | 实现要点 |
|---|---|
| Cortex-M | 入口由 `SVC_Handler` 汇编取 MSP/PSP 后跳入；硬件压栈顺序为 R0,R1,R2,R3,...，故 `((uint32*)ctx)[idx]` 即第 idx 个参数；系统调用号取自 SVC 指令的 8 位立即数 |
| RISC-V | `ecall` 无立即数，约定调用号放在寄存器（建议 `a7`），参数在 `a0~a3`，返回 `a0`；在 trap 入口保存的 trap 帧中按偏移取值 |
| LoongArch | `syscall` 指令携带 15 位 code 作为调用号，参数在 `$a0~$a3`（r4~r7），返回 `$a0` |

> 注意：由于 Cortex-M 的 SVC 号只有 8 位（0x00~0xFF），现行内核采用
> 「一个模块占一个 SVC 号 + 子命令放在第 0 个参数」的两级分发（见 `svcrt_def.h`），
> 该结构在 RISC-V / LoongArch 上同样成立，且可用空间更大。

### 3.2 上下文与任务切换

```c
uint32 svcrt_port_stack_init(uint32 stack_top, void (*entry)(void)); /* 构造初始栈帧 */
void   svcrt_port_switch_task(void);   /* 触发一次任务切换 */
void   svcrt_port_enter_idle(uint32 stack_ptr, uint32 use_priv);     /* 启动首个上下文 */
```

| 架构 | 实现要点 |
|---|---|
| Cortex-M | `svcrt_context.S` 中 PendSV 保存/恢复 R4-R11（FPU 使能时含 S16-S31）；`switch_task` 置位 `SCB->ICSR.PENDSVSET` |
| RISC-V | 任务切换通常在 `ecall`/`mret` 路径完成：保存 x1~x31、`mepc`、`mstatus` 到任务栈，`switch_task` 可置位软件中断（如 CLINT MSIP）触发切换 |
| LoongArch | 保存 $r1~$r31、`ERA`、`PRMD` 到任务栈；`switch_task` 可写 `ESTAT.IS` 触发软件中断 |

### 3.3 内存保护

```c
void svcrt_port_mpu_init(void);
void svcrt_port_mpu_set_region(uint32 rom_addr, uint32 rom_size, uint32 ram_addr, uint32 ram_size);
void svcrt_port_mpu_set_app(const svcrt_arch_mpu_t *p_mpu);  /* 任务切换时写回硬件 */
void svcrt_port_mpu_reset(void);
```

`svcrt_arch_mpu_t` 只约定两组寄存器语义：`region_base`（基址/地址寄存器）与
`region_attr`（属性/配置寄存器），最大条目数由 `SVCRT_MPU_REGION_MAX`（默认 8）决定。
内核 TCB 中每任务保存一份，切换时整块交给 port 写回。

| 架构 | 实现要点 |
|---|---|
| Cortex-M | 直接写 `MPU->RBAR / MPU->RASR`（现有实现每次写 4 个区域） |
| RISC-V | 映射到 PMP：`pmpaddr0..n` 与 `pmpcfg0..n`，注意 RV32 下 `pmpaddr` 为地址右移 2 位、配置寄存器每 8 位一项 |
| LoongArch | 映射到 TLB 表项 / 地址窗口（DMW、MTLB）配置，属性位与 ARM 语义不同，需在 port 内换算 |

### 3.4 时钟与临界区

```c
void   svcrt_port_start_timer(uint32 tick_period_us);   /* 启动节拍定时器 */
uint32 svcrt_port_get_timer_counter(void);              /* 节拍计数器当前值 */
uint32 svcrt_port_get_timer_reload(void);               /* 节拍计数器重载值 */
uint32 svcrt_port_get_system_clock(void);
uint32 svcrt_port_delay_us(uint32 us);                  /* 微秒级忙等 */

uint32 svcrt_port_enter_critical(void);                 /* 保存状态 + 关中断 */
void   svcrt_port_exit_critical(uint32 state);          /* 恢复状态 */
```

| 架构 | 实现要点 |
|---|---|
| Cortex-M | SysTick；临界区保存/恢复 `PRIMASK` |
| RISC-V | 机器模式定时器（mtime/mtimecmp 或 CLINT）；临界区保存/恢复 `mstatus.MIE` |
| LoongArch | 恒定频率定时器（TCFG/TVAL）；临界区保存/恢复 `CRMD.IE` |

## 4. 移植新架构的完整步骤

1. **登记核心**：在 `kernelsrc/include/svcrt_arch.h` 中增加核心宏（如 `SVCRT_CPU_CORE_RV32IMAC`）
   并补全能力派生分支（FPU / MPU / 特权级 / SVC 号位宽）。
2. **创建目录**：`kernelsrc/port/<族>/<核心>/`。
3. **实现 `svcrt_port.c`**：完整实现 `svcrt_hal.h` 中的全部非弱定义接口，
   重点是第 3 节的四类耦合点。
4. **实现上下文汇编**：参考 `arm/cortex-m4/svcrt_context.S`，
   完成「首次进入任务」「任务切换」「系统调用入口」三个入口。
5. **创建板级配置**：`board/<芯片>/svcrt_board_config.h`，至少设置
   `SVCRT_ARCH_CORE`、`SVCRT_SYSTEM_CLOCK_HZ`、共享内存地址与大小。
6. **创建板级实现与驱动**：`board/<芯片>/svcrt_board.c`，实现弱定义函数（板级初始化、
   中断初始化、空闲任务 MPU 配置）与中断入口（节拍中断调用 `svcrt_kernel_tick_handler()`）。
7. **工程配置**：把新核心目录的 `.c/.S` 加入工程源文件，头文件路径仍只需 `kernelsrc/include`
   与对应 `board/<芯片>`；预定义宏按第 2 节设置。
8. **内核零改动**：`kernelsrc/src` 与 `kernelsrc/include` 不应出现任何新增修改；
   若必须修改，说明抽象层存在泄漏，应优先补充 `svcrt_arch.h` / `svcrt_hal.h` 的公共接口。

## 5. 移植验收清单

- [ ] 最小构建：选定新核心后，`kernelsrc/src/*.c` 全部编译通过（无架构相关警告）
- [ ] 启动路径：空闲上下文能建立并切到第一个任务
- [ ] 上下文切换：两个任务交替运行，寄存器与栈内容正确
- [ ] 系统调用：任务能通过 SVC/ecall 完成 `svcrt_task_wait` 等调用并正确返回
- [ ] 节拍：`svcrt_get_time_ms()` 每毫秒稳定递增
- [ ] 同步原语：信号量、互斥锁、事件、消息队列、软定时器功能正常
- [ ] 内存保护：开启 `SVCRT_USE_MPU` 后，任务越界访问能触发保护异常
- [ ] 故障处理：触发一次 HardFault/等价异常，故障记录可被读出且任务能恢复
