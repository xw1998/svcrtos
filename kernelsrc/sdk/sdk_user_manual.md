# SVCrtOS SDK 用户手册

## 1. 概述

SVCrtOS 提供两套独立的SDK，分别面向**应用开发者**和**驱动开发者**：

| SDK | 目标用户 | 核心文件 | 依赖 | 编译模式 |
|-----|---------|---------|------|---------|
| **App SDK** | 应用程序开发者 | `svcrt.h` | 无MCU依赖 | 用户态（SVC调用） |
| **Driver SDK** | 设备驱动开发者 | `svcrt_driver_sdk.h` | 无MCU依赖 | 内核态/用户态 |

两套SDK均**不依赖任何MCU头文件或内核内部头文件**，开发者只需包含一个头文件即可开始开发。

---

## 第一部分：App SDK

### 2. App SDK 简介

App SDK 是面向SVCrtOS应用程序（分区）的开发工具包。应用程序运行在用户态，通过SVC系统调用与内核交互，无法直接访问硬件寄存器或内核数据结构。

**设计理念：**
- 应用程序是"可安装"的独立固件，烧录到指定ROM分区即可运行
- 应用程序之间通过共享内存和事件机制通信
- 所有硬件访问通过设备驱动框架间接完成

### 3. App SDK 文件清单

```
sdk/app_sdk/
├── svcrt.h              ← 应用API入口头文件（唯一需要包含的文件）
├── svcrt_types.h        ← 基础类型定义（被svcrt.h自动包含）
├── svcrt_app_config.h   ← 分区配置结构定义
├── svcrt_oslib.c        ← SVC系统调用封装实现
├── svcrt_app_main.c     ← 应用入口模板（含AppMain弱定义）
├── svcrt_app_start.s    ← 应用启动汇编入口
└── examples/
    └── example_app.c    ← 完整示例应用
```

### 4. 快速开始：5步开发一个应用

#### 步骤1：创建工程

新建C文件，包含SVCrtOS App SDK头文件：

```c
#include "svcrt.h"
```

#### 步骤2：实现 AppMain()

`AppMain()` 是应用程序的入口函数，相当于普通C程序的 `main()`：

```c
void AppMain(void)
{
    // 你的应用逻辑
}
```

#### 步骤3：使用OS API

在 `AppMain()` 中调用OS接口：

```c
void AppMain(void)
{
    int32 led = svcrt_dev_open("LED", 0);

    while(1)
    {
        uint8 val = 1;
        svcrt_dev_write(led, &val, 1);
        svcrt_task_wait(500);

        val = 0;
        svcrt_dev_write(led, &val, 1);
        svcrt_task_wait(500);
    }
}
```

#### 步骤4：配置分区参数

在 `svcrt_app_config.h` 中定义分区资源需求：

```c
svcrt_app_cfg_t svcrt_app_cfg_table[] = {
    {
        .ram_start       = 0x20000000,   // RAM起始地址
        .ram_size        = 0x1000,       // RAM大小(4KB)
        .stack_size      = 0x400,        // 栈大小(1KB)
        .rom_start       = 0x08020000,   // ROM起始地址
        .rom_size        = 0x20000,      // ROM大小(128KB)
        .period          = 1000,         // 任务周期(1秒)
        .priority        = 10,           // 优先级
        .shm_attri       = 0             // 共享内存访问属性
    }
};
int32 svcrt_app_count = 1;
```

#### 步骤5：编译与烧录

将应用编译为独立固件，链接到分区配置中指定的ROM地址，烧录即可。

### 5. App SDK API 详解

#### 5.1 设备操作API

##### `svcrt_dev_open` - 打开设备

```c
int32 svcrt_dev_open(char* name, uint32 param);
```

| 参数 | 说明 |
|------|------|
| `name` | 设备名称字符串，如 `"COM1"`, `"LED"`, `"SPI1"` |
| `param` | 打开参数，具体含义由驱动定义（如波特率） |

| 返回值 | 说明 |
|--------|------|
| ≥ 0 | 设备句柄，后续操作使用 |
| -1 | 设备未找到或打开失败 |

**示例：**
```c
int32 com1 = svcrt_dev_open("COM1", 115200);  // 以115200波特率打开串口1
int32 led  = svcrt_dev_open("LED", 0);         // 打开LED设备
int32 spi  = svcrt_dev_open("SPI1", 0);        // 打开SPI1

if(com1 < 0) {
    // 设备打开失败处理
}
```

##### `svcrt_dev_close` - 关闭设备

```c
int32 svcrt_dev_close(int32 handle);
```

| 参数 | 说明 |
|------|------|
| `handle` | `svcrt_dev_open` 返回的设备句柄 |

| 返回值 | 说明 |
|--------|------|
| 0 | 关闭成功 |
| -1 | 句柄无效或关闭失败 |

**示例：**
```c
int32 com1 = svcrt_dev_open("COM1", 115200);
// ... 使用设备 ...
svcrt_dev_close(com1);  // 关闭设备，释放句柄
```

##### `svcrt_dev_read` - 读取数据

```c
int32 svcrt_dev_read(int32 handle, void *pdata, int32 len);
```

