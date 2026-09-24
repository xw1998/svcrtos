# SVCrtOS

[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![Release](https://img.shields.io/badge/release-0.0.2-brightgreen.svg)](https://gitee.com/xw19981010/svcrtos_new/tags)
[![Gitee](https://img.shields.io/badge/gitee-svcrtos__new-red.svg)](https://gitee.com/xw19981010/svcrtos_new)

**简体中文 | [English](readme_en.md)**

SVCrtOS 是一个面向 ARM Cortex-M 系列微控制器的安全实时操作系统（RTOS），采用 SVC（Supervisor Call）实现特权级隔离，利用 MPU 进行内存保护，支持多任务分区调度和统一设备驱动框架。

**它对外的承诺只有一条：让 MCU 像手机一样安装、升级、卸载应用与驱动。**
不写死地址、不重编内核、不整机重烧——应用与驱动以 `.svcapp` 镜像装进 Flash 上的统一镜像池，
运行期认领、崩溃有记账、卸载按力度回收空间。

## 它和通用 RTOS 差在哪

通用 RTOS（FreeRTOS / RT-Thread / Zephyr）把应用编译进固件：换一个应用 = 改代码、重编、重烧、整机复位。
SVCrtOS 把固件拆成**内核 / 驱动 / 应用**三块可以各自独立升级的映像，于是多出一条别人没有的能力线：

| 维度 | 通用 RTOS | SVCrtOS |
|---|---|---|
| 换一个应用 | 改代码 → 重编固件 → 整机重烧 | 串口或图形上位机装入 `.svcapp`，重启后自动认领 |
| 应用与地址的关系 | 编译期写死在同一张链接脚本里 | 地址只在 `config/svcrt_partition.h` 定义一次，其余全部派生；Loader / App 工程**禁止** include 分区头 |
| 驱动的部署 | 随内核一起编译 | 可独立编译为专属 ROM 分区的固件，独立烧录、独立升级；App 只认设备名，不知道地址 |
| 权限模型 | 全部代码同权（MPU 一般不开） | 任务跑非特权态，SVC 陷入内核；MPU 逐任务隔离，越权访问当场被硬件拦下 |
| 应用写错后的死循环 | 看门狗复位整机 | 崩溃日记跨复位记账，同一镜像连续 3 次禁用**该槽位**，其余应用照常跑 |
| 卸载 | 这个概念不存在 | 作废镜像 → 所属物理单元空出后真擦 → `pool free` 由 776244 B 回到 786432 B |
| 运行中换版本 | 停机重启 | 两个小程序交接控制权，迭代计数逐字连续，交接空洞实测 27~407 ms |
| 既有 C 代码的迁移 | 需要改成 RTOS 自己的 API | 提供 POSIX / Windows 兼容层，改 include 列表即可编译上板 |

> 这张表只比「动态安装」这一条主线。生态、协议栈与功能认证是另外的维度，本项目不声称在那些方面更强。

## 实测数据速览

下面每个数字都来自设备端真实返回或内核导出，不是估算；口径与复现方式在各文档里写清。

| 指标 | 实测值 | 出处 |
|---|---|---|
| 单趟 PendSV 往返（插桩关） | 710.5 → **430.9 周期**（就绪集改为 256 位两级位图 + 链，−39%） | [调度器说明](docs/调度器说明.md) |
| PendSV 占 CPU（插桩关） | 0.768% → **0.448%** | [调度器说明](docs/调度器说明.md) |
| 4.97 s 窗口内的切换间隔 | 5153 次切换**全部**落在 10 µs 与 2000 µs 两条带上，无一次跳变 | 本文 §调度表现 |
| 空载 CPU 时间占用 | `idle` **99.48%** | 本文 §调度表现 |
| MPU 越权防护 | `pass=78 fail=0`（App 主动越权访问被拦下） | [内存保护与App越权验证](docs/内存保护与App越权验证.md) |
| FPU 非特权使用 | APP_DEMO 第八节七项全过：写进 S16-S19 的值跨任务切换后读回仍相符 | [应用安装与调试指南](docs/SVCrtOS应用安装与调试指南.md) |
| VFS 端口并发 | 两个写者 + 一个只读读者压同一路径，读者只看到完整载荷，`pass=128 fail=0` | [VFS路径命名空间](docs/VFS路径命名空间.md) |
| 文件类 POSIX | `FS_DEMO` 上板自检全过（open/read/write/stat/dirent 打到 VFS） | [POSIX与Windows兼容层说明](docs/POSIX与Windows兼容层说明.md) |
| socket / select | F427 + `SOCKET_DEMO`，`pass=36 fail=0`（TCP 回显、UDP、IPv6 报错三组） | [socket与select兼容层](docs/socket与select兼容层.md) |
| 运行中升级 | 交接空洞 **27 / 31 / 224 / 407 ms**，`it=994 → resumed at it=994`，受控量不回零 | [无缝升级例程](docs/无缝升级例程.md) |
| 节拍计数器 32 位回绕 | 注入使 `svcrt_kernel_tick` 跨过回绕，两侧 `APP_ALIVE` 间隔精确 5.000 s | [调度器说明](docs/调度器说明.md) §2.3 |
| 双板同源 | F427 与 F401 同一份内核源码均编译通过（F401 `Code=46526`） | 本文 §验证状态 |

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
- **统一镜像池 + 两种安装策略**：App 与驱动共用一个池，安装时按 1 KB 粒度贴合放置；整机可选「固定槽位」（落点与 RAM 窗口按配置走，适合交付客户二次开发）或「自动选址」（在池内找最大连续空位，卸载按力度回收）
- **设备端持久配置区**：安装策略、槽位表与运行期参数写在独立的配置扇区，由上位机在线写入（`tools/svcrt_cfg.py` 或图形界面），不必重新编译；策略类字段重启后生效——写入的命令是 `cfg load`，它负责**接收**记录，不是「重新读一遍」
- **线程服务**：App / 驱动可以在自己的固件与 RAM 窗口内创建线程（SVC 0x1B），入口与栈都要落在调用者自己的窗口内，否则拒绝
- **POSIX / Windows 兼容层**：`pthread` / `semaphore` / `mqueue` / `unistd` 与 `CreateThread` / `Sleep` / `strcpy_s` 等常用名直接可用，移植既有 C 程序改 include 列表即可
- **双 SDK 架构**：独立的应用 SDK 和驱动 SDK，支持编译为独立分区固件
- **文件系统与 VFS**：内置 littlefs + NOR 块设备，`svcrt_fs` 门面之上是 ark_vfs 的路径命名空间；文件类 POSIX（`open` / `read` / `write` / `stat` / `dirent`）直接打到 VFS
- **小程序**：文件系统里的第三种用户态应用——不安装、不占槽位，从卷里读进 RAM 就执行，退出即把两块内存（代码块 + RAM 块）还给内核；支持多实例、崩溃禁用与开机自启
- **运行中升级**：两个小程序之间可以交接控制权（状态记录 + 请求记录两个文件，三条硬规则），旧版让出、新版接手，期间不停机、受控量不回零
- **内核与芯片解耦**：采用 RT-Thread 类似的分层架构，内核零芯片依赖，移植只需修改 board/ 目录

## 例程一览

`example/stm32f427/` 下的每个工程都是可以直接 `UV4 -r` 编译、`-f` 烧录、上板自检的完整工程：

| 工程 | 类型 | 演示什么 |
|---|---|---|
| `kernel/SVCRTOS_TEST` | 内核 | 内核本体（板上在跑的这一份） |
| `app_sdk/APP_DEMO` | 应用 | 全功能自测八节：任务 / 事件 / 信号量 / 互斥锁 / 消息队列 / 软定时器 / 堆与 pthread / FPU |
| `app_sdk/POSIX_DEMO` | 应用 | 把 Linux / Windows 写法的 C 程序直接搬上来 |
| `app_sdk/FS_DEMO` | 应用 | 文件类 POSIX 打到 VFS / littlefs |
| `app_sdk/SOCKET_DEMO` | 应用 | POSIX socket / select 跑在 lwIP 回环网卡上 |
| `app_sdk/MINI_DEMO` | 小程序 | 从文件系统读进 RAM 执行、退出即归还两块内存 |
| `app_sdk/SEAMLESS_V1` / `SEAMLESS_V2` | 小程序 | 运行中交接控制权（无缝升级），见 [无缝升级例程](docs/无缝升级例程.md) |
| `app_sdk/BLED_APP` + `driver_sdk/BLED_DRV` | 应用 + 独立驱动 | 三个固件互不依赖地址的端到端例子，见 `BLUE_LED_E2E_README.md` |
| `app_sdk/APP_BAD` | 应用 | 故意写错的镜像：验证崩溃记账与「连续 3 次禁用」 |
| `driver_sdk/DRV_DEMO` | 驱动 | 驱动骨架，与 App 同一条安装通道：`--type driver` → `.svcapp` → `install` |
| `example/stm32f401/kernel/SVCRTOS_TEST` | 内核 | 换一颗芯片要改多少（只改 `board/` 与板级分区头） |

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
│   │   ├── driver_sdk/             # Driver SDK
│   │   └── posix/                  # POSIX / Windows 兼容层（App 侧，见其 README）
│   ├── shell/                      # 内核控制台（ark-shell + SVCrtOS 平台适配层）
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
├── tools/                          # 上位机工具（gen_scatter / pack_app / svcrt_layout / gen_api_doc）
├── docs/                           # 文档，索引见 docs/README.md
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

### 阻塞接口的超时语义（统一约定）

event / sem / mutex / mq 的等待接口共用同一套 `timeout_ms` 约定，与 FreeRTOS、
RT-Thread、Zephyr 一致：

| 取值 | 语义 |
|------|------|
| `timeout_ms == 0` | **不等待**：只试一次，条件不满足立即返回超时 |
| `timeout_ms > 0` | 最多等待 `timeout_ms` 毫秒，超时返回 |
| `timeout_ms < 0` | **永久等待**，直到条件满足 |

```c
svcrt_mutex_lock(mtx, -1);   /* 永久等待（推荐写法，语义一眼可见） */
svcrt_mutex_lock(mtx, 0);    /* 只试一次，拿不到就返回超时 */
```

> 早期版本把 0 当作"永久等待"，与主流 RTOS 相反，且同一头文件里三类接口写法不一致。
> 现已统一为上面的约定，仓库内的调用点也全部改成显式 `-1`。

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
| `svcrt_mq_create(name)` | 创建消息队列（深度 `SVCRT_MQ_DEPTH`，单条最长 `SVCRT_MQ_MSG_WORDS` 字） |
| `svcrt_mq_send(h, buf, len_words, timeout)` | 发送一条 `len_words` 字的消息（1..`SVCRT_MQ_MSG_WORDS`；拷贝语义，满时阻塞可超时） |
| `svcrt_mq_recv(h, buf, len_words, timeout)` | 接收一条消息，拷入 `len_words` 字的调用者缓冲（空时阻塞可超时） |
| `svcrt_mq_delete(h)` | 删除消息队列 |

**消息是变长的**：`len_words` 由调用者给出，内核只拷这么多字、槽内其余字节补 0，并在槽内记下实际长度。
`len_words` 必须 ≤ 接收缓冲的容量——**调用者缓冲是硬边界**。

返回值约定：`svcrt_mq_send` 返回 0=成功、负值=超时或错误；`svcrt_mq_recv` 返回**实际收到的字数**（>0），负值=超时或错误。

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

### 线程服务（SVC 0x1B）

App / 驱动在**自己的 RAM 窗口内**创建线程。线程需要 TCB 与调度器槽位，只有内核有，
所以必须走这一组调用：

| API | 说明 |
|-----|------|
| `svcrt_thread_create(entry, stack, stack_size, priority, period_ms)` | 创建线程，返回任务号（>0）；`period_ms=0` 为事件驱动，>0 为周期线程 |
| `svcrt_thread_self()` | 当前任务号（>0） |
| `svcrt_thread_exit()` | 结束当前线程，不再返回 |

内核在创建时做两道边界校验，不通过就拒绝而不是静默接受：

1. **入口必须落在调用者自己的固件窗口内**；
2. **整段栈必须落在调用者自己的 RAM 窗口内**（内核不会给别人分配或跨窗口使用栈）。

`priority` 取 1..254，`255` 是调度器「无候选」哨兵，会被显式拒绝。

不想直接用它的话，`kernelsrc/sdk/posix/` 提供了一层 POSIX / Windows 兼容层，
用 `pthread_create` / `CreateThread` 的写法即可，见 [POSIX与Windows兼容层说明.md](docs/POSIX与Windows兼容层说明.md)。

### 中断上下文安全 API（特权态直调，不经 SVC）

| API | 说明 |
|-----|------|
| `svcrt_sem_post_from_isr(h)` | 在 ISR 中释放信号量（唤醒任务，不切换） |
| `svcrt_event_set_from_isr(h)` | 在 ISR 中触发事件 |
| `svcrt_mq_send_from_isr(h, buf, len)` | 在 ISR 中发送消息（满则丢弃返回 -1） |

## 驱动开发接口

SVCrtOS 的驱动有三种投递形态，**核心亮点是「驱动与 App 走同一条安装通道」**：驱动就是一个 `type = driver` 的 `.svcapp`，用装 App 的同一套命令装进同一个镜像池，内核按镜像头里的类型分流到驱动槽位。

| 形态 | 编译方式 | 部署 | 适用场景 |
|------|---------|------|---------|
| 内置驱动 | 随内核一起编译 | 与内核同一固件 | 板载固定外设 |
| **池内安装驱动** | **单独编译 + `pack_app.py --type driver`** | **串口 `install` 进统一镜像池，与 App 同一个 `.svcapp` 封装** | **随应用一起分发、可安装/卸载/升级的驱动** |
| **独立驱动固件** | **单独编译为 .bin/.hex** | **烧录到专属 ROM 分区** | **可热插拔/独立升级的驱动** |

```bash
# 与打包 App 的唯一差别是 --type driver（包内 type = 2，内核据此分流到驱动槽位）
python tools/pack_app.py --project example/stm32f427/driver_sdk/DRV_DEMO/MDK-ARM/drv_demo.uvprojx \
    --type driver --name DRV_DEMO --out build/DRV_DEMO/DRV_DEMO.svcapp
# 上板后敲 install 打开接收窗口（fixed 模式敲 install <槽位号>），再把文件原样发给串口；
# 校验用 drv list：应出现一条 type=drv 的槽位记录。
```

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
| `SVCRT_CPU_ARCH` | 由架构头派生（本板 `SVCRT_ARCH_CORTEX_M4`） | CPU 架构选择 |
| `SVCRT_USE_FPU` | 由架构派生，本板 1 | 浮点单元使能 |
| `SVCRT_USE_MPU` | 由架构派生，本板 1 | MPU 内存保护使能。Cortex-M4 上默认 = `SVCRT_ARCH_HAS_MPU`，全仓无显式关闭；机制与上板验证见 [docs/内存保护与App越权验证.md](docs/内存保护与App越权验证.md) |
| `SVCRT_USE_PRIV` | 依赖 `SVCRT_USE_MPU`，本板 1 | 特权级分离使能。`SVCRT_USE_MPU == 1` 时自动为 1：App 任务非特权（CONTROL.nPRIV = 1），内核任务特权 |
| `SVCRT_TASK_MAX_NUM` | 48 | 任务表总容量（静态 TCB 数组元素个数）。默认值已**移到 `config/svcrt_partition.h` 第九节**，与分区策略同源，见下文「任务容量分层」 |
| `SVCRT_TASK_TABLE_RAM_MAX` | 8192 | TCB 数组的 RAM 预算上限（字节），**固定 8KB，不随容量派生**；守护断言直接比较 `sizeof(svcrt_task_table)`，超出则编译报错 |
| `SVCRT_TICK_PERIOD_US` | 500 | 滴答周期（微秒） |
| `SVCRT_EVENT_NUM` | 10 | 事件对象数量 |
| `SVCRT_MAX_EVENT_WAITERS` | 4 | 单个事件的等待者上限 |
| `SVCRT_SEM_NUM` / `SVCRT_MTX_NUM` | 8 / 8 | 信号量 / 互斥锁对象数量 |
| `SVCRT_MAX_SYNC_WAITERS` | 4 | 单个信号量/互斥锁的等待者上限 |
| `SVCRT_DEV_MAX_NUM` | 8 | 最大设备数量 |
| `SVCRT_USE_SPINLOCK` | 1 | 自旋锁开关（svcrt_spin.h） |
| `SVCRT_USE_SCHED_LOCK` | 1 | 用户态调度器锁开关（SVC 0x11 子命令 7~9） |
| `SVCRT_USE_STACK_USAGE` | 1 | 任务栈峰值用量统计开关 |
| `SVCRT_STACK_FILL_PATTERN` | 0xcdcdcdcd | 栈填充图案（用于水位统计） |
| `SVCRT_USE_CPU_LOAD` | 1 | CPU 负载统计开关 |
| `SVCRT_USE_STACK_CHECK` | 1 | 栈溢出检测开关 |
| `SVCRT_SHARE_MEM_ADDR` / `SVCRT_SHARE_MEM_SIZE` | — | **已删除**：共享内存位置由 `config/svcrt_partition.h` 的 `SHARE_RAM_BASE` / `SHARE_RAM_SIZE` 决定 |
| `SVCRT_SYSTEM_CLOCK_HZ` | 168000000 | 系统主频（板级配置覆盖） |
| `SVCRT_USE_MQ` / `SVCRT_MQ_NUM` | 1 / 8 | 消息队列开关与数量 |
| `SVCRT_MQ_DEPTH` / `SVCRT_MQ_MSG_WORDS` | 8 / 4 | 队列深度与单条消息长度（字） |
| `SVCRT_USE_TIMER` / `SVCRT_TIMER_NUM` | 1 / 8 | 软定时器开关与数量 |
| `SVCRT_TIMER_TASK_PRI` | 200 | 定时器服务任务优先级 |
| `SVCRT_TIMER_TASK_STACK_WORDS` | 96 | 定时器服务任务栈大小（字） |
| `SVCRT_USE_FAULT_RECOVER` | 1 | 任务故障自动恢复开关 |
| `SVCRT_FAULT_RECORD_NUM` | 8 | 故障记录环形缓冲容量 |
| `SVCRT_USE_STACK_CHECK` / `SVCRT_STACK_END_FLAG` | 1 / 0xed01 | 栈溢出检测开关与栈底保护字 |

所有配置宏都用 `#ifndef` 包裹，板级配置先于 `svcrt_config.h` 加载，因此**覆盖板级配置
是唯一入口**，不需要改 `kernelsrc/`。

### 分区与镜像策略开关（`config/svcrt_partition.h`）

地址与分区布局只在 `config/svcrt_partition.h` 里定义一次，其余地址一律派生；
`.sct` 由 `tools/gen_scatter.py` 生成，属构建产物，**不要手工编辑**。
几个影响运行行为的策略开关：

| 宏 | `config/` 默认 | 说明 |
|----|----|----|
| `APP_AUTO_START` / `DRIVER_AUTO_START` | 1 | **只影响裸镜像路径**（Keil 直接烧录的镜像没有镜像头，无法自带自启标志）。带头的 `.svcapp` 的自启由镜像头 `flags` 决定，并可在设备端配置区里逐槽改写 |
| `APP_ALLOW_RAW_IMAGE` | 0 | 是否认领裸镜像（无镜像头、直接烧进池的固件）。编译期默认 0，F427 板级置 1；**运行期还要设备端配置区 `flags` 的 `RAW_ALLOW` 位一起成立**，两处都开才生效 |
| `APP_CRASH_RESTART_MAX` | 3 | App/驱动连续故障重启上限，达到即禁用；0 = 不限次 |
| `INSTALLER_ENABLE` | 1 | 内核内安装模块（占用 COM1）；不用串口安装时置 0 |
| `SHELL_ENABLE` | 1 | 内核 Shell 控制台（ark-shell，占用 `SHELL_DEV_NAME`）；置 1 时不注册常驻安装任务，安装改由 `install` 命令触发一次性窗口 |
| `INSTALLER_FIXED_SLOT` | -1 | 固定槽模式下常驻安装任务的目标槽（配置槽序号）；`-1` = 不指定，此时常驻任务拒绝任何帧。设成 N：上电时先停掉该槽里正在运行的镜像、再整槽擦一次，然后只接受一帧，要再装就重启 |
| 镜像头 `flags` | 0x1 | 逐槽开机自启标志，打包时由 `tools/pack_app.py --autostart / --no-autostart` 写入；运行期用 `app list` / `drv list` 的 auto 列查看 |
| `SLOT_MAX` / `SVCRT_CFG_SLOT_MAX` | 16 / 8 | 槽位记录条数上限（共享表数组 / 设备端配置区容量）。两者都必须 ≤ `SVCRT_SLOT_ARRAY_MAX`（16，见 `svcrt_share.h`），越界由编译期断言拦住 |
| `SVCRT_DEV_SLOT_MAX` / `SVCRT_DEV_RAM_WINDOW` | 4 / 16 KB | 开发槽位表条目数与每条的 RAM 窗口。窗口序号 = 起始单元号 % 条目数，公式与 `tools/gen_scatter.py` 的 `dev_ram_base` 同源 |
| `SVCRT_RECLAIM_MODE` | `SVCRT_RECLAIM_GLOBAL` | 自动选址模式下的卸载回收力度：全局压实 / 只处理含失效字节的扇区 |
| `DRIVER_TASK_STACK_SIZE` / `APP_TASK_STACK_SIZE` | 1K / 4K | 驱动 / App 任务栈，从**各自槽位那块 RAM** 的顶部切出 |

> 注意：Loader / App 工程**禁止** `#include "svcrt_partition.h"`，布局在运行期经 SVC 0x18 获取。

### 任务容量分层（扩容入口）

任务表是一块静态 TCB 数组，容量在编译期定死。为了让「很多用户驱动 + 很多用户应用」
的场景可以直接扩到大规模，容量按**内核 / 驱动 / 应用**三类分层，全部集中在
`config/svcrt_partition.h` 第九节（与 Flash/RAM 布局同一个文件，避免改一处忘另一处）：

| 宏 | 默认值 | 说明 |
|----|----|----|
| `SVCRT_TASK_MAX_NUM` | 48 | 任务表总容量，扩规模只改这个数字 |
| `SVCRT_TASK_MAX_KERNEL` | 8 | 内核自带任务 + 示例静态任务的预留 |
| `SVCRT_TASK_PER_DRIVER` | 1 | 每个驱动镜像占用的任务数 |
| `SVCRT_TASK_PER_APP` | 1 | 每个 App 镜像占用的任务数 |
| `SVCRT_TASK_NEED_MIN` | 派生 | `内核 + 槽位数 × 每槽任务数` 之和，即分区策略要求的最小容量 |
| `SVCRT_TASK_TABLE_RAM_MAX` | 8192 | 静态 TCB 数组的 RAM 预算（字节），固定值；实测 `sizeof(svcrt_task_t)` = 76 字节（MPU 关）/ 140 字节（MPU 开） |

编译期断言 `svcrt_task_capacity_check` 会拦住「容量装不下当前槽位策略」的配置；
`svcrt_cfg.c` 的 `svcrt_task_table_ram_check` 再拦一道 RAM 预算。

`svcrt_config.h` 不再自带这两个宏的默认值，而是 `#include "svcrt_partition.h"`；
板级配置仍然优先（分区头里的定义用 `#ifndef` 包裹，且板级配置先于它加载）。

### 分区、槽位与 ABI 版本

Flash 布局（F427，1 MB）：`BOOT(0)` → `KERNEL(128 KB)` → `CONFIG(128 KB，一个物理扇区)` → `IMAGE_POOL(768 KB，6 × 128 KB 单元)`。

| 项 | 值 | 说明 |
|----|----|----|
| 内核区 | `KERNEL_SIZE` = 128 KB @ `0x08000000` | **可按编译结果调整**（内核大了就给它更多）；改动只在这一个宏 |
| 配置区 | `CONFIG_SIZE` = 128 KB @ `0x08020000` | 设备端持久配置：安装策略、槽位表、运行期参数，由上位机在线写入 |
| 镜像池 | `IMAGE_POOL_SIZE` = 768 KB @ `0x08040000`，6 × 128 KB 单元 | App 与驱动**共用统一池**；单元内按 1 KB 粒度分配 |
| 槽位条目 | `SLOT_MAX` = 16（配置区 `SVCRT_CFG_SLOT_MAX` = 8） | 一次能记录的镜像条目数，≤ `SVCRT_SLOT_ARRAY_MAX`(16) |
| 开发槽位 | `SVCRT_DEV_SLOT_MAX` = 4，窗口 `SVCRT_DEV_RAM_WINDOW` = 16 KB | 只服务「Keil 直接烧录 + 下断点」这条旁路；裸镜像不可搬移、不参与回收 |
| RAM 池 | `SLOT_RAM_TOTAL` = 64 KB @ `0x20010000` | 伙伴分配，块大小 1 KB ~ 32 KB |
| 共享内存 ABI | `SVCRT_PARTITION_VERSION` = **7** | 分区表 / 配置区 / 开发槽位表都在这个结构里交换 |
| 硬件兼容签名 | `SVCRT_HW_COMPAT_ID` = **`0x42700005`** | 低 16 位是镜像兼容世代；旧的 `.svcapp` 会被兼容校验拦下，必须重新打包 |

**两种安装策略，整机二选一**，由设备端配置区决定（编译期只给回退默认）：

| 策略 | 语义 |
|------|------|
| **固定槽位**（`fixed`） | 镜像落点与 RAM 窗口都按配置里的槽位走。卸载只擦本槽，不做压实、空间不退回池。适合「设备交给客户二次开发、客户直接接 MDK 下载调试」的场景 |
| **自动选址**（`auto`） | 安装时在池内找最大连续空位，以 1 KB 粒度贴合放置；卸载按 `SVCRT_RECLAIM_MODE` 做全局压实或最小移动 |

> 两句话记住边界：**地址只在 `config/svcrt_partition.h` 定义一次**，其余地址一律派生；
> **Loader / App 工程禁止 `#include "svcrt_partition.h"`**，布局在运行期经 SVC 0x18 获取。

配置记录的字节布局、错误码、槽位表语义与上位机用法见
[配置区与安装策略.md](docs/配置区与安装策略.md)。

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

## 板级职责：节拍入口与 HAL 毫秒时基

`SysTick_Handler` 由**板级**实现，内核只对外提供 `svcrt_kernel_tick_handler()`。
除了喂内核节拍，如果板级用了 STM32 HAL，还必须在这里把 HAL 的毫秒时基推进起来：

```c
void SysTick_Handler(void)
{
    /* 内核节拍 500us = 2kHz，HAL 时基 1ms = 1kHz，所以每 2 个节拍补一次 */
    if((svcrt_kernel_get_tick() % (1000u / SVCRT_TICK_PERIOD_US)) == 0u)
    {
        HAL_IncTick();
    }

    svcrt_kernel_tick_handler();
}
```

**为什么必须补**：CubeMX 生成的 `SysTick_Handler`（全工程唯一调用 `HAL_IncTick()`
的地方）会被本工程在 `stm32f4xx_it.c` 里整体屏蔽。不补这一句，`HAL_GetTick()`
恒为 0，任何 `HAL_Delay()` 都会死等；`HAL_Delay()` 等的正是 GetTick 的变化。
HAL 属芯片相关代码，桥接只能放在 `board/`，内核保持零芯片依赖。

## 快速移植

SVCrtOS 可移植到任何 ARM Cortex-M MCU，只需在 `board/` 目录下创建新的芯片移植目录。

1. **创建板级目录**：`board/<你的芯片>/`
2. **创建 `svcrt_board_config.h`**：设置芯片的主频和内存地址
3. **实现 `svcrt_board.c`**：实现 `svcrt_port.h` 中的所有接口函数 + MPU 操作（M3 可跳过）
4. **写中断入口**：`SysTick_Handler` 转 `svcrt_kernel_tick_handler()`（用 STM32 HAL 的板子还要补 `HAL_IncTick()`，见上一节）；`HardFault_Handler` 必须转交内核故障处理，不要 `while(1)` 死循环
5. **移植上下文汇编**：参考 `kernelsrc/port/arm/cortex-m4/svcrt_context.S`（不同内核版本需适配 FPU 处理）
6. **编写板载驱动**：UART、LED 等

**关键：kernelsrc/ 目录无需任何修改！**

详细移植步骤请参考 [移植手册](kernelsrc/porting_manual.md)。

## 外部分区固件（用户态 App / Driver）

除了把任务直接编译进内核，SVCrtOS 还支持把应用和驱动编译为**独立固件**烧录到专属 ROM 分区，由内核调度运行，实现固件解耦与独立升级。

完整示例见 `example/stm32f427/app_sdk/`（应用）、`example/stm32f427/driver_sdk/`（驱动）以及蓝灯端到端示例 `example/stm32f427/BLUE_LED_E2E_README.md`。

集成要点（详见 [SDK 用户手册](kernelsrc/sdk/sdk_user_manual.md) 第三部分）：
1. 镜像装入后由 loader 认领：从镜像头/槽位表取得入口，按槽分配 RAM 块并注册为任务（入口带 Thumb 位 `| 1`）
2. 外部固件启动汇编必须经 C 库入口 `__main` 完成 `.data` 拷贝、`.bss` 清零，否则驱动接口函数指针表为随机值导致 HardFault
3. 内核、各固件的 ROM/RAM 分区地址必须互不重叠

> **安装与布局现状**：App 与驱动共用一个 768 KB 镜像池（F427），池内按 1 KB 粒度贴合放置。
> 安装策略（固定槽位 / 自动选址）与槽位表由**设备端配置区**决定，编译期只给回退默认。
> 工具链：`tools/svcrt_layout.py` 生成配置记录、`tools/svcrt_cfg.py` 经串口在线写入配置区、
> `tools/gen_scatter.py --target image --unit N` 生成各固件的分散加载文件、`tools/pack_app.py`
> 打包 `.svcapp`。人操作走图形界面 `tools/svcrt_host_gui.py`（连接 / 控制台 / 安装 / 布局配置），
> 自动化流程走 `skills/svcrtos/SKILL.md`（见 docs 的《应用安装与调试指南》§11）。
> 交给客户用 MDK 直接烧录调试的路径走**开发槽位表**（裸镜像，不参与回收）。
> 完整语义见 [配置区与安装策略](docs/配置区与安装策略.md) 与
> [应用安装与调试指南](docs/SVCrtOS应用安装与调试指南.md)。

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

## 源码编码约定

- 历史源文件为 **GBK**（保证 Keil 编辑器里中文注释不乱码），近几轮新增模块为 **UTF-8**
- 修改 GBK 文件时按**字节级补丁**插入内容，**不要整文件转码**，否则 Keil 里的中文注释会变乱码
- `*.md` 文档统一 UTF-8
- `tools/gen_api_doc.py` 会先在系统临时目录生成一份 UTF-8 副本再跑 Doxygen，
  不改动仓库内任何源文件

## 自动化调试通道（mdkdebug MCP）

本工程的编译、烧录与上板调试可经外部 MCP 服务自动完成：
[mdk_agent_mcp](https://gitee.com/xw19981010/mdk_agent_mcp.git)。
该服务把 Keil uVision 的 UVSOCK/TCP 通道封装成一组工具，调试过程不必人工操作
Keil 界面，即可完成「编译 -> 烧录 -> 进入调试 -> 运行控制 -> 读回状态」的闭环。

经该通道可用的能力包括：

- 工程构建：编译 / 全量重建 / 烧录，以及「关旧窗口 -> 编烧 -> 重开 -> 进调试」的一体化闭环
- 目标观察：读写内存与外设寄存器、读变量与结构体、CPU 寄存器组、反汇编、源码定位与调用栈
- 运行控制：运行 / 暂停 / 单步 / 复位 / 运行到指定位置，软件断点、条件断点与数据观察点
- 异常排查：SCB 异常寄存器与故障现场、内存地图与内存搜索、函数级与采样级性能剖析
- 串口交互：在宿主机开串口收日志（支持增量读取），并从同一句柄下发 shell 命令，收发一体
- 符号管理：运行期切换 .axf/.map 符号文件，为运行期重定位的 App 侧符号设置全局偏移

本工程的真机验证（内核节拍与调度、应用安装闭环、卸载与空间回收、掉电后的镜像状态等）
有一部分是通过该通道完成的，全程不需要人工在 Keil 里点击操作。
部署方式、工具清单与使用示例见上述仓库的 README。

## 调度表现（F427 实测 trace）

调度器的结构（256 位两级位图就绪集、按需拉 PendSV、抢占 + 时间片）见
[调度器说明](docs/调度器说明.md)。这一节给的是**在真机上量到的表现**：
STM32F427VGTx @96 MHz，DAPLink（SWD 两线，无 SWO）经 mdkdebug 的 SWD trace 通道，
把内核插桩事件搬到宿主机后绘制。原始事件 4.97 s / 18314 条。

> **口径先说清**：trace 插桩本身每趟 PendSV 要多花约 1100~1500 周期。
> 因此**可对外引用的开销数字只看下面第三张图的 ⑦ 幅（插桩关闭口径）**；
> 前两张图用来说明「谁在什么时候跑、切换有多规律」，不要拿图上的 µs 当性能结论。

### 一、整体：4.97 s 内 5153 次切换，间隔无一次跳变

![整体调度时间线与切换间隔](docs/img/sched-overview.png)

- **上**：4.97 s 窗口里 5153 次任务切换的间隔，全部落在两条水平带上——10 µs
  （任务跑完即让出）与 2000 µs（一个节拍）。没有任何一次切换落在带外，也没有长尾。
- **下**：放大到 21 ms（灰色虚线为节拍）。`svcrt_shell_task` 严格每 2 ms 被唤醒一次、
  跑约 10 µs 即回 `idle`；窗口右端 led 与 App 槽位同时到场，一次刷新里跑完三个任务。

### 二、单次切换：两次 PendSV 就把新任务跑起来

![单次切换细节](docs/img/sched-switch-detail.png)

- **上**：`idle → svcrt_shell_task → idle` 的完整一段。两次 PendSV 各占 15.6 µs / 14.0 µs
  （1498 / 1341 周期，**插桩开启口径**），中间 shell 主动让出（`SVCRT_TR_EV_WAIT`）。
- **下**：放大 134 µs。App 任务 `slot5` 被调度进来连续跑 72.7 µs，期间 **3 次 SVCall 陷入内核**
  ——特权隔离路径在真实负载上被走到；最后由 SVC 主动让出，调度器随即切回 shell。

### 三、分布与 A/B：调度器占用压在 0.5% CPU 以内

![统计与优化 A/B](docs/img/sched-stats.png)

| 项 | 数值（F427 @96 MHz） |
|---|---|
| 切换间隔构成 | 10 µs 档 50%、2000 µs 档 50%，两档之外无样本 |
| CPU 时间占用 | `idle` 99.48%、`svcrt_shell_task` 0.51%，App / 驱动 / led 任务合计 < 0.01% |
| PendSV 往返（插桩开） | 1682 趟，中位 1347 周期，min 679 / max 1656 |
| PendSV 往返（插桩关） | 全表线性扫描 710.5 → 两级位图 + 链 **430.9** 周期（−39%） |
| PendSV 占 CPU（插桩关） | 0.768% → **0.448%** |

第三张图的 ⑦ 幅是同一块板、同批 App 镜像、同一 3 s 窗口、用 DWT 窗口差值量出来的 A/B：
就绪集从「全表线性扫描」换成「256 位两级位图 + 每优先级链」后，决策段 627 → 331 周期（−47%），
整趟 PendSV −39%。该组数字全程 **trace 关闭**，不含插桩开销，可对外引用。
复现方式（采集脚本、shell 命令、口径细节）见 [调度器说明](docs/调度器说明.md)。

## 验证状态

已验证到两层证据：**Keil UV4 全量重建**（内核 + 四个示例工程，0 Error）+ **F427 真机**（DAPLink，SWD 两线，USART1 → 主机 COM3）。

真机上已闭环的部分：

| 项 | 证据 |
|----|------|
| 内核节拍与调度 | 心跳任务持续输出（`APP_ALIVE`，`cpu=0%`），周期与超时唤醒正常 |
| 应用安装闭环 | `.svcapp` 经串口装入、上电扫描认领、按槽启动 |
| 驱动安装闭环 | 驱动包（`--type driver`，2164 B，包内 `type = 2`）经**同一个 `install` 窗口**装进统一镜像池：`install: ok, slot 1`、`drv list` 显示 `1 drv RUNNING`、`pool map` 里 app 与 drv 同列一个池；驱动注册的设备与命令（`TEMPDRV` / `temp`）当场可用 |
| 卸载与空间回收 | 卸载作废镜像；最后一个活槽位所在的物理单元空出后扇区才真擦，`pool free` 由 776244 B 回到 786432 B |
| 掉电后的镜像状态 | 复位后能正确区分「有效安装镜像 / 已作废 / 裸镜像」 |
| 裸镜像（开发槽位）路径 | 直接烧到池内的镜像被认领，RAM 窗口 `0x20014000` 绑定正确，开机自启跑完十段自测；`app stop` 退回 `RAW`、`app start` 可再起 |
| 消息队列 | 变长语义与返回值修正后，`mq.send` / `mq.recv` / `mq.recv_timeout` 在真机全部通过 |
| POSIX / Windows 兼容层 | APP_DEMO 第九节自测（堆、pthread 创建/join/返回值、mutex、sem 空判、`usleep`、`CreateThread`/`Wait`、`strcpy_s` 越界）全部 `OK` |
| 设备端配置区 | `tools/svcrt_cfg.py` 的 `write` / `show` / `clear` 三态全部真机跑通：4×128 分块下发、读回 `source: device config region`、坏 CRC 记录被设备拒绝并给出真实原因、`clear` 回退默认。`mode = fixed` 重启后 `pool` 按配置列出固定槽，共享分区表 `layout_mode` 直读与 `cfg show` 一致 |
| F401 移植 | 双板同源编译通过（F401 `Code=46526`） |
| 节拍计数器 32 位回绕 | 定向注入使 `svcrt_kernel_tick` 从 `0xFFFFC000` 起算并跨过回绕：`t=` 由 `2147478038 ms` 跳回小值，两侧 `APP_ALIVE` 间隔精确 5.000 s，`fault` 无记录、`sched` 一致。该次实测牵出并修掉了延时链插入排序与到期判定在回绕处不一致的缺陷（见 [docs/调度器说明.md](docs/调度器说明.md) §2.3） |
| VFS 端口并发锁 | 两个写者 + 一个只读的第三方读者同时压同一条路径：读者必须看到两种**完整**载荷（`vfs.conc_saw_both`），两次干净开机自检 `pass=128 fail=0`，`sched` 一致、`fault` 无记录（见 [docs/VFS路径命名空间.md](docs/VFS路径命名空间.md)） |
| 常驻安装器的固定槽覆盖安装（`SHELL_ENABLE=0`） | `INSTALLER_FIXED_SLOT=0` 变体上板：上电先 `fixed slot 0 holds a running image (slot 0): stopping it first` → `fixed slot 0 cleared, send the file now`；覆盖安装不再出现 `err -4`，镜像装入后正常自启；同一上电周期内第二帧被干净拒绝（`one image already installed this boot: reboot before sending another one` + `drained 1932 B of the rejected frame`） |
| 运行中无缝升级 | 两个小程序（`SEAMLESS_V1` / `SEAMLESS_V2`）交接控制权：V1 独占 10.7 ms/轮、V2 接管后 5.9 ms/轮；交接空洞 27/31/224/407 ms；迭代计数逐字连续（`it=994` → `994`）；受控量 `pv` 未回零；`guard starves=0`；交接前后 `fault` 行数不变。反例侧：旧版抢不回控制权、新版超时后 5000 ms 内无人释放、残留请求已撤回（见 [docs/无缝升级例程.md](docs/无缝升级例程.md)） |

仍未上板验证或未闭环的项：

- **真实以太网收发**：板级无可用 PHY（`board/stm32f427/` 下没有 PHY 驱动、MDIO 读写、`eth_` 接口与 RMII 引脚分配），网络路径跑的是回环网卡，未做真实链路收发验证（见 [docs/lwIP协议栈接入.md](docs/lwIP协议栈接入.md) §1）
- **故障恢复的「连续重启 3 次禁用」**策略未做专项真机验证
- **固定槽位模式下一次真实 `install <slot>` 的落点复验**（槽表已生效并被 `pool` 列出，但「装进去正好落在配置地址」这一环还没跑）

文档索引见 [docs/README.md](docs/README.md)。

---

## 文档地图

| 我想…… | 看这份 |
|---|---|
| 把 App / 驱动调试起来、装到板子上、处理崩溃 | [SVCrtOS应用安装与调试指南](docs/SVCrtOS应用安装与调试指南.md)（操作手册，先读这份） |
| 搞懂安装策略、设备端配置区、槽位与 RAM 窗口 | [配置区与安装策略](docs/配置区与安装策略.md) |
| 查所有文档的索引 | [docs/README.md](docs/README.md) |
| 看调度器怎么实现、实测代价账怎么算 | [调度器说明](docs/调度器说明.md) |
| 看内存保护怎么起效、App 越权会怎样 | [内存保护与App越权验证](docs/内存保护与App越权验证.md) |
| 把 Linux / Windows 上的 C 程序搬过来 | [POSIX与Windows兼容层说明](docs/POSIX与Windows兼容层说明.md) |
| 写 socket / select 程序 | [socket与select兼容层](docs/socket与select兼容层.md) |
| 加一个「放在文件系统里、想跑就跑」的小程序 | [小程序设计](docs/小程序设计.md) |
| 做运行中升级 | [无缝升级例程](docs/无缝升级例程.md) |
| 换一颗芯片 | [SVCrtOS移植手册](SVCrtOS移植手册.md) |
| 查 API 签名与参数 | [api/SVCrtOS_API参考.md](docs/api/SVCrtOS_API参考.md) |
| 让自动化流程（含 AI agent 工作流）驱动这块板子 | [skills/svcrtos/SKILL.md](skills/svcrtos/SKILL.md) |

## 相关开源仓库

| 仓库 | 关系 |
|---|---|
| [svcrtos_new](https://gitee.com/xw19981010/svcrtos_new) | 本仓库：内核 + 板级移植 + 示例工程 |
| [ark-shell](https://gitee.com/xw19981010/ark-shell.git) | 内核控制台的上游组件（`kernelsrc/shell/` 为原样引入 + 一个 SVCrtOS 平台适配层） |
| [ark_vfs](https://gitee.com/xw19981010/ark_vfs) | 零依赖 VFS，文件路径命名空间的上游（`kernelsrc/components/ark_vfs/` 为同步副本） |
| [mdk_agent_mcp](https://gitee.com/xw19981010/mdk_agent_mcp.git) | 自动化编译 / 烧录 / 上板调试通道 |

## 许可与版本

- 许可证：[MIT](LICENSE)（Copyright © 2026 春雫）
- 当前版本：**0.0.2**。版本以 git tag 为唯一记法，列表见 <https://gitee.com/xw19981010/svcrtos_new/tags>
- **0.0.2**：动态安装主线补齐——小程序（文件系统里的第三种用户态应用）、运行中无缝升级、socket / select 与 lwIP 回环、文件类 POSIX；MPU 越权与 FPU 非特权使用上板验证；调度器就绪集改 256 位两级位图（单趟 PendSV −39%）
- **0.0.1**：首个可安装应用的发布——自动选址与固定槽位两种安装模式可配置，图形上位机与项目内操作手册就位
- 本文件为简体中文；英文版见 [readme_en.md](readme_en.md)
