# SVCrtOS SDK 用户手册

## 1. 概述

SVCrtOS 提供两套独立的SDK，分别面向**应用开发者**和**驱动开发者**：

| SDK | 目标用户 | 核心文件 | 依赖 |
|-----|---------|---------|------|
| **App SDK** | 应用程序开发者 | `svcrt.h` | 无MCU依赖 |
| **Driver SDK** | 设备驱动开发者 | `svcrt_driver_sdk.h` | 无MCU依赖 |

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
    int32 led = dev_open("LED", 0);

    while(1)
    {
        uint8 val = 1;
        dev_write(led, &val, 1);
        TaskWait(500);

        val = 0;
        dev_write(led, &val, 1);
        TaskWait(500);
    }
}
```

#### 步骤4：配置分区参数

在 `svcrt_app_config.h` 中定义分区资源需求：

```c
SVCRT_APP_CONFIG app_configures[] = {
    {
        .ram_start       = 0x20000000,   // RAM起始地址
        .ram_size        = 0x1000,       // RAM大小(4KB)
        .stack_size      = 0x400,        // 栈大小(1KB)
        .rom_start       = 0x08020000,   // ROM起始地址
        .rom_size        = 0x20000,      // ROM大小(128KB)
        .period_ms       = 1000,         // 任务周期(1秒)
        .priority        = 10,           // 优先级
        .share_mem_access = 0            // 共享内存访问属性
    }
};
int32 app_num = 1;
```

#### 步骤5：编译与烧录

将应用编译为独立固件，链接到分区配置中指定的ROM地址，烧录即可。

### 5. App SDK API 详解

#### 5.1 设备操作API

##### `dev_open` - 打开设备

```c
int32 dev_open(char* name, uint32 param);
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
int32 com1 = dev_open("COM1", 115200);  // 以115200波特率打开串口1
int32 led  = dev_open("LED", 0);         // 打开LED设备
int32 spi  = dev_open("SPI1", 0);        // 打开SPI1

if(com1 < 0) {
    // 设备打开失败处理
}
```

##### `dev_read` - 读取数据

```c
int32 dev_read(int32 handle, void *pdata, int32 len);
```

| 参数 | 说明 |
|------|------|
| `handle` | `dev_open` 返回的设备句柄 |
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
int32 len = dev_read(com1, buf, 64);
if(len > 0) {
    // 处理接收到的数据
}
```

##### `dev_write` - 写入数据

```c
int32 dev_write(int32 handle, void *pdata, int32 len);
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
dev_write(com1, msg, 5);
```

##### `dev_ctrl` - 设备控制

```c
int32 dev_ctrl(int32 handle, uint32 code, uint32 value);
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
dev_ctrl(com1, SVCRT_DEV_CTRL_SET_BAUD, 9600);

// 获取设备状态
int32 status = dev_ctrl(com1, SVCRT_DEV_CTRL_GET_STATUS, 0);

// 复位设备
dev_ctrl(com1, SVCRT_DEV_CTRL_RESET, 0);
```

#### 5.2 任务管理API

##### `TaskWait` - 等待指定毫秒

```c
void TaskWait(uint32 ms);
```

将当前任务挂起指定的毫秒数。任务状态变为WAIT，等待时间到期后自动恢复为READY。

**示例：**
```c
while(1) {
    do_work();
    TaskWait(100);  // 每100ms执行一次
}
```

##### `TaskWaitNxtPeriod` - 等待下一周期

```c
void TaskWaitNxtPeriod(void);
```

将当前任务挂起，直到下一个调度周期到来。适用于周期性任务。

**示例：**
```c
while(1) {
    periodic_work();
    TaskWaitNxtPeriod();  // 等待下一个周期
}
```

##### `TaskDelay` - 微秒级忙等

```c
void TaskDelay(uint32 us);
```

微秒级精确延时，**不会让出CPU**，任务保持RUNNING状态。适用于短时间精确延时。

**示例：**
```c
// 等待传感器就绪（100微秒）
TaskDelay(100);
```