| 参数 | 说明 |
|------|------|
| `handle` | `svcrt_dev_open` 返回的设备句柄 |
| `pdata` | 数据接收缓冲区指针 |
| `len` | 期望读取的字节数 |

| 返回值 | 说明 |
|--------|------|
| > 0 | 实际读取的字节数 |
| 0 | 无数据可读 |
| -1 | 读取失败 |

**示例：**
```c
uint8 buf[64];
int32 len = svcrt_dev_read(com1, buf, 64);
if(len > 0) {
    // 处理接收到的数据
}
```

##### `svcrt_dev_write` - 写入数据

```c
int32 svcrt_dev_write(int32 handle, void *pdata, int32 len);
```

| 参数 | 说明 |
|------|------|
| `handle` | 设备句柄 |
| `pdata` | 待发送数据缓冲区指针 |
| `len` | 待发送字节数 |

| 返回值 | 说明 |
|--------|------|
| ≥ 0 | 实际写入的字节数 |
| -1 | 写入失败 |

**示例：**
```c
uint8 msg[] = "Hello";
svcrt_dev_write(com1, msg, 5);
```

##### `svcrt_dev_ctrl` - 设备控制

```c
int32 svcrt_dev_ctrl(int32 handle, uint32 code, uint32 value);
```

| 参数 | 说明 |
|------|------|
| `handle` | 设备句柄 |
| `code` | 控制码（见标准控制码表） |
| `value` | 控制值，含义由控制码决定 |

**标准控制码：**

| 控制码 | 值 | 说明 |
|--------|-----|------|
| `SVCRT_DEV_CTRL_OPEN` | 0x0001 | 打开设备 |
| `SVCRT_DEV_CTRL_CLOSE` | 0x0002 | 关闭设备 |
| `SVCRT_DEV_CTRL_SET_BAUD` | 0x0010 | 设置波特率 |
| `SVCRT_DEV_CTRL_SET_MODE` | 0x0011 | 设置工作模式 |
| `SVCRT_DEV_CTRL_GET_STATUS` | 0x0020 | 获取设备状态 |
| `SVCRT_DEV_CTRL_RESET` | 0x0030 | 复位设备 |

**示例：**
```c
// 设置波特率为9600
svcrt_dev_ctrl(com1, SVCRT_DEV_CTRL_SET_BAUD, 9600);

// 获取设备状态
int32 status = svcrt_dev_ctrl(com1, SVCRT_DEV_CTRL_GET_STATUS, 0);

// 复位设备
svcrt_dev_ctrl(com1, SVCRT_DEV_CTRL_RESET, 0);
```

#### 5.2 任务管理API

##### `svcrt_task_wait` - 等待指定毫秒

```c
void svcrt_task_wait(uint32 ms);
```

将当前任务挂起指定的毫秒数。任务状态变为WAIT，等待时间到期后自动恢复为READY。

**示例：**
```c
while(1) {
    do_work();
    svcrt_task_wait(100);  // 每100ms执行一次
}
```

##### `svcrt_task_wait_period` - 等待下一周期

```c
void svcrt_task_wait_period(void);
```

将当前任务挂起，直到下一个调度周期到来。适用于周期性任务。

**示例：**
```c
while(1) {
    periodic_work();
    svcrt_task_wait_period();  // 等待下一个周期
}
```

##### `svcrt_task_delay` - 微秒级忙等

```c
void svcrt_task_delay(uint32 us);
```

微秒级精确延时，**不会让出CPU**，任务保持RUNNING状态。适用于短时间精确延时。

**示例：**
```c
// 等待传感器就绪（100微秒）
svcrt_task_delay(100);
```

> **注意：** `svcrt_task_delay` 是忙等，会占用CPU。长时间延时请使用 `svcrt_task_wait`。

##### `svcrt_task_kill` - 终止当前任务

```c
void svcrt_task_kill(void);
```

终止当前执行的任务，任务状态变为INVALID，不再被调度。

**示例：**
```c
if(fatal_error) {
    svcrt_task_kill();  // 发生致命错误，终止任务
}
```

#### 5.3 系统信息API

##### `svcrt_get_time_ms` - 获取系统时间

```c
uint32 svcrt_get_time_ms(void);
```

返回自系统启动以来的毫秒数。

**示例：**
```c
uint32 start = svcrt_get_time_ms();
do_work();
uint32 elapsed = svcrt_get_time_ms() - start;
```

##### `svcrt_get_cpu_usage` - 获取CPU负载

```c
uint32 svcrt_get_cpu_usage(void);
```

返回CPU负载率（0~1024范围内的值，1024表示100%负载）。

**示例：**
```c
uint32 load = svcrt_get_cpu_usage();
// load / 1024 * 100 = CPU使用率百分比
```

#### 5.4 事件API

##### `svcrt_event_create` - 创建事件

```c
int32 svcrt_event_create(char* name);
```

| 参数 | 说明 |
|------|------|
| `name` | 事件名称字符串（最长15字符） |

| 返回值 | 说明 |
|--------|------|
| ≥ 0 | 事件句柄 |
| -1 | 创建失败（事件表已满） |

