# POSIX_DEMO：一份未修改的 POSIX 程序，直接编成 SVCrtOS App

这份示例要证明一句话：**把一个为 Linux / Windows 写的控制台程序搬到 SVCrtOS 上，改的是 include 列表，不是程序结构。**

`Src/posix_demo.c` 里没有 `svcrt.h`、没有控制台设备名、没有地址、没有分区号和槽位号。它只用
`<stdio.h>` / `<string.h>` / `<stdlib.h>` / `<ctype.h>` 加 `<unistd.h>` / `<pthread.h>` / `<semaphore.h>`。
同一份源码在 Linux 上 `cc posix_demo.c -pthread` 也能编。

## 编译与烧录

```bash
# 编译（-o 落绝对路径；退出码 1 只表示有 warning）
env -u PYTHONHOME -u PYTHONPATH "D:/Keil_v5/UV4/UV4.exe" -r \
    example/stm32f427/app_sdk/POSIX_DEMO/MDK-ARM/posix_demo.uvprojx -o "C:\\tmp\\posix_demo.log"

# 烧录（开发槽 3 → 裸镜像基址 0x080A0000，独立调用，不要和 -r 串 &&）
env -u PYTHONHOME -u PYTHONPATH "D:/Keil_v5/UV4/UV4.exe" -f \
    example/stm32f427/app_sdk/POSIX_DEMO/MDK-ARM/posix_demo.uvprojx -o "C:\\tmp\\posix_flash.log"
```

复位后控制台（COM3 / 115200 8N1）应出现 `POSIX demo:` 段落，随后每 5 秒一行 `heartbeat`。

## 工程里三处不能少的东西

| 位置 | 为什么 |
|------|--------|
| BeforeMake 钩子 `--heap-size 2048` | `posix_demo.c` 用了 `malloc`。散列默认不划库堆区，不给就会在链接期报 `L6915E: Heap was used, but no heap region was defined` |
| 源文件里**没有** `svcrt_app_main.c` | 它自带强 `int main(void)`，与用户 `main` 一起会产出两个 `__ARM_use_no_argv` 定义，链接期 `L6200E` |
| `.uvoptx` 的 `<TargetName>` 必须是 `POSIX_DEMO` | 它是 Keil 下载/调试入口的索引。复制别的工程当模板忘了改，`UV4 -f` 会以 `Internal DLL Error` / `Flash Download failed - Target DLL has been cancelled` 失败——现象像硬件坏了，实际只是这一个字段 |

## 验证结论

**上板验证过**（F427 + DAPLink）：`printf` 真的从控制台出来，`pthread_create` / `pthread_join` /
`sem_wait` / `qsort` / `malloc` / `sleep` 在板上按 POSIX 语义工作；`app list` 里它是 RUNNING 的那个实例
（分区表 id 5，base `0x080A0000`）。完整输出与逐条结论见 `docs/POSIX与Windows兼容层说明.md` 第 10 节。