> ?? **注意：** `TaskDelay` 是忙等，会占用CPU。长时间延时请使用 `TaskWait`。

##### `TaskKill` - 终止当前任务

```c
void TaskKill(void);
```

终止当前执行的任务，任务状态变为INVALID，不再被调度。

**示例：**
```c
if(fatal_error) {
    TaskKill();  // 发生致命错误，终止任务
}
```

#### 5.3 系统信息API

##### `GetSystemTimeMs` - 获取系统时间

```c
uint32 GetSystemTimeMs(void);
```

返回自系统启动以来的毫秒数。

**示例：**
```c
uint32 start = GetSystemTimeMs();
do_work();
uint32 elapsed = GetSystemTimeMs() - start;
```

##### `GetCpuPayload` - 获取CPU负载

```c
uint32 GetCpuPayload(void);
```

返回CPU负载率（0~1024范围内的值，1024表示100%负载）。

**示例：**
```c
uint32 load = GetCpuPayload();
// load / 1024 * 100 = CPU使用率百分比
```

#### 5.4 事件API

##### `CreateEvent` - 创建事件

```c
int32 CreateEvent(char* name);
```

| 参数 | 说明 |
|------|------|
| `name` | 事件名称字符串（最长15字符） |

| 返回值 | 说明 |
|--------|------|
| ≥ 0 | 事件句柄 |
| -1 | 创建失败（事件表已满） |

##### `WaitEvent` - 等待事件

```c
void WaitEvent(int32 handle, int32 timeout);
```

| 参数 | 说明 |
|------|------|
| `handle` | `CreateEvent` 返回的事件句柄 |
| `timeout` | 超时时间(毫秒)，0表示等待到下一周期 |

##### `SetEvent` - 触发事件

```c
void SetEvent(int32 handle);
```

唤醒所有等待此事件的任务。

**完整事件示例：**

```c
// 生产者任务
void AppMain(void)
{
    int32 evt = CreateEvent("data_ready");

    while(1)
    {
        produce_data();
        SetEvent(evt);         // 通知消费者数据就绪
        TaskWait(100);
    }
}

// 消费者任务（另一个分区）
void AppMain(void)
{
    int32 evt = CreateEvent("data_ready");

    while(1)
    {
        WaitEvent(evt, 0);    // 等待数据就绪
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
    int32 com1 = dev_open("COM1", 115200);
    uint8 buf[128];

    while(1)
    {
        int32 len = dev_read(com1, buf, 128);
        if(len > 0)
        {
            dev_write(com1, buf, len);
        }
        TaskWait(10);
    }
}
```

#### 6.2 LED闪烁 + 事件同步

```c
#include "svcrt.h"

void AppMain(void)
{
    int32 led = dev_open("LED", 0);
    int32 evt = CreateEvent("tick");
    uint8 state = 0;

    while(1)
    {
        state = !state;
        dev_write(led, &state, 1);
        SetEvent(evt);
        TaskWait(500);
    }
}
```

#### 6.3 多设备协作

```c
#include "svcrt.h"

void AppMain(void)
{
    int32 com1 = dev_open("COM1", 115200);
    int32 led  = dev_open("LED", 0);
    int32 spi  = dev_open("SPI1", 0);
    int32 evt  = CreateEvent("cmd_recv");
    uint8 cmd_buf[32];
    uint8 spi_buf[64];

    while(1)
    {
        int32 len = dev_read(com1, cmd_buf, 32);
        if(len > 0)
        {
            // 解析命令
            if(cmd_buf[0] == 0x01) {
                // 读取SPI数据
                dev_read(spi, spi_buf, 64);
                dev_write(com1, spi_buf, 64);
            }
            else if(cmd_buf[0] == 0x02) {
                // 切换LED
                uint8 val = cmd_buf[1];
                dev_write(led, &val, 1);
            }

            SetEvent(evt);
        }
        TaskWait(20);
    }
}
```

### 7. App SDK 编程规范

#### 7.1 必须遵守的规则