##### `svcrt_event_wait` - 等待事件

```c
void svcrt_event_wait(int32 handle, int32 timeout);
```

| 参数 | 说明 |
|------|------|
| `handle` | `svcrt_event_create` 返回的事件句柄 |
| `timeout` | 超时时间(毫秒)，0表示等待到下一周期 |

##### `svcrt_event_set` - 触发事件

```c
void svcrt_event_set(int32 handle);
```

唤醒所有等待此事件的任务。

**完整事件示例：**

```c
// 生产者任务
void AppMain(void)
{
    int32 evt = svcrt_event_create("data_ready");

    while(1)
    {
        produce_data();
        svcrt_event_set(evt);         // 通知消费者数据就绪
        svcrt_task_wait(100);
    }
}

// 消费者任务（另一个分区）
void AppMain(void)
{
    int32 evt = svcrt_event_create("data_ready");

    while(1)
    {
        svcrt_event_wait(evt, 0);    // 等待数据就绪
        consume_data();
    }
}
```

### 6. App SDK 完整示例

#### 6.1 串口回显应用

```c
#include "svcrt.h"

void AppMain(void)
{
    int32 com1 = svcrt_dev_open("COM1", 115200);
    uint8 buf[128];

    while(1)
    {
        int32 len = svcrt_dev_read(com1, buf, 128);
        if(len > 0)
        {
            svcrt_dev_write(com1, buf, len);
        }
        svcrt_task_wait(10);
    }
}
```

#### 6.2 LED闪烁 + 事件同步

```c
#include "svcrt.h"

void AppMain(void)
{
    int32 led = svcrt_dev_open("LED", 0);
    int32 evt = svcrt_event_create("tick");
    uint8 state = 0;

    while(1)
    {
        state = !state;
        svcrt_dev_write(led, &state, 1);
        svcrt_event_set(evt);
        svcrt_task_wait(500);
    }
}
```

#### 6.3 多设备协作

```c
#include "svcrt.h"

void AppMain(void)
{
    int32 com1 = svcrt_dev_open("COM1", 115200);
    int32 led  = svcrt_dev_open("LED", 0);
    int32 spi  = svcrt_dev_open("SPI1", 0);
    int32 evt  = svcrt_event_create("cmd_recv");
    uint8 cmd_buf[32];
    uint8 spi_buf[64];

    while(1)
    {
        int32 len = svcrt_dev_read(com1, cmd_buf, 32);
        if(len > 0)
        {
            // 解析命令
            if(cmd_buf[0] == 0x01) {
                // 读取SPI数据
                svcrt_dev_read(spi, spi_buf, 64);
                svcrt_dev_write(com1, spi_buf, 64);
            }
            else if(cmd_buf[0] == 0x02) {
                // 切换LED
                uint8 val = cmd_buf[1];
                svcrt_dev_write(led, &val, 1);
            }

            svcrt_event_set(evt);
        }
        svcrt_task_wait(20);
    }
}
```

### 7. App SDK 编程规范

#### 7.1 必须遵守的规则

1. **不要直接操作硬件寄存器** — 所有硬件访问必须通过 `svcrt_dev_open/read/write/ctrl`
2. **不要使用阻塞式循环等待** — 使用 `svcrt_task_wait` 或 `svcrt_event_wait` 让出CPU
3. **AppMain() 不能返回** — 必须包含无限循环
4. **栈空间有限** — 避免大数组局部变量，使用静态或全局缓冲区
5. **不要调用内核内部函数** — 只使用 `svcrt.h` 中声明的API

#### 7.2 推荐的编程模式

```c
void AppMain(void)
{
    // 阶段1：初始化
    int32 dev1 = svcrt_dev_open("DEV1", 0);
    int32 evt  = svcrt_event_create("my_evt");

    // 阶段2：主循环
    while(1)
    {
        // 读取输入
        int32 len = svcrt_dev_read(dev1, buf, sizeof(buf));

        // 处理数据
        if(len > 0) {
            process_data(buf, len);
        }

        // 输出结果
        svcrt_dev_write(dev1, result, result_len);

        // 等待下一周期
        svcrt_task_wait(period_ms);
    }
}
```

---

## 第二部分：Driver SDK

### 8. Driver SDK 简介

Driver SDK 是面向SVCrtOS设备驱动开发者的工具包。驱动通过 `svcrt_drv_register()` 动态注册到内核设备表，应用程序通过 `svcrt_dev_open()` 等API使用。

Driver SDK 支持**两种编译模式**，驱动开发者可根据需要选择：

| 模式 | 宏定义 | 运行态 | 编译产物 | 硬件访问 | 适用场景 |
|------|--------|--------|----------|----------|----------|
| **内核态** | （默认，无需宏） | 内核态 | 静态库 `.lib` | 直接操作寄存器 | 需要直接操作硬件的高性能驱动 |
| **用户态** | `SVCRT_DRV_USER_MODE` | 用户态 | 独立固件 `.bin` | 通过内核代理 | 不需直接操作硬件的协议驱动 |

