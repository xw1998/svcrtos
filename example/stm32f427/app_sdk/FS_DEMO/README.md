# FS_DEMO：用普通 POSIX 文件调用打到 VFS 命名空间

这份示例证明的是 W3 的 **A 面**里「文件类 POSIX」这一半：一个 App 用
`<fcntl.h>` / `<unistd.h>` / `<sys/stat.h>` / `<dirent.h>` 里那些再普通不过的
调用（`open` / `read` / `write` / `lseek` / `stat` / `fstat` / `opendir` /
`readdir` / `unlink`）就能走到内核的 VFS 命名空间，源码里没有 `svcrt.h`、没有
地址、没有分区号。

`Src/fs_demo.c` 只 include 标准头，同一份源码在 Linux 上 `cc fs_demo.c` 也能编。
它开机跑一遍自检，然后进入心跳。

## 自检覆盖什么

| 段落 | 覆盖 |
|------|------|
| 建/写/关 | `open(O_CREAT\|O_WRONLY\|O_TRUNC)`、`write` 全量、`close` |
| 读回 | 重新 `open(O_RDONLY)`，逐字节与写入内容比对 |
| 定位与元数据 | `lseek(SEEK_SET/SEEK_END)`、`fstat`、`stat`，类型位与长度 |
| 目录流 | `opendir("/")` + `readdir` 找到同一个名字；流结束时 `errno==0` |
| 设备走同一入口 | `opendir("/dev")` 看到字符设备与 `COM1`，`open("/dev/COM1")` 成功且 `fstat` 报 `S_ISCHR` —— 设备和文件用同一个 `open()`，靠类型位区分；往它写一行字（字符设备允许短写，客户端要按字节重试） |
| 错误面 | 不存在的路径 `stat`/`opendir` 必须 `-1`/`NULL` 且 `errno==ENOENT`，不许编一个像样的结果 |
| 删除 | `unlink` 后同一路径 `stat` 必须失败 |

最后一行打印 `fs_demo: pass=N fail=M`。

## 编译与烧录

```bash
# 编译（-o 落绝对路径；退出码 1 只表示有 warning）
env -u PYTHONHOME -u PYTHONPATH "D:/Keil_v5/UV4/UV4.exe" -r \
    example/stm32f427/app_sdk/FS_DEMO/MDK-ARM/fs_demo.uvprojx -o "C:\\tmp\\fs_demo.log"

# 烧录（开发槽 3 → 裸镜像基址 0x080A0000，独立调用，不要和 -r 串 &&）
env -u PYTHONHOME -u PYTHONPATH "D:/Keil_v5/UV4/UV4.exe" -f \
    example/stm32f427/app_sdk/FS_DEMO/MDK-ARM/fs_demo.uvprojx -o "C:\\tmp\\fs_flash.log"
```

复位后控制台（COM3 / 115200 8N1）应出现 `fs_demo:` 段落与每项 `ok`，
最后一行 `pass=N fail=M`，随后每 5 秒一行 `heartbeat`。

## 工程里三处不能少的东西

与 `POSIX_DEMO` 相同（同一个 App SDK 约束）：

| 位置 | 为什么 |
|------|--------|
| BeforeMake 钩子 `--heap-size 2048` | 散列默认不划库堆区；`printf` 的实现会用到堆，不给就会在链接期报 `L6915E` |
| 源文件里**没有** `svcrt_app_main.c` | 用户自带强 `main`，再链 SDK 的 `main` 会 `L6200E` |
| `.uvoptx` 的 `<TargetName>` 必须是 `FS_DEMO` | Keil 下载/调试入口按它索引；复制工程忘了改，`UV4 -f` 会以 `Internal DLL Error` / `Target DLL has been cancelled` 失败，现象像硬件坏了 |

## 验证结论

**上板验证过**（F427，AC5，写开发槽 3 后复位抓串口）：

```
  ok   open /dev/COM1 as path      
  ok   fstat is char device        
  ok   single write is not an error
[fs_demo] device opened as a path
  ok   write to device path        
fs_demo: pass=32 fail=0
```

编译同样是 0 Error / 0 Warning（`Code=8426`）。上板过程中暴露并修掉的两个真问题
（`fstat` 对字符设备求长度、devfs 把设备句柄截断成 `int16_t`）写在
`docs/POSIX与Windows兼容层说明.md` 第 11.3 节。
