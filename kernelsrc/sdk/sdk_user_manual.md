# SVCrtOS SDK 用户手册
> **【时效性提示】** 本文写作于「分区表 + 加载器」架构改造之前。凡涉及
> **分区地址**、**内核入口宏（`BLED_DRV_ENTRY` 之类）**、**`app_config.c` /
> `svcrt_app_config.h`**、**手工维护的 `.sct`** 的段落，均已被下列内容取代：
>
> | 想知道 | 看哪里 |
> |---|---|
> | 今天怎么装 App / 驱动、怎么调试、崩溃了怎么办 | [../../docs/SVCrtOS应用安装与调试指南.md](../../docs/SVCrtOS应用安装与调试指南.md) |
> | 分区 / 加载器 / 镜像格式为什么这样设计 | [../../docs/Loader工程化落地说明.md](../../docs/Loader工程化落地说明.md) |
> | 全工程唯一地址源头 | [../../config/svcrt_partition.h](../../config/svcrt_partition.h) |
> | 文档总索引 | [../../docs/README.md](../../docs/README.md) |
>
> 口诀：**地址只在 `config/svcrt_partition.h` 写一次；`.sct` 由脚本生成；
> 入口由内核从分区表推导，任何地方都不要再抄第二遍地址。**

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

#### 步骤4：配置分区参数（已改）

> **旧写法已废弃**：早期要在 `svcrt_app_config.h` 里定义 `svcrt_app_cfg_table[]`，
> 把 `ram_start` / `rom_start` 等地址写死在 App 工程里。该头文件与结构体**已删除**。

当前做法：

1. 分区基址、容量、任务优先级、栈大小，**只在 `config/svcrt_partition.h` 配一次**；
2. App 工程的分散加载文件由 `tools/gen_scatter.py` 自动生成到 `build/app.sct`，
   App 工程**不包含** `config/svcrt_partition.h`；
3. 运行期需要知道自己的分区信息时，用 `svcrt_app_status(slot)` 这类接口查询，
   不要把地址抄进 App。

所以这一步实际是**空操作**——App 侧不需要配置任何地址。

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
        svcrt_event_wait(evt, -1);   // -1 = 永久等待数据就绪
        consume_data();
    }
}
```

### 6. App SDK 完整示例

#### 6.1 串口回显应用

一个最简单的串口回显应用，演示设备 IO 与任务等待的配合：

```c
#include "svcrt.h"

void AppMain(void)
{
    int32 com1 = svcrt_dev_open("COM1", 115200);
    uint8 buf[64];

    while(1)
    {
        int32 len = svcrt_dev_read(com1, buf, sizeof(buf));
        if(len > 0) {
            svcrt_dev_write(com1, buf, len);   // 收到什么就回什么
        }
        svcrt_task_wait(10);                   // 让出 CPU
    }
}
```

#### 6.2 LED闪烁 + 事件同步

演示通过事件在两个应用之间同步动作：

```c
/* 控制任务：每按一次按键触发一次事件 */
void AppMain(void)
{
    int32 key = svcrt_dev_open("KEY", 0);
    int32 evt = svcrt_event_create("blink");

    while(1) {
        uint8 v;
        svcrt_dev_read(key, &v, 1);
        if(v) svcrt_event_set(evt);
        svcrt_task_wait(50);
    }
}

