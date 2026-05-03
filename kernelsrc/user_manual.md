# SVCrtOS 使用手册

## 1. 系统概述

SVCrtOS 是一个面向ARM Cortex-M系列MCU的实时操作系统，核心特性包括：

- **优先级+时间片混合调度**：高优先级抢占 + 同优先级轮转
- **MPU内存保护**：任务间内存隔离（M4/M7），M3可无MPU运行
- **事件驱动**：任务间同步通信
- **设备驱动框架**：统一设备接口 + 动态驱动注册
- **SVC系统调用**：用户态通过SVC陷入内核态访问资源
- **可配置**：通过 `svcrt_config.h` 开关所有功能

### 1.1 系统架构

```
┌──────────────────────────────────────────────┐
│              应用分区 (App SDK)               │
│  ┌─────────┐  ┌─────────┐  ┌─────────┐      │
│  │  App 1  │  │  App 2  │  │  App N  │      │
│  │ (用户态) │  │ (用户态) │  │ (用户态) │      │
│  └────┬────┘  └────┬────┘  └────┬────┘      │
│       │SVC         │SVC         │SVC         │
├───────┼────────────┼────────────┼────────────┤
│       └────────────┼────────────┘            │
│            SVCrtOS 内核 (内核态)              │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐     │
│  │ 调度器   │ │ 事件模块 │ │ 设备框架 │     │
│  └──────────┘ └──────────┘ └─────┬────┘     │
│       │                           │          │
│  ┌────┴────┐              ┌──────┴──────┐   │
│  │ MPU模块 │              │ Driver SDK  │   │
│  └─────────┘              │ (动态注册)  │   │
│                           └─────────────┘   │
├──────────────────────────────────────────────┤
│            svcrt_port.h (移植层)              │
├──────────────────────────────────────────────┤
│              目标MCU硬件                      │
└──────────────────────────────────────────────┘
```

---

## 2. 快速开始

### 2.1 配置系统

编辑 `svcrt_config.h`：

```c
#define SVCRT_CPU_ARCH         SVCRT_ARCH_CORTEX_M4
#define SVCRT_TASK_MAX_NUM     (7)
#define SVCRT_TICK_PERIOD_US   (500)
#define SVCRT_EVENT_NUM        (10)
```

### 2.2 实现移植接口

实现 `svcrt_port.h` 中定义的回调函数（详见移植手册）。

### 2.3 配置任务

在 `appconfig.c` 中配置任务参数：

```c
__weak int32 svcrt_app_count = 2;

__weak svcrt_app_cfg_t svcrt_app_cfg_table[] =
{
    {
        0x20000000,     // ram_start
        0x1000,         // ram_size
        0x400,          // stack_size
        0x08020000,     // rom_start
        0x20000,        // rom_size
        1000,           // period
        10,             // priority
        0               // shm_attri
    },
    {
        0x20001000,
        0x1000,
        0x400,
        0x08040000,
        0x20000,
        1000,
        10,
        0
    }
};
```

### 2.4 注册驱动

在 `svcrt_port_board_init()` 中注册驱动：

```c
void svcrt_port_board_init(void)
{
    led_drv_install();
    uart_drv_install();
}
```

---

## 3. 任务管理

### 3.1 任务状态

```
                 ┌──────────┐
          创建 → │  READY   │ ← 周期到期/事件触发
                 └────┬─────┘
                      │ 调度器选中
                      ▼
                 ┌──────────┐
                 │ RUNNING  │
                 └────┬─────┘
                      │ svcrt_task_wait / svcrt_task_wait_period
                      ▼
                 ┌──────────┐
                 │   WAIT   │
                 └────┬─────┘
                      │ HardFault / svcrt_task_kill
                      ▼
                 ┌──────────┐
                 │ INVALID  │
                 └──────────┘
```

### 3.2 任务API

| API | 说明 | 示例 |
|-----|------|------|
| `svcrt_task_wait(ms)` | 等待指定毫秒 | `svcrt_task_wait(500);` |
| `svcrt_task_wait_period()` | 等待下一周期 | `svcrt_task_wait_period();` |
| `svcrt_task_delay(us)` | 微秒级忙等 | `svcrt_task_delay(100);` |
| `svcrt_task_kill()` | 终止当前任务 | `svcrt_task_kill();` |

### 3.3 调度策略

- **优先级抢占**：高优先级任务始终优先于低优先级
- **同优先级轮转**：同优先级任务按时间片轮转（基于touch_tick）
- **周期调度**：每个任务有独立周期，周期到期自动唤醒

