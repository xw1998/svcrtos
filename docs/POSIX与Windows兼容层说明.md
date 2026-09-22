# POSIX 与 Windows 兼容层说明

SVCrtOS 的 App 侧兼容层。目标只有一条：**把一份为 Linux / Windows 写的 C 程序搬到 SVCrtOS 上跑，改的是 include 列表，不是程序结构。**

兼容层的实现全部落在 App 自己的映射里，内核侧只提供它本来就有的能力（设备、信号量、消息队列、互斥量、时钟、新增的线程服务）。因此兼容层里没有一个地址、没有一个分区号、没有一个槽位号——同一份源码在任何板子上都能编译。

---

## 1. 这不是什么

先说清楚边界，避免预期错位：

| 不是 | 说明 |
|------|------|
| 不是内核的一部分 | 全部在 `kernelsrc/sdk/posix/`，作为 App 工程的一个源文件加进去。内核不为它改一行接口（除了新增的线程服务 SVC 0x1B）。 |
| 不是 libc | 不提供 `printf` 家族、`string.h`、`stdlib.h`。App 用 SDK 的 `svcrt_*` 或自己带。 |
| 不是模拟器 | 每个调用都真的落到一个内核服务上，没有"假装成功"的桩。做不到的调用一律返回错误并置 `errno`，而不是返回一个看起来合理的结果。 |
| 不是进程模型 | 一个 App 就是一个地址空间 + 若干线程，没有 `fork` / `exec` / `waitpid`，也没有文件系统。 |

---

## 2. 文件清单

```
kernelsrc/sdk/posix/
├── svcrt_posix.h          伞头：一次引入全部兼容层 + 堆接口 + 档位开关
├── svcrt_posix.c          实现（约 1240 行，App 工程加入这一个 .c 即可）
├── svcrt_posix_types.h    基础类型（ssize_t / off_t / time_t / struct timespec）
├── errno.h                错误码 + 每线程 errno
├── fcntl.h                open 标志
├── unistd.h               read / write / close / sleep / usleep / nanosleep / time
├── time.h                 clock_gettime / sched_yield
├── pthread.h              pthread 线程与互斥量
├── semaphore.h            无名信号量
├── mqueue.h               消息队列
└── svcrt_win_compat.h     Windows 风格名字（CreateThread / Sleep / strcpy_s ...）
```

三种引入方式，按需选：

```c
/* A. 全都要：一份普通 POSIX 程序，改一行 include */
#include "svcrt_posix.h"

/* B. 只要一样：单头也独立可用 */
#include "semaphore.h"      /* 例：只用信号量 */

/* C. Windows 风格的名字也要 */
#include "svcrt_win_compat.h"   /* 它内部已经 include 了 svcrt_posix.h */
```

工程里把 `svcrt_posix.c` 加入编译即可，没有别的构建步骤。它占用的是 App 自己的 RAM，不是内核的。

---

## 3. 可调档位

全部在包含头文件之前定义，或写在工程的预定义宏里。

| 宏 | 默认 | 含义 |
|----|------|------|
| `SVCRT_POSIX_THREAD_MAX` | `2` | 同时存在的 pthread 上限。每多一个线程多一份静态栈 + 两个信号量。 |
| `SVCRT_POSIX_STACK_WORDS` | `256` | 每个 pthread 的栈字数（= 1 KB）。 |
| `SVCRT_POSIX_HEAP_SIZE` | `1024` | App 侧堆 arena 字节数。 |
| `SVCRT_POSIX_DEFAULT_PRIO` | `10` | 线程默认内核优先级。 |
| `SVCRT_POSIX_NO_TIMESPEC` | 未定义 | 平台头已定义 `struct timespec` 时定义它，避免重复定义。 |
| `SVCRT_POSIX_WRAP_STDLIB` | 未定义 | 默认关闭。定义后把 `malloc/free/calloc/realloc` 映射到本兼容层的 arena。见 §8。 |

### 默认档位与 RAM 窗口的账

