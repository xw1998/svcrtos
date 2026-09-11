# SVCrtOS

SVCrtOS 是一个面向 ARM Cortex-M 系列微控制器的安全实时操作系统（RTOS），采用 SVC（Supervisor Call）实现特权级隔离，利用 MPU 进行内存保护，支持多任务分区调度和统一设备驱动框架。

## 核心特性

- **SVC 特权隔离**：用户任务通过 SVC 系统调用进入内核态，内核态代码受硬件保护，用户任务无法直接访问
- **MPU 内存保护**：利用 Cortex-M MPU 实现任务间 ROM/RAM 空间隔离，防止越界访问
- **优先级 + 时间片调度**：支持多优先级抢占和同优先级时间片轮转
- **事件驱动机制**：轻量级事件同步原语，支持超时等待
- **信号量与互斥锁**：计数信号量 + 带优先级继承的互斥锁，防止优先级反转
- **设备驱动框架**：统一设备 I/O 接口（open/close/read/write/ctrl），支持内置驱动
- **独立驱动固件**：驱动可编译为完全独立的固件烧录到专属 ROM 分区，与内核解耦，支持独立升级；App 仅凭设备名即可访问，无需知道驱动实现或地址
- **FIFO 缓冲区**：环形缓冲区，用于内核和驱动间的数据传输
- **栈溢出检测**：通过 PSP 越界检测任务栈溢出
- **任务栈用量分析**：栈填充 + 最低栈指针双手段统计每任务峰值用量，为裁剪栈空间提供依据
- **自旋锁与调度器锁**：内核/驱动侧自旋锁（原子 CAS + 关中断变体，面向 SMP 扩展），用户态调度器锁（禁止任务切换的轻量临界区）
- **CPU 负载统计**：实时统计 CPU 空闲率
- **FPU 支持**：Cortex-M4F/M7 浮点寄存器（S16-S31）按需自动保存/恢复
- **双 SDK 架构**：独立的应用 SDK 和驱动 SDK，支持编译为独立分区固件
- **内核与芯片解耦**：采用 RT-Thread 类似的分层架构，内核零芯片依赖，移植只需修改 board/ 目录

## 支持 CPU 架构

| 架构 | FPU | MPU | 移植层状态 |
|------|-----|-----|------|
| Cortex-M3 | - | 有 | 已实现 |
| Cortex-M4 | 有 | 有 | 已实现（推荐） |
| Cortex-M7 | 有 | 有 | 已实现（复用 M4 移植层约定） |
| RISC-V（RV32/RV64） | 按核心 | PMP | 抽象层已就绪，移植实现待补 |
| LoongArch（LA32/LA64） | 按核心 | TLB/地址窗口 | 抽象层已就绪，移植实现待补 |

### 架构抽象层

内核与指令集之间只有一层契约，全部收敛在 `kernelsrc/include/svcrt_arch.h`（架构描述与派生能力）
与 `svcrt_hal.h`（端口接口声明）两个头文件中：

1. **两级架构描述**：`SVCRT_ARCH_FAMILY_xxx`（架构族）+ `SVCRT_CPU_CORE_xxx`（具体核心），
   由 `SVCRT_ARCH_CORE` 或兼容旧工程的 `SVCRT_CPU_ARCH` 选择；FPU / MPU / 特权级 / SVC 号位宽
   等能力默认值由核心自动派生
2. **系统调用上下文不透明**：内核 `SVC_Server(void *)` 只通过
   `SVCRT_SVC_NUM / SVCRT_SVC_ARG / SVCRT_SVC_RET` 三个宏访问调用号、参数与返回值，
   栈帧布局完全由 port 层解析（Cortex-M 用硬件压栈帧，RISC-V / LoongArch 用各自 trap 帧）
3. **上下文切换三接口**：`svcrt_port_stack_init()`（栈帧初始化）、
   `svcrt_port_switch_task()`（触发切换）、`svcrt_port_enter_idle()`（启动首个上下文）
4. **内存保护统一描述**：`svcrt_arch_mpu_t`（`region_base` / `region_attr` 两组寄存器）
   承载每任务的隔离上下文，Cortex-M 映射到 `RBAR/RASR`，RISC-V 映射到 `pmpaddr/pmpcfg`，
   LoongArch 映射到 TLB / 地址窗口，内核不感知具体机制
