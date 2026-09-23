# SVCrtOS 小程序 SDK（mini_sdk）

小程序是**第三种用户态应用**（`type = 3`，见 `kernelsrc/include/svcrt_app_image.h`）：
它以普通文件的形式常驻文件系统，`mini run <path>` 时被内核读进 RAM 执行，
退出后代码块与 RAM 块立刻归还——牺牲加载效率，换取"不用分区、不用安装、
改一个文件就能换一个程序"。

## 目录

| 文件 | 作用 |
|------|------|
| `svcrt_mini_start.s` | `MINISTART` 桩：跳到 C 运行时 `__main`（.data/.bss 初始化后进 `main()`） |
| `svcrt_mini_main.c` | `main()` 骨架：调用 `MiniMain()`，返回后 `svcrt_thread_exit()` 交还内存 |
| `svcrt_types.h` | 基础类型 + `SVCRT_WEAK`（与 App / 驱动 SDK 同名文件一致） |

`svcrt_oslib.c`（SVC 门面实现）**与 App SDK 共用同一份**
（`kernelsrc/sdk/app_sdk/svcrt_oslib.c`）。两处各放一份迟早会漂移，
而这份门面本来就不区分镜像类型。

## 怎么用

1. 写你自己的 `MiniMain()`：

   ```c
   #include "svcrt.h"

   void MiniMain(void)
   {
       int32 con = svcrt_dev_open("COM1", 0);
       svcrt_dev_write(con, (void *)"hello from mini\r\n", 17);
       /* 返回即退出，内存归还 */
   }
   ```

2. 工程里加进 `mini_sdk` 的两个源文件 + `app_sdk/svcrt_oslib.c`，
   包含路径加 `kernelsrc/sdk/mini_sdk`、`kernelsrc/sdk/app_sdk`、`kernelsrc/include`。

3. 打包（四遍差分链接 + 重定位表，与 App 同一条流水线）：

   ```
   py -3 tools/pack_app.py --project <你的工程>.uvprojx --type miniapp \
           --name MINI_DEMO --out build/mini_demo.svcapp
   ```

4. 放进文件系统并运行：

   ```
   # 主机侧：py -3 tools/fs_put.py --port COM3 --path /mini_demo.app <镜像>
   # 文件系统里没有 mkdir 命令，路径直接用卷根下的文件名
   mini run /mini_demo.app
   mini stat
   ```

## 与 App 的差别（务必知道）

| 项 | App | 小程序 |
|----|-----|--------|
| 落点 | 镜像池的固定槽位（安装 + 版本管理） | 运行期从 RAM 池借两块 pow2 块，退出即还 |
| 常驻位置 | Flash 镜像池 | 文件系统（NOR 卷） |
| 内存占用 | 代码在 Flash，RAM = 声明的 ram_size | 代码块 + RAM 块**都在 RAM**，两块同生同死 |
| 体积上限 | 池可用空间 | 代码块 ≤ `SLOT_RAM_MAX_BLOCK`，可由 `mini limit` 运行期收紧 |
| 启动命令 | `app start <slot>` | `mini run <path>` |

上限、两块布局的由来与已知限制见 `docs/小程序设计.md`。