开发槽位给 App 的 RAM 窗口是 **8 KB**（`SVCRT_DEV_RAM_WINDOW`，F427 / F401 同值，见 `docs/配置区与安装策略.md`）。默认档位是按这个窗口配的：

| 项 | 字节 |
|----|------|
| 2 个线程栈（2 × 1 KB） | 2048 |
| 堆 arena | 1024 |
| errno 表（64 格 × int32） | 256 |
| 线程槽位表 + 2 个线程的完成信号量等静态数据 | 数十 |
| **兼容层小计** | **约 3.4 KB** |
| 余下给 App 自身数据 + 主线程栈 | 约 4.6 KB |

arena 与线程栈都是**静态**的：不用也占着。App 槽位 RAM 更大时（例如手动把槽位 RAM 窗口调大），按项目把档位提上去；反之若 App 不用线程，把 `SVCRT_POSIX_THREAD_MAX` 设 0 之外的较小值（至少 1，若要 `pthread_create` 则 ≥ 所需线程数）。

---

## 4. 能力与映射一览

### 4.1 设备与时间（`unistd.h` / `time.h` / `fcntl.h`）

| POSIX | 落到 | 说明 |
|-------|------|------|
| `open(name, flags)` | `svcrt_dev_open` | `fd` 就是设备句柄。只有 `O_RDONLY/O_WRONLY/O_RDWR/O_APPEND`（其中 `O_NONBLOCK` 仅记录，内核设备读写无阻塞语义）。 |
| `read(fd,buf,n)` / `write(fd,buf,n)` | `svcrt_dev_read` / `svcrt_dev_write` | 失败置 `EIO`，参数非法置 `EBADF`。 |
| `close(fd)` | `svcrt_dev_close` | |
| `lseek(...)` | **不提供** | `fd` 是**设备句柄**，不是文件句柄：设备侧没有 offset 状态，`unistd.h` 里也没有这个声明。要按偏移读文件，走 `svcrt_fs_read_at()` / `svcrt_fs_read_file()`（内核文件服务），不要绕 POSIX 这一层 |
| `sleep(s)` | `svcrt_task_wait(s*1000)` | 调度的整秒等待。 |
| `usleep(us)` | 调度等待 | 内核按毫秒等待，**向上取整**：usleep 不会提前返回。 |
| `nanosleep(req,rem)` | 调度等待 | 绝对时间等待，`rem` 仅在超时未睡够时填。 |
| `time(t)` / `clock_gettime()` | 启动以来的 ms 节拍 | **没有 RTC，没有日历**。`CLOCK_REALTIME` 被别名到单调时钟，而不是报一个错的真实时间。 |
| `sched_yield()` | 0ms 等待 | 内核没有"让出 CPU 但仍就绪"的独立原语，这是一次普通等待。 |

不提供：`fork` / `exec*` / `getpid`（除桩）/ `localtime` / `strftime` / `lseek` / `open` 的 `O_CREAT`。
理由：没有进程模型；`open` 这一层只面向**设备**（`fd` 即设备句柄）。
内核**有**文件系统（littlefs on NOR，见《SVCrtOS应用安装与调试指南.md》），
但它走 `svcrt_fs_*` 与 SVC 0x1C，**不挂在 POSIX `open` 下面**：
文件有路径与偏移，设备没有，两者共用一套 fd 语义只会造出一个看着能用的错东西。
**编译期就报错**远好过运行期打开一个意想不到的东西。

### 4.2 errno（`errno.h`）

`errno` 是**每线程**的，存在 App 自己的 RAM 里：一张 64 格的 `int32` 数组，格子的下标取自内核任务号（SVC 0x1B 的 `self`）。

- 读取 `errno` 的代价是 **一次 SVC**。这是刻意的取舍：小 RTOS 上正确性优先。热循环里请把它缓存在局部变量。
- 表大小 64 与内核任务表上限同源。
- 内核不认识某条 POSIX 错误码时，兼容层统一用最接近的一条（如超时 → `ETIMEDOUT=110`）。

