# SVCrtOS

SVCrtOS 是一个面向 ARM Cortex-M 系列微控制器的安全实时操作系统（RTOS），采用 SVC（Supervisor Call）实现特权级隔离，利用 MPU 进行内存保护，支持多任务分区调度和统一设备驱动框架。

## 核心特性

- **SVC 特权隔离**：用户任务通过 SVC 系统调用进入内核态，内核态代码受硬件保护，用户任务无法直接访问
- **MPU 内存保护**：利用 Cortex-M MPU 实现任务间 ROM/RAM 空间隔离，防止越界访问
- **优先级 + 时间片调度**：支持多优先级抢占和同优先级时间片轮转
- **事件驱动机制**：轻量级事件同步原语，支持超时等待
- **信号量与互斥锁**：计数信号量 + 带优先级继承的互斥锁，防止优先级反转
- **设备驱动框架**：统一设备 I/O 接口（open/close/read/write/ctrl），支持内置驱动和可安装驱动
- **FIFO 缓冲区**：环形缓冲区，用于内核和驱动间的数据传输
- **栈溢出检测**：通过 PSP 越界检测任务栈溢出
- **CPU 负载统计**：实时统计 CPU 空闲率
- **FPU 支持**：Cortex-M4F/M7 浮点寄存器（S16-S31）按需自动保存/恢复
- **双 SDK 架构**：独立的应用 SDK 和驱动 SDK，支持编译为独立分区固件
- **内核与芯片解耦**：采用 RT-Thread 类似的分层架构，内核零芯片依赖，移植只需修改 board/ 目录

## 支持 CPU 架构

| 架构 | FPU | MPU | 备注 |
|------|-----|-----|------|
| Cortex-M3 | - | - | 最小配置 |
| Cortex-M4 | 有 | 有 | 推荐使用 |
| Cortex-M7 | 有 | 有 | 高性能 |

## 目录结构

```
SVCRTOS/
├── kernelsrc/                      # ★ 内核源码（零芯片依赖）
│   ├── include/                    # 内核头文件
│   │   ├── svcrt.h                 # 应用 API
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
├── kernelsrc/port/arm/cortex-m4/   # ★ CPU 架构移植层（上下文切换/启动）
│   ├── svcrt_context.S             # PendSV 上下文切换汇编（含 FPU 寄存器）
│   └── svcrt_port.c                # 任务栈帧初始化（含 EXC_RETURN）
│
└── example/                        # 工程示例
    └── stm32f427/                  # MDK 工程
```

**架构解耦原则：**
- `kernelsrc/` = 纯内核，不包含任何芯片头文件，不直接操作任何硬件寄存器
- `board/` = 芯片相关，移植到新芯片只需创建新的 `board/<芯片>/` 目录
- `svcrt_port.h` = 内核与硬件的唯一耦合点，纯函数声明

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

## 驱动开发接口

驱动开发者只需包含 `svcrt_driver_sdk.h`，实现 `svcrt_dev_drv_t` 接口后调用 `svcrt_drv_register()` 注册：

```c
static svcrt_dev_drv_t my_drv = {
    my_drv_open,
    my_drv_close,
    my_drv_read,
    my_drv_write,
    my_drv_ctrl
};

int32 my_drv_install(void)
{
    return svcrt_drv_register("MYDEV", &my_drv, 0);
}
```

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
| `SVCRT_USE_CPU_LOAD` | 1 | CPU 负载统计开关 |
| `SVCRT_USE_STACK_CHECK` | 1 | 栈溢出检测开关 |
| `SVCRT_SHARE_MEM_ADDR` | 0x20028000 | 共享内存地址（板级配置覆盖） |
| `SVCRT_SHARE_MEM_SIZE` | 0x8000 | 共享内存大小（板级配置覆盖） |
| `SVCRT_SYSTEM_CLOCK_HZ` | 168000000 | 系统主频（板级配置覆盖） |

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