/* LED 任务：收到事件后闪烁 3 次 */
void AppMain(void)
{
    int32 led = svcrt_dev_open("LED", 0);
    int32 evt = svcrt_event_create("blink");
    int32 i;
    uint8 v;

    while(1) {
        svcrt_event_wait(evt, -1);   // -1 = 永久等待
        for(i = 0; i < 3; i++) {
            v = 1; svcrt_dev_write(led, &v, 1); svcrt_task_wait(100);
            v = 0; svcrt_dev_write(led, &v, 1); svcrt_task_wait(100);
        }
    }
}
```

#### 6.3 多设备协作

演示一个应用同时驱动多个设备：

```c
void AppMain(void)
{
    int32 com1 = svcrt_dev_open("COM1", 115200);
    int32 spi  = svcrt_dev_open("SPI1", 0);
    int32 led  = svcrt_dev_open("LED", 0);
    uint8 cmd_buf[32];
    uint8 spi_buf[64];
    uint8 on = 1, off = 0;

    while(1)
    {
        int32 len = svcrt_dev_read(com1, cmd_buf, 32);
        if(len > 0)
        {
            // 简易命令解析
            if(cmd_buf[0] == 0x01) {
                // 命令1：从 SPI 读 64 字节回送串口
                svcrt_dev_read(spi, spi_buf, 64);
                svcrt_dev_write(com1, spi_buf, 64);
            }
            else if(cmd_buf[0] == 0x02) {
                // 命令2：翻转 LED
                svcrt_dev_write(led, &on, 1);
                svcrt_task_wait(200);
                svcrt_dev_write(led, &off, 1);
            }
        }
        svcrt_task_wait(20);
    }
}
```

### 7. App SDK 编程规范

1. **`AppMain()` 不能返回** — 必须包含无限循环，否则任务会被销毁
2. **长延时让出 CPU** — 使用 `svcrt_task_wait` / `svcrt_event_wait` 让出 CPU
3. **栈空间有限** — 默认 1~2KB，禁止在栈上分配大数组
4. **设备名称要匹配** — `svcrt_dev_open` 的名称必须与驱动注册的名称完全一致
5. **错误处理** — 所有 API 的返回值都应检查，特别是 `dev_open` 是否返回 -1

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
| `svcrt_driver_sdk.h` | ✓ 必需 | ✓ 必需 | 驱动开发唯一需要包含的头文件 |
| `svcrt_types.h` | ✓ 必需 | ✓ 必需 | 基础类型定义，SDK自包含 |
| `svcrt_driver_bridge.c` | ✓ 必需 | ✓ 必需 | 注册桥接层，条件编译切换调用方式 |
| `svcrt_drv_main.c` | — 不需要 | ✓ 必需 | 提供 `DrvMain()` 入口和 `main()` |
| `svcrt_drv_start.s` | — 不需要 | ✓ 必需 | 用户态启动汇编入口 |
| `svcrt_drv_oslib.c` | — 不需要 | ✓ 必需 | 任务等待、事件等OS接口 |

### 10. 快速开始：开发一个驱动

驱动开发的前4步在两种编译模式下完全相同，区别在于第5步的编译方式和入口调用。

#### 步骤1：创建驱动文件

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
static svcrt_dev_hdr_t* my_drv_open(uint32 devid, uint32 param)  { /* ... */ }
static int32 my_drv_close(svcrt_dev_hdr_t *obj)                  { /* ... */ }
static int32 my_drv_read (svcrt_dev_hdr_t *obj, uint8 *p, int32 len) { /* ... */ }
static int32 my_drv_write(svcrt_dev_hdr_t *obj, uint8 *p, int32 len) { /* ... */ }
static int32 my_drv_ctrl (svcrt_dev_hdr_t *obj, uint32 code, uint32 val) { /* ... */ }
```

#### 步骤4：声明驱动接口实例

```c
static svcrt_dev_drv_t my_drv = {
    my_drv_open, my_drv_close, my_drv_read, my_drv_write, my_drv_ctrl
};
```

#### 步骤5：编译与安装（按模式选择）

**内核态模式：**

实现安装函数，在板级初始化时调用：

```c
int32 my_drv_install(void) {
    return svcrt_drv_register("MYDEV", &my_drv, 0);
}
```

- 宏定义：无需额外宏（默认内核态）
- 编译文件：`my_drv.c` + `svcrt_driver_bridge.c`
- 编译产物：静态库 `.lib`，与内核链接合并
- 安装方式：在 `board/` 层 `svcrt_dev_board_init()` 中调用 `my_drv_install()`

**用户态模式：**

实现 `DrvMain()` 函数：

```c
void DrvMain(void) {
    svcrt_drv_register("MYDEV", &my_drv, 0);
    while(1) { svcrt_task_wait(1000); }
}
```

- 宏定义：`SVCRT_DRV_USER_MODE`
- 编译文件：`my_drv.c` + `svcrt_driver_bridge.c` + `svcrt_drv_oslib.c` + `svcrt_drv_main.c` + `svcrt_drv_start.s`
- 编译产物：独立固件 `.bin`，烧录到指定ROM分区
- 安装方式：固件启动后自动调用 `DrvMain()`，通过 SVC 0x14 注册驱动

> **注意：** 用户态驱动的 `DrvMain()` 不能返回，必须包含无限循环，
> 而且**循环里必须让出 CPU**（`svcrt_task_wait()`，见上面的示例），
> 不要写空转的 `while(1){}`。
>
> 原因：驱动任务优先级（`DRIVER_TASK_PRIORITY`（`config/svcrt_partition.h`），默认 9）**高于** App 任务
> （`APP_TASK_PRIORITY`，默认 10），空转会吃满 CPU，把所有 App 永久饿死。
> 只注册设备、不需要后台维护的驱动，注册完让出 CPU 即可。
> SDK 自带的 `svcrt_drv_main.c` 在 `DrvMain()` 返回后也是用 `svcrt_task_wait(1000)` 兜底。