### 4.3 pthread（`pthread.h`）

| POSIX | 落到 |
|-------|------|
| `pthread_create` | 线程服务 SVC 0x1B（见 §5） |
| `pthread_join` | 每线程一个完成信号量 |
| `pthread_exit` | `svcrt_thread_exit` |
| `pthread_self` | `svcrt_thread_self` |
| `pthread_mutex_*` | 内核互斥量（`svcrt_mutex_*`） |

`pthread_attr_t` 是 SVCrtOS 自己的结构（`stackaddr` / `stacksize` / `priority` / `period_ms`），**全 0 即为默认**：

```c
pthread_t th;
pthread_create(&th, 0, worker, arg);   /* 零属性，栈从 SDK 静态池取 */
```

**明确不支持（故意的）**：

- **没有条件变量**。它需要一个内核等待队列加上 signal/broadcast 原语，目前不存在。宁可不提供，也不做一个语义不完整的版本。
- **没有 `pthread_attr_set*` 系列**。属性直接填结构体字段；POSIX 那套 setter 在 32 位小系统上只是额外的间接层。
- 互斥量**没有优先级继承的对外接口**。内核互斥量确实做了优先级继承，但 POSIX 程序既观察不到也设置不了。
- `thread == 0` 表示"无此线程"，与 `pthread_t`（`uint32`）同宽。

线程栈的所有权：要么由调用者提供（`stackaddr`），要么取自兼容层的静态池。**内核不会替你分配栈**，也不会接受一个落在内核或别的 App RAM 里的栈指针。

### 4.4 信号量（`semaphore.h`）

`sem_t` 只放一个内核句柄，因此 `sem_init` / `sem_destroy` 就是全部生命周期调用，不需要静态存储。

| POSIX | 说明 |
|-------|------|
| `sem_init(s, pshared, v)` | `pshared` 被忽略（同一地址空间，没有"进程间"概念） |
| `sem_wait` / `sem_trywait` / `sem_post` | 直接映射 |
| `sem_timedwait(s, ms)` | **超时单位是毫秒**，不是 `struct timespec`（内核契约就是 ms） |
| `sem_getvalue` | **不支持**，返回 -1 并置 `ENOSYS` |

`sem_getvalue` 不支持的原因：内核信号量服务没有"当前计数"查询，在本侧猜一个数会和其它任务竞争。**报错好过给一个可能是错的数字。**

### 4.5 消息队列（`mqueue.h`）

| POSIX | 说明 |
|-------|------|
| `mq_open(name, oflag)` | 只创建，不查找。内核没有名字空间，`oflag` 被忽略 |
| `mq_send(q, msg, len_words)` | 长度单位是**字**，不是字节 |
| `mq_receive(q, msg, len_words, timeout_ms)` | 超时是毫秒参数，不是 `struct timespec` |
| `mq_close(q)` | `svcrt_mq_delete`——**队列被销毁**，见下方说明 |
| `mq_unlink(name)` | **不支持**，返回 -1 并置 `ENOSYS`（没有名字空间可解链） |

**`mq_close` 与 POSIX 不同**：POSIX 的 `mq_close` 只关闭描述符、队列本体仍在；这里没有引用计数，`mq_close` 直接销毁队列。所以队列的生死由创建/关闭它的那一个线程决定，别指望 A 关掉之后 B 还能用。同理 `mq_open` 不做查找，两个线程各自 `mq_open` 同名会得到**两个独立的队列**，不是同一个——名字只是个给内核看的标签，不是共享钥匙。跨线程共享队列的正确做法是：创建者把返回的 `mqd_t` 传给别人。

**长度单位是字（word）而不是字节**，这是照抄内核契约，不是疏忽：内核就是按字拷贝的。写 `len_words` 时按"我要传几个 32 位字"算，例如传一个 `uint32[2]` 就是 `2`。消息长度可以是 1..`SVCRT_MQ_MSG_WORDS`（= 4）字，接收侧返回**实际收到的字数**。