5. **临界区状态化**：`svcrt_port_enter_critical()/exit_critical()` 保存并恢复中断状态、支持嵌套，
   适配需要保存 `mstatus.MIE`（RISC-V）、`CRMD.IE`（LoongArch）的架构
6. **时钟统一命名**：`svcrt_port_get_timer_counter()/get_timer_reload()` 屏蔽
   SysTick / mtime / 恒定频率定时器差异

新增架构只需在 `kernelsrc/port/<族>/<核心>/` 下实现上述接口，**内核源码零改动**；
完整步骤与验收清单见 [移植层说明](kernelsrc/port/README.md)。

## 目录结构

```
SVCRTOS/
├── kernelsrc/                      # ★ 内核源码（零芯片依赖）
│   ├── include/                    # 内核头文件
│   │   ├── svcrt.h                 # 应用 API
│   │   ├── svcrt_arch.h            # ★ 架构抽象层（架构族/核心/能力派生/MPU 上下文）
│   │   ├── svcrt_config.h          # 集中配置（可被板级配置覆盖）
│   │   ├── svcrt_port.h            # 硬件抽象接口（纯声明，无芯片依赖）
│   │   ├── svcrt_types.h           # 基础类型定义
│   │   └── ...                     # 其他内核内部头文件
│   ├── src/                        # 内核源文件（纯C，无硬件操作）
│   │   ├── svcrt_init.c            # 内核启动与初始化
│   │   ├── svcrt_task.c            # 任务调度 + SVC 分发
│   │   ├── svcrt_event.c           # 事件管理
│   │   ├── svcrt_fifo.c            # FIFO 环形缓冲区
│   │   ├── svcrt_dev.c             # 设备驱动框架
│   │   └── svcrt_cfg.c             # 任务配置加载
│   ├── app/                        # 应用模板
│   ├── sdk/                        # SDK 开发包
│   │   ├── app_sdk/                # App SDK
│   │   └── driver_sdk/             # Driver SDK
│   └── components/                 # 可选组件
│
├── board/                          # ★ 板级移植层（芯片相关）
│   └── stm32f427/                  # STM32F427 移植示例
│       ├── svcrt_board.c           # 移植接口 + 中断入口 + 板载设备注册
│       ├── svcrt_board_config.h    # 板级配置覆盖（主频、内存地址等）
│       ├── drvuart.c/h             # UART 驱动（HAL库）
│       └── drvled.c/h              # LED 驱动（HAL库）
│
├── kernelsrc/port/                 # ★ CPU 架构移植层：port/<架构族>/<核心>/
│   ├── README.md                   # 移植层说明与新架构移植指南
│   ├── arm/cortex-m3/              # Cortex-M3（无 FPU）
│   └── arm/cortex-m4/              # Cortex-M4
│       ├── svcrt_context.S         # PendSV 上下文切换汇编（含 FPU 寄存器）
│       └── svcrt_port.c            # 系统调用上下文/栈帧/临界区/定时器/MPU 实现
│
└── example/                        # 工程示例
    └── stm32f427/                  # MDK 工程
```

**架构解耦原则：**
- `kernelsrc/` = 纯内核，不包含任何芯片头文件，不直接操作任何硬件寄存器
- `board/` = 芯片相关，移植到新芯片只需创建新的 `board/<芯片>/` 目录
- `svcrt_port.h` / `svcrt_hal.h` = 内核与硬件的唯一耦合点，纯函数声明
- `svcrt_arch.h` = 架构描述与能力派生；新增架构只需在 `port/<架构族>/<核心>/` 下实现接口，内核零改动

## 系统架构

