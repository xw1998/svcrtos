# SVCrtOS

SVCrtOS 是一款面向 ARM Cortex-M 系列微控制器的实时操作系统（RTOS），基于 SVC（Supervisor Call）指令实现特权级隔离，支持 MPU 内存保护，适用于安全关键型嵌入式应用。

## 核心特性

- **SVC 特权隔离** — 应用程序运行在非特权模式，通过 SVC 指令陷入内核态访问系统服务，实现特权级分离
- **MPU 内存保护** — 利用 Cortex-M MPU 为每个任务划分独立的 ROM/RAM 区域，防止任务间内存越界
- **优先级 + 时间片调度** — 基于优先级的抢占式调度，同优先级任务按时间片轮转
- **周期任务支持** — 原生支持周期性任务调度，适合控制类应用
- **设备驱动框架** — 统一的设备 I/O 接口（open/read/write/ctrl），支持动态驱动注册
- **事件机制** — 轻量级事件同步原语，支持超时等待
- **FIFO 缓冲** — 内核级环形缓冲区，用于驱动层收发缓冲
- **栈溢出检测** — 运行时栈底标志检测，及时发现栈溢出
- **CPU 负载统计** — 实时统计 CPU 空闲率
- **FPU 支持** — Cortex-M4F/M7 浮点上下文自动保存恢复
- **双 SDK 架构** — 分别为应用开发和驱动开发提供独立 SDK

## 支持的 CPU 架构

| 架构 | FPU | MPU | 状态 |
|------|-----|-----|------|
| Cortex-M3 | - | - | 支持 |
| Cortex-M4 | ? | ? | 支持（默认） |
| Cortex-M7 | ? | ? | 规划中 |

## 目录结构

```
kernelsrc/
├── app/                        # 应用层配置与入口
│   ├── appconfig.c             # 分区配置模板（RAM/ROM/周期/优先级）
│   ├── appstart.s              # 应用启动汇编入口
│   └── oslib.c                 # 应用层 OS 接口封装（SVC 调用）
├── drivers/                    # 板级驱动
│   ├── devsio.c                # 板级设备注册表（静态方式）
│   ├── drvled.c/h              # LED 指示灯驱动
│   └── drvuart.c/h             # 串口驱动（中断收发 + FIFO 缓冲）
├── include/                    # 内核头文件
│   ├── svcrt.h                 # 应用层 API（面向 App SDK 用户）
│   ├── svcrt_config.h          # 集中配置文件（功能开关/参数）
│   ├── svcrt_port.h            # 硬件移植接口
│   ├── svcrt_types.h           # 应用层基础类型定义
│   ├── svcrt_def.h             # 内核公共定义（句柄宏/SVC编号）
│   ├── svcrt_task.h            # 任务管理模块（内核内部）
│   ├── svcrt_event.h           # 事件模块（内核内部）
│   ├── svcrt_fifo.h            # FIFO 模块（内核内部）
│   ├── svcrt_mpu.h             # MPU 模块（内核内部）
│   ├── svcrt_dev.h             # 设备驱动框架（内核内部）
│   └── svcrt_cfg.h             # 任务配置加载（内核内部）
├── libcpu/                     # CPU 架构移植
│   └── arm/
│       ├── cortex-m3/          # Cortex-M3 上下文切换与移植
│       └── cortex-m4/          # Cortex-M4 上下文切换与移植
├── sdk/                        # 开发工具包
│   ├── app_sdk/                # 应用开发 SDK
│   │   ├── examples/           # 应用示例
│   │   ├── svcrt_app_config.h  # 应用配置结构
│   │   ├── svcrt_app_main.c    # 应用入口模板
│   │   ├── svcrt_app_start.s   # 应用启动汇编
│   │   └── svcrt_oslib.c       # SVC 接口封装（ARM Compiler __svc）
│   └── driver_sdk/             # 驱动开发 SDK
│       ├── examples/           # 驱动示例
│       ├── svcrt_driver_bridge.c # 驱动注册桥接层
│       └── svcrt_driver_sdk.h  # 驱动开发接口
└── src/                        # 内核源码
    ├── svcrt_init.c            # 系统启动与初始化
    ├── svcrt_task.c            # 任务管理与调度器
    ├── svcrt_event.c           # 事件模块实现
    ├── svcrt_fifo.c            # FIFO 环形缓冲实现
    ├── svcrt_mpu.c             # MPU 内存保护实现
    ├── svcrt_dev.c             # 设备驱动框架实现（动态注册）
    └── svcrt_cfg.c             # 任务配置加载
```

## 系统架构

```
┌─────────────────────────────────────────────────┐
│                  应用分区 (Unprivileged)          │
│  ┌───────────┐  ┌───────────┐  ┌───────────┐   │
│  │  App Task1 │  │  App Task2 │  │  App TaskN │   │
│  │  (svcrt.h) │  │  (svcrt.h) │  │  (svcrt.h) │   │
│  └─────┬─────┘  └─────┬─────┘  └─────┬─────┘   │
│        │  SVC 0x10~0x13       │              │    │
├────────┼──────────────────────┼──────────────┼────┤
│        ▼                      ▼              ▼    │
│                  内核 (Privileged)                │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐         │
│  │  调度器   │ │  事件模块 │ │  设备框架 │         │
│  └──────────┘ └──────────┘ └──────────┘         │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐         │
│  │  MPU 模块 │ │  FIFO    │ │  栈检测   │         │
│  └──────────┘ └──────────┘ └──────────┘         │
├─────────────────────────────────────────────────┤
│              硬件移植层 (svcrt_port.h)            │
│         Cortex-M3 / M4 / M7                     │
└─────────────────────────────────────────────────┘
```

## 应用层 API