**核心特性：**
- **动态注册** — 驱动可以在运行时注册/注销，无需修改内核源码
- **统一接口** — 所有驱动实现相同的 `svcrt_dev_drv_t` 接口
- **独立编译** — 驱动SDK不依赖任何MCU头文件或内核内部头文件，可独立编译
- **双模式架构** — 内核态模式通过 `extern` 直接调用内核函数；用户态模式通过 SVC 0x14 陷入内核完成注册

### 9. Driver SDK 文件清单

```
sdk/driver_sdk/
├── svcrt_driver_sdk.h       ← 驱动开发API头文件（唯一需要包含的文件）
├── svcrt_types.h            ← 基础类型定义（被svcrt_driver_sdk.h自动包含）
├── svcrt_driver_bridge.c    ← 驱动注册桥接层（支持双模式条件编译）
├── svcrt_drv_main.c         ← 用户态驱动入口（含DrvMain弱定义）
├── svcrt_drv_start.s        ← 用户态驱动启动汇编入口
├── svcrt_drv_oslib.c        ← 用户态驱动OS接口封装（SVC调用）
└── examples/
    ├── example_led_drv.c    ← LED驱动示例
    └── example_uart_drv.c   ← UART驱动示例
```

**各文件用途与适用模式：**

| 文件 | 内核态模式 | 用户态模式 | 说明 |
|------|-----------|-----------|------|
| `svcrt_driver_sdk.h` | ? 必需 | ? 必需 | 驱动开发唯一需要包含的头文件 |
| `svcrt_types.h` | ? 必需 | ? 必需 | 基础类型定义，SDK自包含 |
| `svcrt_driver_bridge.c` | ? 必需 | ? 必需 | 注册桥接层，条件编译切换调用方式 |
| `svcrt_drv_main.c` | — 不需要 | ? 必需 | 提供 `DrvMain()` 入口和 `main()` |
| `svcrt_drv_start.s` | — 不需要 | ? 必需 | 用户态启动汇编入口 |
| `svcrt_drv_oslib.c` | — 不需要 | ? 必需 | 任务等待、事件等OS接口 |

### 10. 快速开始：开发一个驱动

驱动开发的前4步在两种编译模式下完全相同，区别在于第5步的编译方式和入口调用。

#### 步骤1：创建驱动文件

新建C文件，包含Driver SDK头文件：

```c
#include "svcrt_driver_sdk.h"
```

#### 步骤2：定义设备对象结构

```c
typedef struct {
    svcrt_dev_hdr_t hdr;     // 必须第一个成员！
    uint32  my_reg;          // 设备寄存器地址
    uint8   state;           // 设备状态
} my_dev_obj_t;
```

> **重要：** 设备对象结构体的第一个成员必须是 `svcrt_dev_hdr_t`。

#### 步骤3：实现5个驱动接口函数

```c
static svcrt_dev_hdr_t* my_drv_open(uint32 devid, uint32 param)
{
    // 初始化硬件、配置参数
    // 返回设备对象指针
}

static int32 my_drv_close(svcrt_dev_hdr_t *obj)
{
    // 关闭设备、释放资源
}

static int32 my_drv_read(svcrt_dev_hdr_t *obj, uint8 *pdata, int32 len)
{
    // 从设备读取数据
    // 返回实际读取的字节数
}

static int32 my_drv_write(svcrt_dev_hdr_t *obj, uint8 *pdata, int32 len)
{
    // 向设备写入数据
    // 返回实际写入的字节数
}

static int32 my_drv_ctrl(svcrt_dev_hdr_t *obj, uint32 code, uint32 value)
{
    // 设备控制操作
    // 根据code执行不同操作
}
```

#### 步骤4：声明驱动接口实例

```c
static svcrt_dev_drv_t my_drv = {
    my_drv_open,
    my_drv_close,
    my_drv_read,
    my_drv_write,
    my_drv_ctrl
};
```

#### 步骤5：编译与安装（按模式选择）

##### 内核态模式

实现安装函数，在板级初始化时调用：

```c
int32 my_drv_install(void)
{
    return svcrt_drv_register("MYDEV", &my_drv, 0);
}
```

**编译配置：**
- 宏定义：无需额外宏（默认内核态）
- 编译文件：`my_drv.c` + `svcrt_driver_bridge.c`
- 头文件路径：`sdk/driver_sdk/`
- 编译产物：静态库 `.lib`，与内核链接合并
- 安装方式：在 `board/` 层的 `svcrt_dev_board_init()` 中调用 `my_drv_install()`

##### 用户态模式

实现 `DrvMain()` 函数：

```c
void DrvMain(void)
{
    svcrt_drv_register("MYDEV", &my_drv, 0);

    while(1)
    {
        svcrt_task_wait(1000);
    }
}
```

**编译配置：**
- 宏定义：`SVCRT_DRV_USER_MODE`
- 编译文件：`my_drv.c` + `svcrt_driver_bridge.c` + `svcrt_drv_oslib.c` + `svcrt_drv_main.c` + `svcrt_drv_start.s`
- 头文件路径：`sdk/driver_sdk/`
- 编译产物：独立固件 `.bin`，烧录到指定ROM分区
- 安装方式：固件启动后自动调用 `DrvMain()`，通过 SVC 0x14 注册驱动

