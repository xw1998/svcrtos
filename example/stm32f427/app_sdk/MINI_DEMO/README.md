# MINI_DEMO —— 小程序示例工程

演示 SVCrtOS 的第三种用户态应用（**小程序**，`type = 3`）：程序常驻文件系统，
`mini run` 装载进 RAM 执行，退出后内存立刻归还。

## 与 APP_DEMO 的关系

工程结构、编译器选项、差分链接流水线全部与 `APP_DEMO` 一样，只有三处不同：

| 项 | APP_DEMO | MINI_DEMO |
|----|----------|-----------|
| SDK | `kernelsrc/sdk/app_sdk`（`svcrt_app_start.s` / `svcrt_app_main.c`） | `kernelsrc/sdk/mini_sdk`（`svcrt_mini_start.s` / `svcrt_mini_main.c`） |
| 入口符号 | `APPSTART` | `MINISTART` |
| `gen_app_sct.py --type` | `app` | `miniapp`（栈与 App 同口径；只影响打包与 .sct 生成） |

`svcrt_oslib.c` 与 App SDK 共用同一份（见 `kernelsrc/sdk/mini_sdk/README.md`）。
本工程不链接 POSIX 库——小程序的卖点之一就是小。

## 怎么打包

```
py -3 tools/pack_app.py --project example/stm32f427/app_sdk/MINI_DEMO/MDK-ARM/mini_demo.uvprojx \
        --type miniapp --no-autostart --name MINI_DEMO --out build/mini_demo.svcapp
```

`--no-autostart` 不能省：小程序的默认类型是「自启」，不显式关掉时 `pack_app.py` 会直接
报错（`--autostart 对小程序无效`）。

打包工具会编译四遍（标称 / ROM+delta / RAM+delta / 双 delta 验证）并生成重定位表，
与 App 走的是同一条流水线。打包期会顺带核对小程序的两条硬约束：

- 代码块 = `pow2(负载长度)` 必须 ≤ `SLOT_RAM_MAX_BLOCK`；
- 代码块 + RAM 块必须 ≤ RAM 池总量。

## 怎么上板跑

```
# 主机侧送进设备文件系统（卷根下，文件系统没有 mkdir 命令）
py -3 tools/fs_put.py --port COM3 --path /mini_demo.app build/mini_demo.svcapp
# 设备侧运行与观察
mini run /mini_demo.app
mini stat
```