### 3.4 任务配置参数

| 参数 | 说明 |
|------|------|
| `ram_start` | 任务RAM起始地址 |
| `ram_size` | 任务RAM大小 |
| `stack_size` | 任务栈大小 |
| `rom_start` | 任务代码起始地址 |
| `rom_size` | 任务代码大小 |
| `period` | 任务周期（tick数） |
| `priority` | 任务优先级（0=最高） |
| `shm_attri` | 共享内存访问属性 |

---

## 4. 设备驱动

### 4.1 设备操作API

| API | 说明 | 返回值 |
|-----|------|--------|
| `svcrt_dev_open(name, param)` | 打开设备 | 设备句柄(≥0)或-1 |
| `svcrt_dev_read(handle, buf, len)` | 读取数据 | 实际读取字节数 |
| `svcrt_dev_write(handle, buf, len)` | 写入数据 | 实际写入字节数 |
| `svcrt_dev_ctrl(handle, code, value)` | 设备控制 | 操作结果 |

### 4.2 使用示例

```c
int32 com1 = svcrt_dev_open("COM1", 115200);
if(com1 >= 0)
{
    uint8 buf[64];
    int32 len = svcrt_dev_read(com1, buf, 64);
    svcrt_dev_write(com1, buf, len);
}
```

### 4.3 开发自定义驱动（Driver SDK）

```c
#include "svcrt_driver_sdk.h"

static svcrt_dev_hdr_t* my_open(uint32 devid, uint32 param) { ... }
static int32 my_close(svcrt_dev_hdr_t *obj) { ... }
static int32 my_read(svcrt_dev_hdr_t *obj, uint8 *data, int32 len) { ... }
static int32 my_write(svcrt_dev_hdr_t *obj, uint8 *data, int32 len) { ... }
static int32 my_ctrl(svcrt_dev_hdr_t *obj, uint32 code, uint32 value) { ... }

static svcrt_dev_drv_t my_drv = {
    my_open, my_close, my_read, my_write, my_ctrl
};

void my_driver_init(void)
{
    svcrt_drv_register("MYDEV", &my_drv, 0);
}
```

### 4.4 标准控制码

| 控制码 | 值 | 说明 |
|--------|-----|------|
| `SVCRT_DEV_CTRL_OPEN` | 0x0001 | 打开设备 |
| `SVCRT_DEV_CTRL_CLOSE` | 0x0002 | 关闭设备 |
| `SVCRT_DEV_CTRL_SET_BAUD` | 0x0010 | 设置波特率 |
| `SVCRT_DEV_CTRL_SET_MODE` | 0x0011 | 设置模式 |
| `SVCRT_DEV_CTRL_GET_STATUS` | 0x0020 | 获取状态 |
| `SVCRT_DEV_CTRL_RESET` | 0x0030 | 复位设备 |

---

## 5. 事件机制

### 5.1 事件API

| API | 说明 |
|-----|------|
| `svcrt_event_create(name)` | 创建事件 |
| `svcrt_event_wait(handle, timeout)` | 等待事件 |
| `svcrt_event_set(handle)` | 触发事件 |

### 5.2 使用示例

**生产者任务：**
```c
int32 evt = svcrt_event_create("data_ready");

while(1)
{
    produce_data();
    svcrt_event_set(evt);
    svcrt_task_wait(100);
}
```

**消费者任务：**
```c
int32 evt = svcrt_event_create("data_ready");

while(1)
{
    svcrt_event_wait(evt, 0);
    consume_data();
}
```

---

## 6. 系统信息

### 6.1 系统信息API

| API | 说明 |
|-----|------|
| `svcrt_get_time_ms()` | 获取系统运行时间(毫秒) |
| `svcrt_get_cpu_usage()` | 获取CPU负载率 |

### 6.2 CPU负载统计

CPU负载统计通过 `SVCRT_USE_CPU_LOAD` 配置开关控制。开启后，内核每1024个tick采样一次CPU使用情况。

---

## 7. 内存保护（MPU）

### 7.1 MPU功能说明

当 `SVCRT_USE_MPU=1` 时：
- 每个任务只能访问自己的RAM区域和ROM区域
- 任务无法修改内核或其他任务的内存
- 非法内存访问触发MemManage_Handler，任务被标记为INVALID

### 7.2 无MPU运行

当 `SVCRT_USE_MPU=0` 时（如Cortex-M3）：
- 所有任务共享全部内存空间
- 没有内存隔离保护
- HardFault仍然会杀死出错任务
- `SVCRT_USE_PRIV` 自动关闭

---