> **注意：** 用户态驱动的 `DrvMain()` 不能返回，必须包含无限循环。可以使用 `svcrt_task_wait()` 让出CPU。

### 11. Driver SDK API 详解

#### 11.1 驱动接口 svcrt_dev_drv_t

```c
typedef struct {
    svcrt_drv_open_func    drv_open;    // 打开设备
    svcrt_drv_close_func   drv_close;   // 关闭设备
    svcrt_drv_read_func    drv_read;    // 读取数据
    svcrt_drv_write_func   drv_write;   // 写入数据
    svcrt_drv_ctrl_func    drv_ctrl;    // 设备控制
} svcrt_dev_drv_t;
```

各函数指针类型定义：

```c
typedef svcrt_dev_hdr_t* (*svcrt_drv_open_func)(uint32 devid, uint32 param);
typedef int32            (*svcrt_drv_close_func)(svcrt_dev_hdr_t *obj);
typedef int32            (*svcrt_drv_read_func)(svcrt_dev_hdr_t *obj, uint8 *pdata, int32 len);
typedef int32            (*svcrt_drv_write_func)(svcrt_dev_hdr_t *obj, uint8 *pdata, int32 len);
typedef int32            (*svcrt_drv_ctrl_func)(svcrt_dev_hdr_t *obj, uint32 code, uint32 value);
```

#### 11.2 drv_open - 打开设备

```c
svcrt_dev_hdr_t* drv_open(uint32 devid, uint32 param);
```

| 参数 | 说明 |
|------|------|
| `devid` | 驱动内部设备编号（由注册时指定） |
| `param` | 应用传入的打开参数 |

| 返回值 | 说明 |
|--------|------|
| 非0 | 设备对象指针（svcrt_dev_hdr_t*） |
| 0 | 打开失败 |

**实现要点：**
- 初始化硬件外设
- 配置设备参数（如波特率、工作模式等）
- 返回设备对象指针，内核将其与应用句柄关联

#### 11.3 drv_close - 关闭设备

```c
int32 drv_close(svcrt_dev_hdr_t *obj);
```

| 参数 | 说明 |
|------|------|
| `obj` | drv_open返回的设备对象 |

| 返回值 | 说明 |
|--------|------|
| `SVCRT_DRV_OK` (0) | 成功 |
| `SVCRT_DRV_ERROR` (-1) | 失败 |

#### 11.4 drv_read - 读取数据

```c
int32 drv_read(svcrt_dev_hdr_t *obj, uint8 *pdata, int32 len);
```

| 参数 | 说明 |
|------|------|
| `obj` | 设备对象 |
| `pdata` | 数据接收缓冲区 |
| `len` | 期望读取的字节数 |

| 返回值 | 说明 |
|--------|------|
| > 0 | 实际读取字节数 |
| 0 | 无数据 |
| 负数 | 错误码 |

#### 11.5 drv_write - 写入数据

```c
int32 drv_write(svcrt_dev_hdr_t *obj, uint8 *pdata, int32 len);
```

参数和返回值含义同 drv_read。

#### 11.6 drv_ctrl - 设备控制

```c
int32 drv_ctrl(svcrt_dev_hdr_t *obj, uint32 code, uint32 value);
```

| 参数 | 说明 |
|------|------|
| `obj` | 设备对象 |
| `code` | 控制码 |
| `value` | 控制值 |

**标准控制码：**

| 控制码 | 值 | 说明 |
|--------|-----|------|
| `SVCRT_DEV_CTRL_OPEN` | 0x0001 | 重新打开 |
| `SVCRT_DEV_CTRL_CLOSE` | 0x0002 | 关闭 |
| `SVCRT_DEV_CTRL_SET_BAUD` | 0x0010 | 设置波特率 |
| `SVCRT_DEV_CTRL_SET_MODE` | 0x0011 | 设置模式 |
| `SVCRT_DEV_CTRL_GET_STATUS` | 0x0020 | 获取状态 |
| `SVCRT_DEV_CTRL_RESET` | 0x0030 | 复位 |

驱动可以定义自定义控制码（建议从0x0100开始）。

#### 11.7 驱动注册/注销API

##### `svcrt_drv_register` - 注册驱动

```c
int32 svcrt_drv_register(const char *name, svcrt_dev_drv_t *drv, uint32 dev_num);
```

| 参数 | 说明 |
|------|------|
| `name` | 设备名称（最长7字符），应用通过此名称打开设备 |
| `drv` | 驱动接口指针 |
| `dev_num` | 驱动内部设备编号，传入drv_open的devid参数 |

| 返回值 | 说明 |
|--------|------|
| 0 | 注册成功 |
| -1 | 设备表已满 |
| -2 | 参数无效或名称重复 |

##### `svcrt_drv_unregister` - 注销驱动

```c
int32 svcrt_drv_unregister(const char *name);
```

| 返回值 | 说明 |
|--------|------|
| 0 | 注销成功 |
| -1 | 设备未找到 |

##### `svcrt_drv_get_count` - 获取已注册设备数

```c
int32 svcrt_drv_get_count(void);
```