```
┌──────────────────────────────────────────────────────────────────────┐
│                  用户层 (Unprivileged)                                │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐               │
│  │  App Task1   │  │  App Task2   │  │  App TaskN   │               │
│  │  (svcrt.h)   │  │  (svcrt.h)   │  │  (svcrt.h)   │               │
│  └──────┬───────┘  └──────┬───────┘  └──────┬───────┘               │
│         │  SVC 0x10~0x13  │                 │                        │
├─────────┼─────────────────┼─────────────────┼────────────────────────┤
│         └─────────────────┼─────────────────┘                        │
│           内核 kernelsrc/ (零芯片依赖)                                 │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐               │
│  │  任务调度    │  │  事件管理    │  │  设备框架    │               │
│  └──────────────┘  └──────────────┘  └──────────────┘               │
│  ┌──────────────┐  ┌──────────────┘                                  │
│  │  FIFO 缓冲   │  │  配置加载   │                                   │
│  └──────────────┘  └──────────────┘                                  │
├──────────────────────────────────────────────────────────────────────┤
│        svcrt_port.h (硬件抽象接口，纯声明)                             │
├──────────────────────────────────────────────────────────────────────┤
│        board/<芯片>/ (板级移植层)                                      │
│   svcrt_board.c / svcrt_mpu.c / 驱动 / 中断入口                      │
└──────────────────────────────────────────────────────────────────────┘
```

## 应用 API

用户程序包含 `svcrt.h` 即可使用所有操作系统接口，所有调用通过 SVC 指令陷入内核态执行。

### 任务管理

| API | 说明 |
|-----|------|
| `svcrt_task_wait(ms)` | 等待指定毫秒（阻塞调度） |
| `svcrt_task_wait_period()` | 等待当前周期结束 |
| `svcrt_task_delay(us)` | 微秒级忙等延时 |
| `svcrt_task_kill()` | 终止当前任务 |

### 设备操作

| API | 说明 |
|-----|------|
| `svcrt_dev_open(name, param)` | 打开设备，返回句柄 |
| `svcrt_dev_close(handle)` | 关闭设备，释放句柄 |
| `svcrt_dev_read(handle, buf, len)` | 从设备读取数据 |
| `svcrt_dev_write(handle, buf, len)` | 向设备写入数据 |
| `svcrt_dev_ctrl(handle, code, value)` | 设备控制命令 |

### 事件与系统信息

| API | 说明 |
|-----|------|
| `svcrt_event_create(name)` | 创建命名事件 |
| `svcrt_event_wait(handle, timeout)` | 等待事件（可超时） |
| `svcrt_event_set(handle)` | 触发事件 |
| `svcrt_get_time_ms()` | 获取系统运行时间（ms） |
| `svcrt_get_cpu_usage()` | 获取 CPU 空闲率 |

### 信号量与互斥锁

| API | 说明 |
|-----|------|
| `svcrt_sem_create(name, init)` | 创建计数信号量 |
| `svcrt_sem_wait(handle, timeout)` | 等待信号量（计数减一/挂起） |
| `svcrt_sem_post(handle)` | 释放信号量（唤醒等待者/计数加一） |
| `svcrt_sem_delete(handle)` | 删除信号量 |
| `svcrt_mutex_create(name)` | 创建互斥锁 |
| `svcrt_mutex_lock(handle, timeout)` | 加锁（支持优先级继承） |
| `svcrt_mutex_unlock(handle)` | 解锁（仅持有者可解锁） |
| `svcrt_mutex_delete(handle)` | 删除互斥锁 |

### 消息队列（SVC 0x16）

| API | 说明 |
|-----|------|
| `svcrt_mq_create(name)` | 创建消息队列（容量 `SVCRT_MQ_DEPTH`，单条 `SVCRT_MQ_MSG_WORDS` 字） |
| `svcrt_mq_send(h, buf, len_words, timeout)` | 发送消息（拷贝语义，满时阻塞可超时） |
| `svcrt_mq_recv(h, buf, len_words, timeout)` | 接收消息（空时阻塞可超时） |
| `svcrt_mq_delete(h)` | 删除消息队列 |

返回值约定：0=成功，1=超时，-1=错误。

### 软定时器（SVC 0x17）

| API | 说明 |
|-----|------|
| `svcrt_timer_create(name)` | 创建定时器 |
| `svcrt_timer_start(h, period_ms, mode, cb, arg)` | 启动；mode 为单次/周期 |
| `svcrt_timer_stop(h)` | 停止 |
| `svcrt_timer_delete(h)` | 删除 |

到期回调统一在内核自动注册的定时器服务任务上下文中执行（优先级 `SVCRT_TIMER_TASK_PRI`），tick 中断只做倒计时与唤醒，不执行用户回调，保证 SVC 特权隔离不被破坏。