## 8. App SDK 开发指南

### 8.1 应用开发流程

1. 包含 `svcrt.h` 头文件
2. 实现 `AppMain()` 函数
3. 在 `AppMain()` 中使用OS API
4. 编译为独立固件，烧录到指定分区

### 8.2 App SDK 文件清单

| 文件 | 用途 |
|------|------|
| `svcrt.h` | 应用API入口头文件 |
| `svcrt_types.h` | 基础类型定义 |
| `svcrt_app_config.h` | 分区配置结构 |
| `svcrt_oslib.c` | SVC系统调用封装 |
| `svcrt_app_main.c` | 入口模板（含AppMain弱定义） |
| `svcrt_app_start.s` | 应用启动汇编 |

### 8.3 完整应用示例

```c
#include "svcrt.h"

void AppMain(void)
{
    int32 com1 = svcrt_dev_open("COM1", 115200);
    int32 led  = svcrt_dev_open("LED", 0);
    int32 evt  = svcrt_event_create("sync");

    while(1)
    {
        uint8 data[64];
        int32 len = svcrt_dev_read(com1, data, 64);

        if(len > 0)
        {
            svcrt_dev_write(com1, data, len);
        }

        svcrt_event_set(evt);
        svcrt_task_wait(100);
    }
}
```

---

## 9. Driver SDK 开发指南

### 9.1 驱动开发流程

1. 包含 `svcrt_driver_sdk.h` 头文件
2. 实现 `svcrt_dev_drv_t` 中的5个函数
3. 调用 `svcrt_drv_register()` 注册驱动
4. 应用通过 `svcrt_dev_open()` 使用驱动

### 9.2 驱动接口说明

```c
typedef struct {
    svcrt_drv_open_func    drv_open;    // 打开设备，返回设备对象
    svcrt_drv_close_func   drv_close;   // 关闭设备
    svcrt_drv_read_func    drv_read;    // 读取数据
    svcrt_drv_write_func   drv_write;   // 写入数据
    svcrt_drv_ctrl_func    drv_ctrl;    // 设备控制
} svcrt_dev_drv_t;
```

### 9.3 动态注册/注销

```c
// 注册驱动
int32 ret = svcrt_drv_register("SPI1", &spi_drv, 0);
// ret: 0=成功, -1=设备表满, -2=参数无效

// 注销驱动
int32 ret = svcrt_drv_unregister("SPI1");
// ret: 0=成功, -1=设备未找到

// 查询已注册设备数
int32 count = svcrt_drv_get_count();
```

---

## 10. 配置参考

### 10.1 svcrt_config.h 完整配置项

```c
// CPU架构（必须选择一项）
#define SVCRT_CPU_ARCH            SVCRT_ARCH_CORTEX_M4

// 自动派生配置（通常无需手动修改）
#define SVCRT_USE_FPU             1    // 浮点单元
#define SVCRT_USE_MPU             1    // 内存保护
#define SVCRT_USE_PRIV            1    // 特权分离

// 调度器参数
#define SVCRT_TASK_MAX_NUM        (7)   // 最大任务数
#define SVCRT_TICK_PERIOD_US      (500) // 滴答周期(微秒)
#define SVCRT_EVENT_NUM           (10)  // 最大事件数
#define SVCRT_MAX_EVENT_WAITERS   (4)   // 每事件最大等待者

// 设备框架
#define SVCRT_DEV_MAX_NUM         (8)   // 最大设备数

// 可选功能
#define SVCRT_USE_CPU_LOAD        1     // CPU负载统计
#define SVCRT_USE_STACK_CHECK     1     // 栈溢出检测
#define SVCRT_STACK_END_FLAG      (0xED01) // 栈底标记

// 内存配置
#define SVCRT_SHARE_MEM_ADDR      (0x20028000) // 共享内存地址
#define SVCRT_SHARE_MEM_SIZE      (0x8000)     // 共享内存大小
#define SVCRT_SYSTEM_CLOCK_HZ     (168000000)  // 系统主频
```

### 10.2 Cortex-M3 最小配置

```c
#define SVCRT_CPU_ARCH         SVCRT_ARCH_CORTEX_M3
#define SVCRT_TASK_MAX_NUM     (3)
#define SVCRT_TICK_PERIOD_US   (1000)
#define SVCRT_EVENT_NUM        (4)
#define SVCRT_MAX_EVENT_WAITERS (2)
#define SVCRT_DEV_MAX_NUM      (4)
#define SVCRT_USE_CPU_LOAD     0
#define SVCRT_USE_STACK_CHECK  0
```