#### 11.8 驱动返回值定义

| 返回值 | 数值 | 说明 |
|--------|------|------|
| `SVCRT_DRV_OK` | 0 | 操作成功 |
| `SVCRT_DRV_ERROR` | -1 | 一般错误 |
| `SVCRT_DRV_BUSY` | -2 | 设备忙 |
| `SVCRT_DRV_TIMEOUT` | -3 | 操作超时 |
| `SVCRT_DRV_INVALID_PARAM` | -4 | 参数无效 |

### 12. Driver SDK 编译模式详解

Driver SDK 通过条件编译宏 `SVCRT_DRV_USER_MODE` 切换两种编译模式。两种模式下，驱动开发者编写的驱动代码**完全相同**，只需改变编译配置即可切换模式。

#### 12.1 内核态模式（默认）

**工作原理：**

桥接层 `svcrt_driver_bridge.c` 通过 `extern` 声明直接调用内核函数：

```
svcrt_drv_register()  →  svcrt_dev_register()    （内核函数，链接时解析）
svcrt_drv_unregister() →  svcrt_dev_unregister()  （内核函数，链接时解析）
svcrt_drv_get_count()  →  svcrt_dev_get_count()   （内核函数，链接时解析）
```

**依赖链：**

```
svcrt_driver_sdk.h  →  svcrt_types.h  （SDK自包含，无MCU依赖）
svcrt_driver_bridge.c  →  svcrt_driver_sdk.h  +  extern 内核函数声明
```

**编译配置（MDK工程）：**

| 配置项 | 值 |
|--------|-----|
| 宏定义 | （无需额外宏） |
| C编译文件 | `你的驱动.c`、`svcrt_driver_bridge.c` |
| 头文件路径 | `sdk/driver_sdk/` |
| 编译目标 | 创建库（Create Library），输出 `.lib` |

**集成方式：**

1. 将驱动 `.lib` 添加到内核 MDK 工程的库列表中
2. 在 `board/` 层的 `svcrt_dev_board_init()` 中调用驱动的安装函数：

```c
extern int32 my_drv_install(void);

void svcrt_dev_board_init(void)
{
    svcrt_dev_register("COM1", &usart_drv, 0);
    svcrt_dev_register("LED",  &led_drv,  0);
    my_drv_install();                    // 安装自定义驱动
}
```

**优势：**
- 驱动可直接操作硬件寄存器，零开销
- 与内核在同一地址空间，性能最优

**限制：**
- 最终需与内核链接，不能独立运行
- 驱动bug可能导致整个系统崩溃

#### 12.2 用户态模式（SVCRT_DRV_USER_MODE）

**工作原理：**

桥接层 `svcrt_driver_bridge.c` 通过 SVC 指令陷入内核完成注册：

```
svcrt_drv_register()  →  __svc(0x14)  →  SVC_Server  →  svcrt_dev_register()
svcrt_drv_unregister() →  __svc(0x14)  →  SVC_Server  →  svcrt_dev_unregister()
svcrt_drv_get_count()  →  __svc(0x14)  →  SVC_Server  →  svcrt_dev_get_count()
```

SVC 0x14 的子功能编码：

| p[0] | 功能 | 参数 |
|------|------|------|
| 1 | 注册驱动 | p[1]=name, p[2]=drv指针, p[3]=dev_num |
| 2 | 注销驱动 | p[1]=name |
| 3 | 获取设备数 | 无 |

**依赖链：**

```
svcrt_driver_sdk.h  →  svcrt_types.h  （SDK自包含，无MCU依赖）
svcrt_driver_bridge.c  →  svcrt_driver_sdk.h  （SVC调用，无内核依赖）
svcrt_drv_oslib.c      →  svcrt_driver_sdk.h  （SVC调用，无内核依赖）
svcrt_drv_main.c       →  svcrt_driver_sdk.h  （无内核依赖）
svcrt_drv_start.s      →  无依赖
```

**编译配置（MDK工程）：**

| 配置项 | 值 |
|--------|-----|
| 宏定义 | `SVCRT_DRV_USER_MODE` |
| C编译文件 | `你的驱动.c`、`svcrt_driver_bridge.c`、`svcrt_drv_oslib.c`、`svcrt_drv_main.c` |
| 汇编文件 | `svcrt_drv_start.s` |
| 头文件路径 | `sdk/driver_sdk/` |
| 编译目标 | 创建可执行文件（Create Executable），输出 `.bin/.hex` |
| 链接地址 | 按分区ROM/RAM地址配置scatter文件 |

**用户态驱动可用的OS API：**

通过 `svcrt_drv_oslib.c` 提供，与 App SDK 的接口一致：

| API | 说明 |
|-----|------|
| `svcrt_task_wait(ms)` | 挂起指定毫秒 |
| `svcrt_task_wait_period()` | 等待下一调度周期 |
| `svcrt_task_delay(us)` | 微秒级忙等 |
| `svcrt_get_time_ms()` | 获取系统时间 |
| `svcrt_event_create(name)` | 创建事件 |
| `svcrt_event_wait(handle, timeout)` | 等待事件 |
| `svcrt_event_set(handle)` | 触发事件 |