### 任务诊断与故障恢复（SVC 0x11 扩展 + 0x12 扩展）

| API | 说明 |
|-----|------|
| `svcrt_task_status_get(task_id)` | 查询任务状态（READY/WAIT/RUNNING/INVALID） |
| `svcrt_task_recover_req(task_id)` | 请求任务故障恢复（两阶段：重建栈帧后重新调度） |
| `svcrt_fault_record_count()` | 查询故障记录条数 |
| `svcrt_fault_record_read(index, out3)` | 读取第 index 条记录（类型/任务号/tick） |

HardFault/栈溢出自动写入故障环形记录（容量 `SVCRT_FAULT_RECORD_NUM`，记满覆盖最旧）；使能 `SVCRT_USE_FAULT_RECOVER` 后故障任务自动重建恢复，不再永久丢失任务槽位。

### 中断上下文安全 API（特权态直调，不经 SVC）

| API | 说明 |
|-----|------|
| `svcrt_sem_post_from_isr(h)` | 在 ISR 中释放信号量（唤醒任务，不切换） |
| `svcrt_event_set_from_isr(h)` | 在 ISR 中触发事件 |
| `svcrt_mq_send_from_isr(h, buf, len)` | 在 ISR 中发送消息（满则丢弃返回 -1） |

## 驱动开发接口

SVCrtOS 的驱动有两种部署形态，**核心亮点是「独立驱动」——驱动可以编译为完全独立的固件，与内核解耦、独立烧录、独立升级**：

| 形态 | 编译方式 | 部署 | 适用场景 |
|------|---------|------|---------|
| 内置驱动 | 随内核一起编译 | 与内核同一固件 | 板载固定外设 |
| **独立驱动** | **单独编译为 .bin/.hex** | **烧录到专属 ROM 分区** | **可热插拔/独立升级的驱动** |

### 独立驱动（External Driver）

独立驱动是一个**自带启动代码、自带分散加载脚本、独立编译链接**的固件，烧录到内核之外的专属 ROM/RAM 分区。内核把该分区入口注册为任务后调度运行，驱动在用户态执行，通过 SVC `0x14` 向内核注册设备。注册后，**任何 App（无论内置还是独立固件）都可以用设备名打开它**，完全不需要知道驱动的实现或地址。

```
┌─────────────────────┐   ┌─────────────────────┐   ┌─────────────────────┐
│   内核固件           │   │  独立驱动固件 BLED   │   │  独立应用固件 BLED   │
│   (ROM 分区 0)       │   │  (ROM 分区 1)        │   │  (ROM 分区 2)        │
│                     │   │                     │   │                     │
│  任务调度/设备框架   │   │  DrvMain()          │   │  AppMain()          │
│         ▲           │   │   ├ 注册 "BLED"     │   │   ├ open("BLED")    │
│         │ SVC 0x14  │?──┼───┘ (svcrt_drv_     │   │   └ write(...)      │
│         │ 注册设备   │   │      register)      │   │         │           │
│         │           │   │                     │   │         │ SVC 0x10  │
│         └───────────┼───┼─────────────────────┼───┼─────────┘ 设备 IO   │
│      内核查表分发到驱动函数指针，三个固件互不依赖地址                      │
└─────────────────────┘   └─────────────────────┘   └─────────────────────┘
```

驱动开发者只需包含 `svcrt_driver_sdk.h`，实现 `svcrt_dev_drv_t` 接口，并在 `DrvMain()` 中注册：

```c
#include "svcrt_driver_sdk.h"

/* 1. 实现 5 个标准设备接口 */
static svcrt_dev_drv_t bled_drv = {
    bled_drv_open,
    bled_drv_close,
    bled_drv_read,
    bled_drv_write,
    bled_drv_ctrl
};

/* 2. 独立固件入口：注册设备名后即可被任意 App 使用 */
void DrvMain(void)
{
    svcrt_drv_register("BLED", &bled_drv, 0);   /* SVC 0x14 注册到内核 */

    while(1)
    {
        svcrt_task_wait(1000);   /* 驱动可常驻做后台维护，也可注册后退出 */
    }
}
```