### 4.6 Windows 风格名字（`svcrt_win_compat.h`）

搭在 `svcrt_posix.h` 之上，只提供**有真实对应物**的调用。

| Windows | 落到 |
|---------|------|
| `Sleep(ms)` | `svcrt_task_wait(ms)` |
| `GetTickCount()` | `svcrt_get_time_ms()` |
| `CreateThread(...)` | `pthread_create` |
| `WaitForSingleObject(h, INFINITE)` | `pthread_join` |
| `InitializeCriticalSection` / `Enter` / `Leave` / `Delete` | 内核互斥量 |
| `strcpy_s` / `strcat_s` / `sprintf_s` | 兼容层自带的边界检查实现 |
| `ZeroMemory` | `svcrt_posix_memset` |

两条映射注意点：

- **`CreateThread` 的启动例程是 `DWORD(*)(LPVOID)`，线程服务要的是 `void*(*)(void*)`。** ARM 上 `WINAPI` 是空的，两者的参数与返回值都在 `r0` 里，所以直接转换函数指针调用即可。
- **`WaitForSingleObject` 只认"线程句柄 + INFINITE"。** 其它句柄或有限超时直接返回 `WAIT_TIMEOUT`。原因：有限等待若超时，必须在**不收尸**的情况下退出，而调用者无法把"还在跑、句柄仍有效"和"已结束"区分开——所以宁可明说做不到。

**明确不支持**：`CloseHandle` 返回 `FALSE` 并置 `EOPNOTSUPP`。线程句柄就是线程槽位，槽位由 `pthread_join` 释放；不等待就"关闭"会让线程永远占着槽位。

---

## 5. 为什么要新增一个内核线程服务

POSIX 的 `pthread_create` 需要创建一个线程，而**用户态没有任何途径创建一个线程**：线程要有 TCB、要有调度器槽位，这两样只有内核持有。所以内核新增了线程服务 **SVC 0x1B**，对内暴露三个调用（`svcrt.h`）：

```c
int32 svcrt_thread_create(void (*entry)(void), void *stack, uint32 stack_size,
                          uint32 priority, uint32 period_ms);
int32 svcrt_thread_self(void);
void  svcrt_thread_exit(void);
```

兼容层在这三个之上补齐了 POSIX 语义。这里有一段值得写下来的设计：**启动门闩**。

内核的线程服务只接受一个裸的 `void (*)(void)`，而 POSIX 交出来的是"启动例程 + 一个参数"。桥接靠一个 trampoline 加每线程一个槽位：

1. `pthread_create` 先在槽位表里占一格，把 `start_routine`、`arg`、栈指针等**全部写好**；
2. 调 `svcrt_thread_create` 建线程，内核返回任务号；
3. 把任务号也写进槽位；
4. **此刻才 `sem_post` 释放启动门闩。**

新线程一进来先 `sem_wait(gate)`，等门闩开了再通过自己的任务号找到槽位，取出例程和参数去执行。

为什么不用更省事的做法？因为如果建线程后立刻放它跑，新线程可能在槽位还没填完时就被调度上 CPU，读到半成品槽位。门闩把顺序变成**显式的**，而不是赌抢占时机。代价是每个线程多一个信号量，换掉一类极难复现的偶发错误。

线程完成后会 `post` 第二个信号量（`done`），`pthread_join` 等它，然后释放槽位。两个信号量名字必须挤在 8 字节内（`pth0g` / `pth0d`，末位是线程序号）。

---

## 6. App 侧堆

兼容层自带一个 **first fit + 相邻块合并**的小分配器，位于 App 自己的 RAM 里，默认 1 KB。