### 11. Driver SDK API 详解

#### 11.1 驱动接口 svcrt_dev_drv_t

```c
typedef struct {
    svcrt_drv_open_func    drv_open;   // 打开设备
    svcrt_drv_close_func   drv_close;  // 关闭设备
    svcrt_drv_read_func    drv_read;   // 读数据
    svcrt_drv_write_func   drv_write;  // 写数据
    svcrt_drv_ioctl_func   drv_ctrl;   // 设备控制
} svcrt_dev_drv_t;
```

驱动开发者只需实现这 5 个函数，并填入此结构体即可完成一个驱动。

#### 11.2 drv_open 原型

```c
typedef svcrt_dev_hdr_t *(*svcrt_drv_open_func)(uint32 dev_id, uint32 param);
```

| 参数 | 说明 |
|------|------|
| `devid` | 驱动内部设备编号（由注册时指定） |
| `param` | 应用传入的打开参数（如波特率） |

| 返回值 | 说明 |
|--------|------|
| 非 NULL | 设备对象指针，将作为后续操作的 obj 参数 |
| NULL | 打开失败 |

#### 11.3 drv_close 原型

```c
typedef int32 (*svcrt_drv_close_func)(svcrt_dev_hdr_t *obj);
```

| 参数 | 说明 |
|------|------|
| `obj` | drv_open 返回的设备对象 |

| 返回值 | 说明 |
|--------|------|
| `SVCRT_DRV_OK` | 关闭成功 |
| 负数 | 关闭失败 |

#### 11.4 drv_read / drv_write 原型

```c
typedef int32 (*svcrt_drv_read_func)(svcrt_dev_hdr_t *obj, uint8 *pdata, int32 len);
typedef int32 (*svcrt_drv_write_func)(svcrt_dev_hdr_t *obj, uint8 *pdata, int32 len);
```

| 参数 | 说明 |
|------|------|
| `obj` | drv_open 返回的设备对象 |
| `pdata` | 数据缓冲区指针 |
| `len` | 数据长度 |

| 返回值 | 说明 |
|--------|------|
| ≥ 0 | 实际读/写的字节数 |
| 负数 | 读/写失败（见驱动返回值定义） |

#### 11.5 drv_ctrl 原型

```c
typedef int32 (*svcrt_drv_ioctl_func)(svcrt_dev_hdr_t *obj, uint32 code, uint32 value);
```

| 参数 | 说明 |
|------|------|
| `obj` | drv_open 返回的设备对象 |
| `code` | 控制码（标准控制码或驱动自定义） |
| `value` | 控制参数 |

#### 11.6 设备对象结构 svcrt_dev_hdr_t

```c
typedef struct {
    uint32 block_size;      // 块大小（用于块设备，字符设备可设为 0 或 1）
} svcrt_dev_hdr_t;
```

驱动开发者**必须**将自己的设备对象结构体的**第一个成员**定义为 `svcrt_dev_hdr_t`，这样才能通过指针强转访问。

#### 11.7 驱动注册/注销API

##### `svcrt_drv_register` - 注册驱动

```c
int32 svcrt_drv_register(const char *name, svcrt_dev_drv_t *drv, uint32 dev_num);
```

| 参数 | 说明 |
|------|------|
| `name` | 设备名称字符串（应用通过此名称打开设备） |
| `drv` | 驱动接口结构体指针 |
| `dev_num` | 设备编号（用于同一驱动管理多个设备实例） |

| 返回值 | 说明 |
|--------|------|
| `SVCRT_DRV_OK` | 注册成功 |
| 负数 | 注册失败（设备表已满或名称重复） |

##### `svcrt_drv_unregister` - 注销驱动

```c
int32 svcrt_drv_unregister(const char *name);
```

#### 11.8 驱动返回值定义

| 宏 | 值 | 说明 |
|----|-----|------|
| `SVCRT_DRV_OK` | 0 | 操作成功 |
| `SVCRT_DRV_ERROR` | -1 | 通用错误 |
| `SVCRT_DRV_BUSY` | -2 | 设备忙 |
| `SVCRT_DRV_TIMEOUT` | -3 | 超时 |
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
svcrt_driver_sdk.h     →  svcrt_types.h  （SDK自包含，无MCU依赖）
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
2. 在 `board/` 层的 `svcrt_dev_board_init()` 中调用驱动的安装函数

**优势：**
- 驱动可直接操作硬件寄存器，零开销
- 与内核在同一地址空间，性能最优