应用侧无需关心驱动是内置还是独立固件，统一用设备名访问：

```c
int32 h = svcrt_dev_open("BLED", 0);   /* 内核查表，自动路由到独立驱动 */
int32 on = 1;
svcrt_dev_write(h, &on, 1);            /* 点亮蓝灯 */
```

> **独立驱动注意事项**：
> 1. 启动汇编必须经 C 库入口 `__main` 完成 `.data` 拷贝、`.bss` 清零，否则 `bled_drv` 函数指针表为随机值，注册后调用会 HardFault。
> 2. 驱动固件、应用固件、内核固件的 ROM/RAM 分区地址必须互不重叠（由各自的分散加载脚本 `.sct` 约定）。
> 3. 在 MPU 隔离环境下，独立驱动若需直接访问外设寄存器，需由内核授予对应外设区域权限。
>
> 完整可运行示例见 `example/stm32f427/driver_sdk/BLED_DRV/`（独立驱动）与 `example/stm32f427/app_sdk/BLED_APP/`（独立应用），端到端说明见 `example/stm32f427/BLUE_LED_E2E_README.md`。

## 内核配置参数

所有配置参数集中在 `svcrt_config.h` 中管理，芯片相关参数可通过板级配置文件覆盖。

| 参数名 | 默认值 | 说明 |
|--------|--------|------|
| `SVCRT_CPU_ARCH` | `SVCRT_ARCH_CORTEX_M4` | CPU 架构选择 |
| `SVCRT_USE_FPU` | 1 (M4/M7) | 浮点单元使能 |
| `SVCRT_USE_MPU` | 1 (M4/M7) | MPU 内存保护使能 |
| `SVCRT_USE_PRIV` | 1 (依赖MPU) | 特权级分离使能 |
| `SVCRT_TASK_MAX_NUM` | 7 | 最大任务数量 |
| `SVCRT_TICK_PERIOD_US` | 500 | 滴答周期（微秒） |
| `SVCRT_EVENT_NUM` | 10 | 事件对象数量 |
| `SVCRT_DEV_MAX_NUM` | 8 | 最大设备数量 |
| `SVCRT_USE_SPINLOCK` | 1 | 自旋锁开关（svcrt_spin.h） |
| `SVCRT_USE_SCHED_LOCK` | 1 | 用户态调度器锁开关（SVC 0x11 子命令 7~9） |
| `SVCRT_USE_STACK_USAGE` | 1 | 任务栈峰值用量统计开关 |
| `SVCRT_STACK_FILL_PATTERN` | 0xcdcdcdcd | 栈填充图案（用于水位统计） |
| `SVCRT_USE_CPU_LOAD` | 1 | CPU 负载统计开关 |
| `SVCRT_USE_STACK_CHECK` | 1 | 栈溢出检测开关 |
| `SVCRT_SHARE_MEM_ADDR` | 0x20028000 | 共享内存地址（板级配置覆盖） |
| `SVCRT_SHARE_MEM_SIZE` | 0x8000 | 共享内存大小（板级配置覆盖） |
| `SVCRT_SYSTEM_CLOCK_HZ` | 168000000 | 系统主频（板级配置覆盖） |
| `SVCRT_USE_MQ` / `SVCRT_MQ_NUM` | 1 / 8 | 消息队列开关与数量 |
| `SVCRT_MQ_DEPTH` / `SVCRT_MQ_MSG_WORDS` | 8 / 4 | 队列深度与单条消息长度（字） |
| `SVCRT_USE_TIMER` / `SVCRT_TIMER_NUM` | 1 / 8 | 软定时器开关与数量 |
| `SVCRT_TIMER_TASK_PRI` | 200 | 定时器服务任务优先级 |
| `SVCRT_TIMER_TASK_STACK_WORDS` | 96 | 定时器服务任务栈大小（字） |
| `SVCRT_USE_FAULT_RECOVER` | 1 | 任务故障自动恢复开关 |
| `SVCRT_FAULT_RECORD_NUM` | 8 | 故障记录环形缓冲容量 |

## API 文档

内核头文件采用 Doxygen 注释风格，可一键生成 API 参考：

