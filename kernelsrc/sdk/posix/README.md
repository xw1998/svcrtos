# POSIX / Windows 兼容层

让一份为 Linux / Windows 写的 C 程序在 SVCrtOS 上编译运行，**改 include 列表，不改程序结构**。

```c
#include "svcrt_posix.h"          /* 一次引入全部 */
/* 或只用一个模块 */
#include "semaphore.h"
/* 或要 Windows 风格的名字 */
#include "svcrt_win_compat.h"
```

工程里把 `svcrt_posix.c` 加入编译，没有别的构建步骤。

## 快速上手

```c
#include "svcrt_posix.h"

static void *worker(void *arg)
{
    svcrt_posix_sleep(1);
    return arg;
}

void app_main(void)
{
    pthread_t th;
    void *ret;
    sem_t sem;

    sem_init(&sem, 0, 0);
    sem_post(&sem);
    sem_wait(&sem);            /* 立刻返回 */
    sem_destroy(&sem);

    pthread_create(&th, 0, worker, (void *)0x1234);
    pthread_join(th, &ret);    /* ret == 0x1234 */
}
```

Windows 风格：

```c
#include "svcrt_win_compat.h"

svcrt_HANDLE h = CreateThread(0, 1024u, thread_fn, 0, 0u, 0);
WaitForSingleObject(h, INFINITE);   /* 只支持 INFINITE */
```

## 档位（包含头文件之前定义，或写在工程预定义宏里）

| 宏 | 默认 | 含义 |
|----|------|------|
| `SVCRT_POSIX_THREAD_MAX` | 2 | 并存的 pthread 上限 |
| `SVCRT_POSIX_STACK_WORDS` | 256 | 每线程栈字数（1 KB） |
| `SVCRT_POSIX_HEAP_SIZE` | 1024 | App 侧堆 arena 字节 |
| `SVCRT_POSIX_DEFAULT_PRIO` | 10 | 线程默认优先级 |
| `SVCRT_POSIX_NO_TIMESPEC` | 未定义 | 平台已定义 `struct timespec` 时定义它 |
| `SVCRT_POSIX_WRAP_STDLIB` | 未定义 | 把 `malloc/free/...` 映射到本层 arena |

默认档位是按**开发槽位的 8 KB RAM 窗口**配的（2 × 1 KB 线程栈 + 1 KB 堆，静态占用约 3.4 KB）。槽位 RAM 更大、或 App 不用线程时，按项目调整。

## 文件

| 文件 | 内容 |
|------|------|
| `svcrt_posix.h` | 伞头：档位、堆接口、引入其余全部 |
| `svcrt_posix.c` | 实现（errno / 堆 / 各 facade / Windows 名字） |
| `svcrt_posix_types.h` | `ssize_t` / `off_t` / `time_t` / `struct timespec` |
| `errno.h` | 错误码 + 每线程 `errno` |
| `fcntl.h` | `open` 标志 |
| `unistd.h` | `read` / `write` / `close` / `sleep` / `usleep` / `nanosleep` / `time` |
| `time.h` | `clock_gettime` / `sched_yield` |
| `pthread.h` | 线程、互斥量 |
| `semaphore.h` | 无名信号量 |
| `mqueue.h` | 消息队列（长度单位：**字**） |
| `svcrt_win_compat.h` | Windows 常用名转接 |

## 三条硬边界

1. **不支持的一律报错，不做假实现。** `sem_getvalue` / `mq_unlink` / `CloseHandle` 返回失败并置 `errno`；条件变量、`fork`/`exec`、`O_CREAT`、`localtime` 直接不提供，用了就是编译错误。
2. **长度单位看契约。** `mq_send`/`mq_receive` 的 `len` 是**字**；`sem_timedwait` / `mq_receive` 的超时是**毫秒**。核内就是这两个单位，这里不换算，免得两处口径。
3. **`errno` 每次访问是一次 SVC。** 每线程一格，存在 App RAM。热循环里请缓存到局部变量。

时间只有"启动以来的节拍"，没有 RTC，`CLOCK_REALTIME` 别名到单调时钟。

完整说明（能力表、不支持项及原因、pthread 启动门闩、堆设计、真机自测覆盖）见 `docs/POSIX与Windows兼容层说明.md`。