1. **不要直接操作硬件寄存器** — 所有硬件访问必须通过 `dev_open/read/write/ctrl`
2. **不要使用阻塞式循环等待** — 使用 `TaskWait` 或 `WaitEvent` 让出CPU
3. **AppMain() 不能返回** — 必须包含无限循环
4. **栈空间有限** — 避免大数组局部变量，使用静态或全局缓冲区
5. **不要调用内核内部函数** — 只使用 `svcrt.h` 中声明的API

#### 7.2 推荐的编程模式

```c
void AppMain(void)
{
    // 阶段1：初始化
    int32 dev1 = dev_open("DEV1", 0);
    int32 evt  = CreateEvent("my_evt");

    // 阶段2：主循环
    while(1)
    {
        // 读取输入
        int32 len = dev_read(dev1, buf, sizeof(buf));

        // 处理数据
        if(len > 0) {
            process_data(buf, len);
        }

        // 输出结果
        dev_write(dev1, result, result_len);

        // 等待下一周期
        TaskWait(period_ms);
    }
}
```

---

## 第二部分：Driver SDK

### 8. Driver SDK 简介

Driver SDK 是面向SVCrtOS设备驱动开发者的工具包。驱动运行在内核态，可以直接操作硬件寄存器。驱动通过 `svcrtDrvRegister()` 动态注册到内核设备表，应用程序通过 `dev_open()` 等API使用。

**核心特性：**
- **动态注册** — 驱动可以在运行时注册/注销，无需修改内核源码
- **统一接口** — 所有驱动实现相同的 `SVCRT_DRV_INTERFACE` 接口
- **独立开发** — 驱动代码与内核代码完全分离，可独立编译

### 9. Driver SDK 文件清单

```
sdk/driver_sdk/
├── svcrt_driver_sdk.h       ← 驱动开发API头文件（唯一需要包含的文件）
├── svcrt_driver_bridge.c    ← 驱动注册桥接层实现
└── examples/
    ├── example_led_drv.c    ← LED驱动示例
    └── example_uart_drv.c   ← UART驱动示例
```

### 10. 快速开始：5步开发一个驱动

#### 步骤1：创建驱动文件

新建C文件，包含Driver SDK头文件：

```c
#include "svcrt_driver_sdk.h"
```

#### 步骤2：定义设备对象结构

```c
typedef struct {
    DEV_HDR hdr;        // 必须第一个成员！
    uint32  my_reg;     // 设备寄存器地址
    uint8   state;      // 设备状态
} MY_DEV_OBJ;
```

> ?? **重要：** 设备对象结构体的第一个成员必须是 `DEV_HDR`。

#### 步骤3：实现5个驱动接口函数

```c
static DEV_HDR* my_drv_open(uint32 devid, uint32 param)
{
    // 初始化硬件、配置参数
    // 返回设备对象指针
}

static int32 my_drv_close(DEV_HDR *obj)
{
    // 关闭设备、释放资源
}

static int32 my_drv_read(DEV_HDR *obj, uint8 *pdata, int32 len)
{
    // 从设备读取数据
    // 返回实际读取的字节数
}

static int32 my_drv_write(DEV_HDR *obj, uint8 *pdata, int32 len)
{
    // 向设备写入数据
    // 返回实际写入的字节数
}

static int32 my_drv_ctrl(DEV_HDR *obj, uint32 code, uint32 value)
{
    // 设备控制操作
    // 根据code执行不同操作
}
```

#### 步骤4：声明驱动接口实例

```c
static SVCRT_DRV_INTERFACE my_drv = {
    my_drv_open,
    my_drv_close,
    my_drv_read,
    my_drv_write,
    my_drv_ctrl
};
```

#### 步骤5：实现安装函数并注册

```c
int32 my_drv_install(void)
{
    return svcrtDrvRegister("MYDEV", &my_drv, 0);
}
```

在系统初始化时调用 `my_drv_install()` 即可将驱动注册到内核。

### 11. Driver SDK API 详解