```bash
python tools/gen_api_doc.py          # 有 Doxygen 时生成 HTML，否则生成 Markdown
python tools/gen_api_doc.py --md     # 强制生成 Markdown
```

- 已安装 Doxygen：输出 `docs/api/html/index.html`（带交叉引用、调用关系图）
- 未安装 Doxygen：输出 `docs/api/SVCrtOS_API参考.md`（内置解析器，零依赖）

> 说明：内核源码存在 GBK / UTF-8 混用，脚本会先在系统临时目录生成 UTF-8 副本再生成文档，
> 不会修改仓库内任何源文件；配置见根目录 `Doxyfile`。

## 快速移植

SVCrtOS 可移植到任何 ARM Cortex-M MCU，只需在 `board/` 目录下创建新的芯片移植目录。

1. **创建板级目录**：`board/<你的芯片>/`
2. **创建 `svcrt_board_config.h`**：设置芯片的主频和内存地址
3. **实现 `svcrt_board.c`**：实现 `svcrt_port.h` 中的所有接口函数 + 中断入口 + MPU 操作（M3 可跳过）
4. **移植上下文汇编**：参考 `kernelsrc/port/arm/cortex-m4/svcrt_context.S`（不同内核版本需适配 FPU 处理）
5. **编写板载驱动**：UART、LED 等

**关键：kernelsrc/ 目录无需任何修改！**

详细移植步骤请参考 [移植手册](kernelsrc/porting_manual.md)。

## 外部分区固件（用户态 App / Driver）

除了把任务直接编译进内核，SVCrtOS 还支持把应用和驱动编译为**独立固件**烧录到专属 ROM 分区，由内核调度运行，实现固件解耦与独立升级。

完整示例见 `example/stm32f427/app_sdk/`（应用）、`example/stm32f427/driver_sdk/`（驱动）以及蓝灯端到端示例 `example/stm32f427/BLUE_LED_E2E_README.md`。

集成要点（详见 [SDK 用户手册](kernelsrc/sdk/sdk_user_manual.md) 第三部分）：
1. 内核侧将外部分区入口地址注册为任务（`入口 = 分区基址 | 1`）
2. 外部固件启动汇编必须经 C 库入口 `__main` 完成 `.data` 拷贝、`.bss` 清零，否则驱动接口函数指针表为随机值导致 HardFault
3. 内核、各固件的 ROM/RAM 分区地址必须互不重叠

## 启动流程

```
main()
  ├── svcrt_port_board_init()      // 硬件早期初始化（board层实现）
  ├── svcrt_cfg_load()             // 加载任务配置表
  ├── svcrt_port_irq_init()        // 中断优先级配置（board层实现）
  ├── svcrt_kernel_init()          // 内核模块初始化（事件/设备/MPU等）
  ├── svcrt_port_start_timer()     // 启动 SysTick（board层实现）
  └── svcrt_start_idle()           // 切换到 PSP，进入特权模式
        └── WFI 循环               // 等待第一个任务就绪
```

## 命名规范

SVCrtOS 采用统一的命名规范，参考 FreeRTOS / RT-Thread 风格：

| 类别 | 格式 | 示例 |
|------|------|------|
| 应用层 API | `svcrt_模块_动作` | `svcrt_task_wait()`, `svcrt_dev_open()` |
| 内核内部函数 | `svcrt_模块_动作_internal` | `svcrt_task_wait_internal()` |
| 调度器函数 | `svcrt_sched_动作` | `svcrt_sched_next()`, `svcrt_sched_activate()` |
| 移植层函数 | `svcrt_port_动作` | `svcrt_port_board_init()` |
| 板级实现文件 | `svcrt_board.c` | 移植接口实现 + 中断入口 |
| 结构体类型 | `svcrt_模块_t` | `svcrt_task_t`, `svcrt_fifo_t`, `svcrt_dev_drv_t` |
| 枚举常量 | `SVCRT_模块_状态` | `SVCRT_TASK_READY`, `SVCRT_TASK_WAIT` |
| 宏常量 | `SVCRT_大写描述` | `SVCRT_FIFO_MAGIC`, `SVCRT_DEV_HANDLE_FLAG` |
| 配置宏 | `SVCRT_USE_特性` | `SVCRT_USE_FPU`, `SVCRT_USE_MPU` |