**优势：**
- 完全独立编译，不依赖内核源码
- 驱动bug不会导致内核崩溃（MPU隔离）
- 可独立烧录/替换，无需重新编译内核

**限制：**
- 不能直接操作硬件寄存器
- SVC调用有少量开销
- 需要内核提供硬件操作代理

#### 12.3 模式选择指南

| 场景 | 推荐模式 | 原因 |
|------|---------|------|
| UART/SPI/I2C 等需直接操作寄存器的外设驱动 | 内核态 | 需要直接读写寄存器 |
| LED/GPIO 等简单外设驱动 | 内核态 | 操作简单，直接访问效率高 |
| 文件系统/网络协议栈等逻辑驱动 | 用户态 | 不需直接操作硬件，可独立升级 |
| 传感器数据处理/滤波算法 | 用户态 | 纯逻辑处理，MPU隔离更安全 |
| 第三方闭源驱动 | 用户态 | 不需暴露内核源码即可集成 |

### 13. Driver SDK 完整示例

#### 13.1 SPI驱动示例

```c
#include "svcrt_driver_sdk.h"

#define SPI1_BASE    0x40013000

typedef struct {
    svcrt_dev_hdr_t hdr;
    uint32  base;
    uint32  speed;
    uint8   mode;
} spi_dev_obj_t;

static spi_dev_obj_t spi_dev = {{0}, SPI1_BASE, 1000000, 0};

static svcrt_dev_hdr_t* spi_drv_open(uint32 devid, uint32 param)
{
    spi_dev.speed = param ? param : 1000000;
    spi_dev.mode = 0;
    spi_dev.hdr.block_size = 1;

    // 配置SPI硬件
    // SPI_Init(spi_dev.base, spi_dev.speed, spi_dev.mode);

    return (svcrt_dev_hdr_t*)&spi_dev;
}

static int32 spi_drv_close(svcrt_dev_hdr_t *obj)
{
    // SPI_DeInit(((spi_dev_obj_t*)obj)->base);
    return SVCRT_DRV_OK;
}

static int32 spi_drv_read(svcrt_dev_hdr_t *obj, uint8 *pdata, int32 len)
{
    spi_dev_obj_t *p = (spi_dev_obj_t*)obj;
    int32 i;
    for(i = 0; i < len; i++)
    {
        // pdata[i] = SPI_Transfer(p->base, 0xFF);
    }
    return len;
}

static int32 spi_drv_write(svcrt_dev_hdr_t *obj, uint8 *pdata, int32 len)
{
    spi_dev_obj_t *p = (spi_dev_obj_t*)obj;
    int32 i;
    for(i = 0; i < len; i++)
    {
        // SPI_Transfer(p->base, pdata[i]);
    }
    return len;
}

static int32 spi_drv_ctrl(svcrt_dev_hdr_t *obj, uint32 code, uint32 value)
{
    spi_dev_obj_t *p = (spi_dev_obj_t*)obj;
    switch(code)
    {
    case SVCRT_DEV_CTRL_SET_MODE:
        p->mode = value;
        return SVCRT_DRV_OK;
    case SVCRT_DEV_CTRL_SET_BAUD:
        p->speed = value;
        return SVCRT_DRV_OK;
    case SVCRT_DEV_CTRL_GET_STATUS:
        return p->speed;
    default:
        return SVCRT_DRV_ERROR;
    }
}

static svcrt_dev_drv_t spi_drv = {
    spi_drv_open,
    spi_drv_close,
    spi_drv_read,
    spi_drv_write,
    spi_drv_ctrl
};

int32 spi_drv_install(void)
{
    return svcrt_drv_register("SPI1", &spi_drv, 0);
}
```

#### 13.2 多实例驱动（同一驱动管理多个设备）

```c
#include "svcrt_driver_sdk.h"

typedef struct {
    svcrt_dev_hdr_t hdr;
    uint32  uart_base;
    uint32  baudrate;
} uart_dev_obj_t;

#define UART_MAX  3
static uart_dev_obj_t uart_devs[UART_MAX] = {
    {{0}, 0x40011000, 115200},  // UART1
    {{0}, 0x40004400, 115200},  // UART2
    {{0}, 0x40004800, 115200},  // UART3
};

static svcrt_dev_hdr_t* uart_drv_open(uint32 devid, uint32 param)
{
    uart_dev_obj_t *p;
    if(devid >= UART_MAX) return 0;

    p = &uart_devs[devid];
    p->baudrate = param ? param : 115200;
    p->hdr.block_size = 1;

    // UART_Init(p->uart_base, p->baudrate);
    return (svcrt_dev_hdr_t*)p;
}

static int32 uart_drv_close(svcrt_dev_hdr_t *obj) { return SVCRT_DRV_OK; }
static int32 uart_drv_read(svcrt_dev_hdr_t *obj, uint8 *pdata, int32 len) { return 0; }
static int32 uart_drv_write(svcrt_dev_hdr_t *obj, uint8 *pdata, int32 len) { return len; }

static int32 uart_drv_ctrl(svcrt_dev_hdr_t *obj, uint32 code, uint32 value)
{
    uart_dev_obj_t *p = (uart_dev_obj_t*)obj;
    if(code == SVCRT_DEV_CTRL_SET_BAUD) {
        p->baudrate = value;
        return SVCRT_DRV_OK;
    }
    return SVCRT_DRV_ERROR;
}

static svcrt_dev_drv_t uart_drv = {
    uart_drv_open, uart_drv_close,
    uart_drv_read, uart_drv_write, uart_drv_ctrl
};

void uart_drv_install_all(void)
{
    svcrt_drv_register("COM1", &uart_drv, 0);  // devid=0 → uart_devs[0]
    svcrt_drv_register("COM2", &uart_drv, 1);  // devid=1 → uart_devs[1]
    svcrt_drv_register("COM3", &uart_drv, 2);  // devid=2 → uart_devs[2]
}
```