**限制：**
- 最终需与内核链接，不能独立运行
- 驱动 bug 可能导致整个系统崩溃

#### 12.2 用户态模式（SVCRT_DRV_USER_MODE）

**工作原理：**

桥接层 `svcrt_driver_bridge.c` 通过 SVC 指令陷入内核完成注册：

```
svcrt_drv_register()  →  __svc(0x14)  →  SVC_Server  →  svcrt_dev_register()
```

SVC 0x14 的子功能编码：

| p[0] | 功能 | 参数 |
|------|------|------|
| 1 | 注册驱动 | p[1]=name, p[2]=drv指针, p[3]=dev_num |
| 2 | 注销驱动 | p[1]=name |
| 3 | 获取设备数 | 无 |

**依赖链：**

```
svcrt_driver_sdk.h     →  svcrt_types.h  （SDK自包含，无MCU依赖）
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
- 驱动 bug 不会导致内核崩溃（MPU隔离）
- 可独立烧录/替换，无需重新编译内核

**限制：**
- 不能直接操作硬件寄存器
- SVC 调用有少量开销
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

#### 13.1 SPI驱动示例（内核态）

```c
#include "svcrt_driver_sdk.h"

typedef struct {
    svcrt_dev_hdr_t hdr;
    uint32 base_addr;       // SPI 控制器基址
    uint32 baud;
} spi_obj_t;

static spi_obj_t spi1_obj = { {0}, 0x40013000, 1000000 };

static svcrt_dev_hdr_t *spi_open(uint32 devid, uint32 param)
{
    (void)devid; (void)param;
    /* 配置时钟、引脚、寄存器 ... */
    return (svcrt_dev_hdr_t *)&spi1_obj;
}

static int32 spi_close(svcrt_dev_hdr_t *obj) { return SVCRT_DRV_OK; }

static int32 spi_read(svcrt_dev_hdr_t *obj, uint8 *p, int32 len)
{
    spi_obj_t *o = (spi_obj_t *)obj;
    int32 i;
    for(i = 0; i < len; i++) {
        /* 等待 RXNE 并读取 SPI 数据寄存器 */
        p[i] = 0;
    }
    return len;
}

static int32 spi_write(svcrt_dev_hdr_t *obj, uint8 *p, int32 len)
{
    spi_obj_t *o = (spi_obj_t *)obj;
    int32 i;
    for(i = 0; i < len; i++) {
        /* 等待 TXE 并写入 SPI 数据寄存器 */
    }
    return len;
}

static int32 spi_ctrl(svcrt_dev_hdr_t *obj, uint32 code, uint32 value)
{
    spi_obj_t *o = (spi_obj_t *)obj;
    if(code == SVCRT_DEV_CTRL_SET_BAUD) { o->baud = value; return SVCRT_DRV_OK; }
    return SVCRT_DRV_ERROR;
}

static svcrt_dev_drv_t spi_drv = { spi_open, spi_close, spi_read, spi_write, spi_ctrl };

int32 spi_drv_install(void) { return svcrt_drv_register("SPI1", &spi_drv, 0); }
```

#### 13.2 多实例驱动（同一驱动管理多个设备）

通过 `dev_id` 区分不同的设备实例：

```c
#include "svcrt_driver_sdk.h"

#define UART_NUM   3

typedef struct {
    svcrt_dev_hdr_t hdr;
    uint32 base_addr;
    uint32 baud;
    uint8  in_use;
} uart_obj_t;

static uart_obj_t uart_pool[UART_NUM] = {
    { {0}, 0x40011000, 115200, 0 },   /* UART1 */
    { {0}, 0x40004400, 115200, 0 },   /* UART2 */
    { {0}, 0x40004800, 115200, 0 },   /* UART3 */
};

static svcrt_dev_hdr_t *uart_open(uint32 devid, uint32 param)
{
    if(devid >= UART_NUM) return 0;
    if(uart_pool[devid].in_use) return 0;
    uart_pool[devid].baud   = param;
    uart_pool[devid].in_use = 1;
    /* 配置寄存器 ... */
    return (svcrt_dev_hdr_t *)&uart_pool[devid];
}

/* ... close/read/write/ctrl 类似 ... */

static svcrt_dev_drv_t uart_drv = { uart_open, /* ... */ };