#### 11.1 驱动接口 SVCRT_DRV_INTERFACE

```c
typedef struct {
    DrvOpenFunc    DrvOpen;    // 打开设备
    DrvCloseFunc   DrvClose;   // 关闭设备
    DrvReadFunc    DrvRead;    // 读取数据
    DrvWriteFunc   DrvWrite;   // 写入数据
    DrvIOCtrlFunc  DrvCtrl;    // 设备控制
} SVCRT_DRV_INTERFACE;
```

各函数指针类型定义：

```c
typedef DEV_HDR* (*DrvOpenFunc)(uint32 devid, uint32 param);
typedef int32    (*DrvCloseFunc)(DEV_HDR *obj);
typedef int32    (*DrvReadFunc)(DEV_HDR *obj, uint8 *pdata, int32 len);
typedef int32    (*DrvWriteFunc)(DEV_HDR *obj, uint8 *pdata, int32 len);
typedef int32    (*DrvIOCtrlFunc)(DEV_HDR *obj, uint32 code, uint32 value);
```

#### 11.2 DrvOpen - 打开设备

```c
DEV_HDR* DrvOpen(uint32 devid, uint32 param);
```

| 参数 | 说明 |
|------|------|
| `devid` | 驱动内部设备编号（由注册时指定） |
| `param` | 应用传入的打开参数 |

| 返回值 | 说明 |
|--------|------|
| 非0 | 设备对象指针（DEV_HDR*） |
| 0 | 打开失败 |

**实现要点：**
- 初始化硬件外设
- 配置设备参数（如波特率、工作模式等）
- 返回设备对象指针，内核将其与应用句柄关联

#### 11.3 DrvClose - 关闭设备

```c
int32 DrvClose(DEV_HDR *obj);
```

| 参数 | 说明 |
|------|------|
| `obj` | DrvOpen返回的设备对象 |

| 返回值 | 说明 |
|--------|------|
| `SVCRT_DRV_OK` (0) | 成功 |
| `SVCRT_DRV_ERROR` (-1) | 失败 |

#### 11.4 DrvRead - 读取数据

```c
int32 DrvRead(DEV_HDR *obj, uint8 *pdata, int32 len);
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

#### 11.5 DrvWrite - 写入数据

```c
int32 DrvWrite(DEV_HDR *obj, uint8 *pdata, int32 len);
```

参数和返回值含义同 DrvRead。

#### 11.6 DrvCtrl - 设备控制

```c
int32 DrvIOCtrl(DEV_HDR *obj, uint32 code, uint32 value);
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

##### `svcrtDrvRegister` - 注册驱动

```c
int32 svcrtDrvRegister(const char *name, SVCRT_DRV_INTERFACE *drv, uint32 dev_num);
```

| 参数 | 说明 |
|------|------|
| `name` | 设备名称（最长7字符），应用通过此名称打开设备 |
| `drv` | 驱动接口指针 |
| `dev_num` | 驱动内部设备编号，传入DrvOpen的devid参数 |

| 返回值 | 说明 |
|--------|------|
| 0 | 注册成功 |
| -1 | 设备表已满 |
| -2 | 参数无效或名称重复 |

##### `svcrtDrvUnregister` - 注销驱动

```c
int32 svcrtDrvUnregister(const char *name);
```

| 返回值 | 说明 |
|--------|------|
| 0 | 注销成功 |
| -1 | 设备未找到 |

##### `svcrtDrvGetCount` - 获取已注册设备数

```c
int32 svcrtDrvGetCount(void);
```

#### 11.8 驱动返回值定义

| 返回值 | 数值 | 说明 |
|--------|------|------|
| `SVCRT_DRV_OK` | 0 | 操作成功 |
| `SVCRT_DRV_ERROR` | -1 | 一般错误 |
| `SVCRT_DRV_BUSY` | -2 | 设备忙 |
| `SVCRT_DRV_TIMEOUT` | -3 | 操作超时 |
| `SVCRT_DRV_INVALID_PARAM` | -4 | 参数无效 |