- 接口：`svcrt_posix_malloc` / `free` / `calloc` / `realloc`，另有 `svcrt_posix_heap_total()` 与 `svcrt_posix_heap_free_bytes()`。
- 传入不在 arena 内的指针时**拒绝**，而不是就地写坏内存。
- 刻意**不做成内核服务**：内核没有堆，MPU 已经把 App 关在自己的窗口里，分配器与调用者处在特权边界的同一侧，是少一个出错环节。

**`SVCRT_POSIX_WRAP_STDLIB` 默认关闭。** 打开后 `malloc/free/calloc/realloc` 会被宏替换到本 arena。关闭的理由：一份原本用工具链 libc 堆的程序，被静默换到 1 KB 的 arena 上，会在运行期以 `ENOMEM` 的形式"莫名其妙"地失败；让 `malloc` 保持原样，要换堆就让用户显式打开这个宏来表态。

---

## 7. 真机自测覆盖

`example/stm32f427/app_sdk/APP_DEMO` 的第九节自测直接用兼容层的**公开名字**调用（`pthread_create`、`sem_wait`、`mq_receive`、`CreateThread`、`strcpy_s` …），因此编译期就验证了"名字映射得通"，运行期验证了"语义对得上"。

真机输出（F427 + DAPLink，115200）全部为 `OK`：

```
posix.malloc / calloc_zero / realloc / heap_reclaim      OK
posix.pthread_create / pthread_join / pthread_ran / pthread_retval   OK
posix.mutex / sem_trywait / sem_empty / sem_wait / usleep            OK
win.createthread / wait_infinite / thread_ran / wait_timeout         OK
win.strcpy_s / strcpy_s_bounds / sprintf_s                           OK
```

其中几个判据值得注意，它们是用行为而不是"返回值非负"来判的：

- `pthread_retval`：线程 `return (void*)0x5A5A`，join 后核对 `retval == 0x5A5A`——验证的是返回值真的穿过 trampoline 回来了。
- `pthread_ran`：线程每轮 `+1`，join 后核对计数变化等于预期次数——验证线程真的跑了且跑在对的次数上。
- `heap_reclaim`：`free` 之后空闲字节数回到接近 `malloc` 之前——验证合并真的做了。
- `sem_empty`：计数为 0 时 `sem_trywait` 必须失败——验证"空即失败"，而不是误报成功。
- `win.strcpy_s_bounds`：故意用小缓冲，必须返回**负数**——验证边界检查真的拦了。

---

## 8. 使用建议

- **只想跑一份现成 POSIX 程序**：`#include "svcrt_posix.h"`，工程里加 `svcrt_posix.c`，把 `main` 的返回值和参数处理掉，其余基本不动。
- **给客户二次开发的 App 模板**：用 `svcrt_win_compat.h`。客户熟悉的名字都在，而这些名字背后是真实现，不会出现"编译过了但行为不对"。
- **RAM 紧张的驱动 App**：别引入伞头。只用需要的单头，且把 `SVCRT_POSIX_THREAD_MAX` 降到实际需要值——线程栈是静态的，不用的线程不要留位置。
- **需要排队的数据传递**：优先 `mqueue`（以内核消息队列为底），不要自己拿信号量 + 全局数组凑——那样既没有长度语义，也要自己处理竞争。
- **不要指望 `errno` 在热循环里免费**：一次 SVC。需要就存本地。

---

## 9. 相关文件

| 路径 | 内容 |
|------|------|
| `kernelsrc/sdk/posix/` | 兼容层全部头文件与实现 |
| `kernelsrc/include/svcrt.h` | 线程服务（`svcrt_thread_*`）与各内核服务契约 |
| `kernelsrc/src/svcrt_loader.c` | 线程服务的 SVC 分发 |
| `example/stm32f427/app_sdk/APP_DEMO/Src/app_demo.c` | 第九节自测（兼容层的真机验证） |
| `docs/配置区与安装策略.md` | 槽位 RAM 窗口与安装策略 |
| `docs/SVCrtOS应用安装与调试指南.md` | 打包、安装、上板全流程 |
| `docs/api/SVCrtOS_API参考.md` | SDK 接口参考 |