int32 uart_drv_install(void)
{
    svcrt_drv_register("COM1", &uart_drv, 0);
    svcrt_drv_register("COM2", &uart_drv, 1);
    svcrt_drv_register("COM3", &uart_drv, 2);
    return SVCRT_DRV_OK;
}
```

---

## 第三部分：外部分区固件运行机制（重要）

App SDK 与 Driver SDK 的用户态固件以**独立 .bin** 形式烧录到专属 ROM 分区，由内核加载运行。这一节说明让外部固件正确运行必须满足的三个条件，初次集成时极易踩坑。

### A. 内核自动扫描外部分区（不需要写入口宏）

早期版本要求在内核 `svcrt_register_tasks()` 里把外部分区入口写成宏注册为任务：

```c
#define BLED_DRV_ENTRY  (0x08060000u | 1u)   /* 旧写法，已删除 */
```

**这种写法已经删除**——它等于把 `config/svcrt_partition.h` 里的地址再抄一遍，
布局一改就会悄悄错位。

当前流程：

1. `svcrt_ptable_init()` 把分区表放进共享 RAM（布局信息全部来自配置头）；
2. `svcrt_loader_scan()` / `svcrt_loader_scan_driver()` 上电扫描各分区做镜像识别：
   带头 `.svcapp`（magic `SVCA` + `hw_compat_id` 匹配 + CRC 正确）或开发期裸镜像；
3. 识别通过后按 `APP_TASK_PRIORITY` / `APP_TASK_STACK_SIZE`（驱动用 `DRIVER_TASK_*`）
   自动建任务，**栈从该分区自己的 RAM 区顶部切出**（不再由内核侧数组提供）；
4. 运行期可用 `svcrt_app_load/start/stop/status`、`svcrt_driver_load` 动态管理，
   或让内核安装任务从串口收 `.svcapp` 并按镜像头 `type` 自动分流。

任务被调度时 PC 跳到分区入口，执行启动汇编 → `__main` → `main` → `AppMain`/`DrvMain`。

### B. 启动汇编必须经 `__main`（C 运行时初始化）

**这是最关键、最易踩坑的一点。** 外部固件的全局/静态变量分两类：

| 段 | 内容 | 是否需要初始化 |
|----|------|--------------|
| `.data` | 带初值的全局变量（如**驱动接口表 `svcrt_dev_drv_t`** 的函数指针） | 必须从 Flash 拷贝到 RAM |
| `.bss`  | 零初值全局变量 | 必须清零 |

链接器把 `.data` 的初值放在 Flash 的 Load 区，运行时需拷贝到 RAM 的 Exec 区。**若启动汇编直接 `LDR R0,=main; BLX R0`，会跳过 C 运行时的 scatter loading**，导致：
- 驱动接口表 `drv` 全是**随机函数指针** → 注册后 App 调用即 **HardFault**
- `.bss` 未清零 → 全局状态错乱

**正确做法**：启动汇编跳转到 C 库入口 `__main`，它会自动完成 `.data` 拷贝 + `.bss` 清零（scatter loading），再调用 `main`：

```asm
    AREA    |RESET|, CODE, READONLY
DRVSTART    PROC
    EXPORT DRVSTART
    IMPORT  __main
            NOP
            NOP
            LDR R0, = __main      ; ← 必须经 __main，而非直接跳 main
            BX  R0
            B   .
            ENDP
            ALIGN
            END
```

SDK 提供的 `svcrt_app_start.s` / `svcrt_drv_start.s` 已采用此方式，开发者无需修改。

### C. 分区地址必须互不重叠

内核、驱动池、各 App 槽位的 ROM 与 RAM 区必须严格错开。**这一约束由
`config/svcrt_partition.h` 保证**，改完用脚本自检即可：

```bash
python tools/gen_scatter.py --check     # 校验分区无重叠、无越界
python tools/gen_scatter.py --dump      # 打印当前布局
```

不需要（也不允许）在 scatter file、内核入口宏、App 配置表里各写一遍地址——
**地址只写一次**，其余全部推导。旧文档里「地址需在三处保持一致」的说法已作废：
那三处现在有两处已经不存在了。

### 排错速查

| 现象 | 可能原因 |
|------|---------|
| 烧录后外部固件完全不运行（只有内核任务） | 分区未通过镜像识别（头/CRC/`hw_compat_id` 不对，或裸镜像但 `APP_ALLOW_RAW_IMAGE=0`）；用 `svcrt_loader_state()` 与故障记录定位 |
| 外部固件一运行就 HardFault / 复位 | 启动汇编未经 `__main`，.data/.bss 未初始化（条件 B） |
| App `svcrt_dev_open` 返回 -1 | 对应 Driver 固件未烧录或未注册设备 |
| 多固件随机崩溃 | ROM/RAM 分区地址重叠（条件 C） |