### 12. Driver SDK 完整示例

#### 12.1 SPI驱动示例

```c
#include "svcrt_driver_sdk.h"

#define SPI1_BASE    0x40013000

typedef struct {
    DEV_HDR hdr;
    uint32  base;
    uint32  speed;
    uint8   mode;
} SPI_DEV_OBJ;

static SPI_DEV_OBJ spi_dev = {{0}, SPI1_BASE, 1000000, 0};

static DEV_HDR* spi_drv_open(uint32 devid, uint32 param)
{
    spi_dev.speed = param ? param : 1000000;
    spi_dev.mode = 0;
    spi_dev.hdr.blocksize = 1;

    // 配置SPI硬件
    // SPI_Init(spi_dev.base, spi_dev.speed, spi_dev.mode);

    return (DEV_HDR*)&spi_dev;
}

static int32 spi_drv_close(DEV_HDR *obj)
{
    // SPI_DeInit(((SPI_DEV_OBJ*)obj)->base);
    return SVCRT_DRV_OK;
}

static int32 spi_drv_read(DEV_HDR *obj, uint8 *pdata, int32 len)
{
    SPI_DEV_OBJ *p = (SPI_DEV_OBJ*)obj;
    int32 i;
    for(i = 0; i < len; i++)
    {
        // pdata[i] = SPI_Transfer(p->base, 0xFF);
    }
    return len;
}

static int32 spi_drv_write(DEV_HDR *obj, uint8 *pdata, int32 len)
{
    SPI_DEV_OBJ *p = (SPI_DEV_OBJ*)obj;
    int32 i;
    for(i = 0; i < len; i++)
    {
        // SPI_Transfer(p->base, pdata[i]);
    }
    return len;
}

static int32 spi_drv_ctrl(DEV_HDR *obj, uint32 code, uint32 value)
{
    SPI_DEV_OBJ *p = (SPI_DEV_OBJ*)obj;
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

static SVCRT_DRV_INTERFACE spi_drv = {
    spi_drv_open,
    spi_drv_close,
    spi_drv_read,
    spi_drv_write,
    spi_drv_ctrl
};

int32 spi_drv_install(void)
{
    return svcrtDrvRegister("SPI1", &spi_drv, 0);
}
```

#### 12.2 多实例驱动（同一驱动管理多个设备）

```c
#include "svcrt_driver_sdk.h"

typedef struct {
    DEV_HDR hdr;
    uint32  uart_base;
    uint32  baudrate;
} UART_DEV_OBJ;

#define UART_MAX  3
static UART_DEV_OBJ uart_devs[UART_MAX] = {
    {{0}, 0x40011000, 115200},  // UART1
    {{0}, 0x40004400, 115200},  // UART2
    {{0}, 0x40004800, 115200},  // UART3
};

static DEV_HDR* uart_drv_open(uint32 devid, uint32 param)
{
    UART_DEV_OBJ *p;
    if(devid >= UART_MAX) return 0;

    p = &uart_devs[devid];
    p->baudrate = param ? param : 115200;
    p->hdr.blocksize = 1;

    // UART_Init(p->uart_base, p->baudrate);
    return (DEV_HDR*)p;
}

static int32 uart_drv_close(DEV_HDR *obj) { return SVCRT_DRV_OK; }
static int32 uart_drv_read(DEV_HDR *obj, uint8 *pdata, int32 len) { return 0; }
static int32 uart_drv_write(DEV_HDR *obj, uint8 *pdata, int32 len) { return len; }

static int32 uart_drv_ctrl(DEV_HDR *obj, uint32 code, uint32 value)
{
    UART_DEV_OBJ *p = (UART_DEV_OBJ*)obj;
    if(code == SVCRT_DEV_CTRL_SET_BAUD) {
        p->baudrate = value;
        return SVCRT_DRV_OK;
    }
    return SVCRT_DRV_ERROR;
}

static SVCRT_DRV_INTERFACE uart_drv = {
    uart_drv_open, uart_drv_close,
    uart_drv_read, uart_drv_write, uart_drv_ctrl
};

void uart_drv_install_all(void)
{
    svcrtDrvRegister("COM1", &uart_drv, 0);  // devid=0 → uart_devs[0]
    svcrtDrvRegister("COM2", &uart_drv, 1);  // devid=1 → uart_devs[1]
    svcrtDrvRegister("COM3", &uart_drv, 2);  // devid=2 → uart_devs[2]
}
```