应用程序通过 `svcrt.h` 访问操作系统服务，所有调用通过 SVC 指令陷入内核态执行：

### 任务管理

| API | 说明 |
|-----|------|
| `svcrt_task_wait(ms)` | 挂起当前任务指定毫秒 |
| `svcrt_task_wait_period()` | 挂起至下一周期起点 |
| `svcrt_task_delay(us)` | 微秒级忙等延时 |
| `svcrt_task_kill()` | 终止当前任务 |

### 设备操作

| API | 说明 |
|-----|------|
| `svcrt_dev_open(name, param)` | 打开设备，返回句柄 |
| `svcrt_dev_read(handle, buf, len)` | 从设备读取数据 |
| `svcrt_dev_write(handle, buf, len)` | 向设备写入数据 |
| `svcrt_dev_ctrl(handle, code, value)` | 设备控制命令 |

### 事件与系统信息

| API | 说明 |
|-----|------|
| `svcrt_event_create(name)` | 创建命名事件 |
| `svcrt_event_wait(handle, timeout)` | 等待事件（支持超时） |
| `svcrt_event_set(handle)` | 触发事件 |
| `svcrt_get_time_ms()` | 获取系统运行时间（ms） |
| `svcrt_get_cpu_usage()` | 获取 CPU 负载率 |

## 驱动开发

驱动开发者使用 `svcrt_driver_sdk.h`，实现 `svcrt_dev_drv_t` 接口并通过 `svcrt_drv_register()` 注册：

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

## 配置说明

所有功能开关和参数集中在 `svcrt_config.h` 中管理：

| 配置项 | 默认值 | 说明 |
|--------|--------|------|
| `SVCRT_CPU_ARCH` | `SVCRT_ARCH_CORTEX_M4` | CPU 架构选择 |
| `SVCRT_USE_FPU` | 1 (M4/M7) | 浮点单元使能 |
| `SVCRT_USE_MPU` | 1 (M4/M7) | MPU 内存保护使能 |
| `SVCRT_USE_PRIV` | 1 (依赖MPU) | 特权模式分离 |
| `SVCRT_TASK_MAX_NUM` | 7 | 最大任务数 |
| `SVCRT_TICK_PERIOD_US` | 500 | 系统滴答周期（微秒） |
| `SVCRT_EVENT_NUM` | 10 | 最大事件数 |
| `SVCRT_DEV_MAX_NUM` | 8 | 最大设备数 |
| `SVCRT_USE_CPU_LOAD` | 1 | CPU 负载统计使能 |
| `SVCRT_USE_STACK_CHECK` | 1 | 栈溢出检测使能 |
| `SVCRT_SHARE_MEM_ADDR` | 0x20028000 | 共享内存基地址 |
| `SVCRT_SHARE_MEM_SIZE` | 0x8000 | 共享内存大小 |
| `SVCRT_SYSTEM_CLOCK_HZ` | 168000000 | 系统主频 |

## 移植指南

将 SVCrtOS 移植到新的 MCU 平台，需要实现 `svcrt_port.h` 中定义的接口：

1. **包含目标 MCU 头文件** — 替换 `#include "stm32f4xx.h"`
2. **实现中断控制宏** — `SVCRT_DISABLE_IRQ()` / `SVCRT_ENABLE_IRQ()`
3. **实现任务切换触发** — `SVCRT_SWITCH_TASK()` 设置 PendSV 挂起位
4. **实现 CPU 指令封装** — WFI/WFE/ISB/DSB/DMB/NOP
5. **实现栈指针与控制寄存器操作** — PSP/CONTROL 读写
6. **实现移植层回调函数**：
   - `svcrt_port_board_init()` — 板卡早期初始化（FPU 使能等）
   - `svcrt_port_irq_init()` — 中断优先级配置
   - `svcrt_port_start_timer()` — 启动系统定时器
   - `svcrt_port_enable_fpu()` — FPU 使能
   - `svcrt_port_set_idle_mpu()` — 后台任务 MPU 保护

7. **适配上下文切换汇编** — 修改 `libcpu/` 下对应架构的 `context_rvds.S`

## 启动流程

```
main()
  ├── svcrt_port_board_init()      // 硬件早期初始化
  ├── svcrt_cfg_load()             // 加载任务配置
  ├── svcrt_port_irq_init()        // 中断优先级配置
  ├── svcrt_kernel_init()          // 内核模块初始化（事件/MPU/设备）
  ├── svcrt_port_start_timer()     // 启动 SysTick
  └── svcrt_start_idle()           // 切换到 PSP，进入非特权模式
        └── WFI 循环               // 后台空闲任务
```

## 命名规范

SVCrtOS 采用统一的命名规范，参照 FreeRTOS / RT-Thread 风格：

| 类别 | 前缀 | 示例 |
|------|------|------|
| 公开 API | `svcrt_模块_动作` | `svcrt_task_wait()`, `svcrt_dev_open()` |
| 内核内部函数 | `svcrt_模块_动作_internal` | `svcrt_task_wait_internal()` |
| 调度器 | `svcrt_sched_动作` | `svcrt_sched_next()`, `svcrt_sched_activate()` |
| 移植层 | `svcrt_port_动作` | `svcrt_port_board_init()` |
| 类型 | `svcrt_模块_t` | `svcrt_task_t`, `svcrt_fifo_t`, `svcrt_dev_drv_t` |
| 枚举值 | `SVCRT_模块_状态` | `SVCRT_TASK_READY`, `SVCRT_TASK_WAIT` |
| 宏定义 | `SVCRT_大写描述` | `SVCRT_FIFO_MAGIC`, `SVCRT_DEV_HANDLE_FLAG` |
| 配置项 | `SVCRT_USE_功能` | `SVCRT_USE_FPU`, `SVCRT_USE_MPU` |