### 13. Driver SDK 编程规范

#### 13.1 必须遵守的规则

1. **设备对象第一个成员必须是 `svcrt_dev_hdr_t`** — 内核通过此头部管理设备
2. **drv_open 必须返回有效的设备对象指针** — 返回0表示打开失败
3. **drv_read/drv_write 返回实际操作字节数** — 返回负数表示错误
4. **中断处理中不要调用阻塞API** — 中断上下文不能挂起任务
5. **使用 `svcrt_fifo_t` 进行中断安全的数据缓冲** — 避免在中断中直接操作设备对象

#### 13.2 推荐的编程模式

```c
// 驱动私有数据通过容器宏获取
#define DRV_GET_OBJ(obj, type)  ((type*)(obj))

// 或使用更安全的偏移检查
static my_dev_obj_t* get_dev_obj(svcrt_dev_hdr_t *hdr)
{
    return (my_dev_obj_t*)hdr;
}
```

#### 13.3 中断与驱动的协作

驱动通常需要在中断中接收数据，在任务上下文中处理。推荐使用 FIFO 缓冲区：

```c
static svcrt_fifo_t rx_fifo;
static uint8 rx_buf[256];

static svcrt_dev_hdr_t* my_drv_open(uint32 devid, uint32 param)
{
    svcrt_fifo_init(&rx_fifo, rx_buf, 256);
    // ...
}

// 中断服务函数
void USART1_IRQHandler(void)
{
    if(USART_GetITStatus(USART1, USART_IT_RXNE))
    {
        uint8 data = USART_ReceiveData(USART1);
        svcrt_fifo_write(&rx_fifo, &data, 1);
    }
}

// 驱动读取函数（任务上下文调用）
static int32 my_drv_read(svcrt_dev_hdr_t *obj, uint8 *pdata, int32 len)
{
    return svcrt_fifo_read(&rx_fifo, pdata, len);
}
```

---

## 附录：API 速查表

### App SDK API 速查

| 分类 | API | 说明 |
|------|-----|------|
| 设备 | `svcrt_dev_open(name, param)` | 打开设备 |
| 设备 | `svcrt_dev_close(handle)` | 关闭设备 |
| 设备 | `svcrt_dev_read(handle, buf, len)` | 读取数据 |
| 设备 | `svcrt_dev_write(handle, buf, len)` | 写入数据 |
| 设备 | `svcrt_dev_ctrl(handle, code, value)` | 设备控制 |
| 任务 | `svcrt_task_wait(ms)` | 毫秒等待 |
| 任务 | `svcrt_task_wait_period()` | 等待下一周期 |
| 任务 | `svcrt_task_delay(us)` | 微秒忙等 |
| 任务 | `svcrt_task_kill()` | 终止任务 |
| 事件 | `svcrt_event_create(name)` | 创建事件 |
| 事件 | `svcrt_event_wait(handle, timeout)` | 等待事件 |
| 事件 | `svcrt_event_set(handle)` | 触发事件 |
| 系统 | `svcrt_get_time_ms()` | 系统时间 |
| 系统 | `svcrt_get_cpu_usage()` | CPU负载 |

### Driver SDK API 速查

| API | 说明 |
|-----|------|
| `svcrt_drv_register(name, drv, dev_num)` | 注册驱动 |
| `svcrt_drv_unregister(name)` | 注销驱动 |
| `svcrt_drv_get_count()` | 获取设备数 |
| `svcrt_fifo_init(fifo, buf, size)` | 初始化FIFO |
| `svcrt_fifo_read(fifo, buf, len)` | 从FIFO读取 |
| `svcrt_fifo_write(fifo, buf, len)` | 写入FIFO |

### 错误码速查

| 错误码 | 值 | 说明 |
|--------|-----|------|
| `SVCRT_DRV_OK` | 0 | 成功 |
| `SVCRT_DRV_ERROR` | -1 | 一般错误 |
| `SVCRT_DRV_BUSY` | -2 | 设备忙 |
| `SVCRT_DRV_TIMEOUT` | -3 | 超时 |
| `SVCRT_DRV_INVALID_PARAM` | -4 | 参数无效 |
| `svcrt_dev_open` 返回 -1 | -1 | 设备未找到 |
| `svcrt_event_create` 返回 -1 | -1 | 事件表满 |