### 13. Driver SDK 编程规范

#### 13.1 必须遵守的规则

1. **DEV_HDR 必须是设备对象结构体的第一个成员**
2. **设备名称最长7字符**（含结尾\0为8字节）
3. **DrvOpen 必须返回有效的 DEV_HDR* 指针**，返回0表示打开失败
4. **中断处理函数中不要调用阻塞API**
5. **驱动注册应在系统初始化阶段完成**，不要在任务运行中注册

#### 13.2 推荐的编程模式

```c
// 1. 定义设备对象（静态分配）
static MY_DEV_OBJ my_dev = {0};

// 2. 实现驱动接口
static DEV_HDR* my_open(uint32 devid, uint32 param) { ... }
static int32 my_close(DEV_HDR *obj) { ... }
static int32 my_read(DEV_HDR *obj, uint8 *d, int32 l) { ... }
static int32 my_write(DEV_HDR *obj, uint8 *d, int32 l) { ... }
static int32 my_ctrl(DEV_HDR *obj, uint32 c, uint32 v) { ... }

// 3. 声明接口实例
static SVCRT_DRV_INTERFACE my_drv = {
    my_open, my_close, my_read, my_write, my_ctrl
};

// 4. 安装函数
int32 my_drv_install(void)
{
    return svcrtDrvRegister("MYDEV", &my_drv, 0);
}
```

---

## 附录A：SVC系统调用编号

| SVC号 | 分类 | 功能 |
|--------|------|------|
| 0x10 | 设备IO | p[0]=1:dev_open, 2:dev_read, 3:dev_write, 4:dev_ctrl |
| 0x11 | 时间管理 | r0=1:TaskWait, 2:TaskWaitNxtPeriod, 3:TaskDelay, 4:TaskKill |
| 0x12 | 系统信息 | r0=1:GetSystemTimeMs, 2:GetCpuPayload |
| 0x13 | 事件管理 | p[0]=1:CreateEvent, 2:WaitEvent, 3:SetEvent |

## 附录B：配置参数速查

| 配置项 | 默认值 | 说明 |
|--------|--------|------|
| `SVCRT_TASK_MAX_NUM` | 7 | 最大任务数 |
| `SVCRT_TICK_PERIOD_US` | 500 | 滴答周期(微秒) |
| `SVCRT_EVENT_NUM` | 10 | 最大事件数 |
| `SVCRT_MAX_EVENT_WAITERS` | 4 | 每事件最大等待者 |
| `SVCRT_DEV_MAX_NUM` | 8 | 最大设备驱动数 |
| `SVCRT_USE_FPU` | 自动 | FPU开关 |
| `SVCRT_USE_MPU` | 自动 | MPU开关 |
| `SVCRT_USE_PRIV` | 自动 | 特权分离开关 |
| `SVCRT_USE_CPU_LOAD` | 1 | CPU负载统计 |
| `SVCRT_USE_STACK_CHECK` | 1 | 栈溢出检测 |

## 附录C：错误码速查

| 错误码 | 值 | 说明 |
|--------|-----|------|
| `SVCRT_DRV_OK` | 0 | 成功 |
| `SVCRT_DRV_ERROR` | -1 | 一般错误 |
| `SVCRT_DRV_BUSY` | -2 | 设备忙 |
| `SVCRT_DRV_TIMEOUT` | -3 | 超时 |
| `SVCRT_DRV_INVALID_PARAM` | -4 | 参数无效 |
| dev_open返回-1 | -1 | 设备未找到 |
| CreateEvent返回-1 | -1 | 事件表满 |
