# SVCrtOS App SDK 用户态应用示例工程

本示例展示如何使用 **App SDK** 开发一个独立编译的用户态应用程序，编译为 `.bin` 固件，烧录到指定 ROM 分区，由 SVCrtOS 内核加载运行。

## 目录结构

```
APP_DEMO/
├── MDK-ARM/
│   ├── app_demo.uvprojx     # Keil MDK 工程文件
│   └── app_demo.sct         # 链接 scatter file
└── Src/
    ├── app_demo.c           # 应用主体，含 AppMain() 入口
    └── app_config.c         # 应用配置表（svcrt_app_cfg_table）
```

工程引用的 SDK 文件位于 `kernelsrc/sdk/app_sdk/`：
- `svcrt_app_main.c` — 提供 `main()` 入口，调用用户实现的 `AppMain()`
- `svcrt_oslib.c` — SVC 系统调用封装
- `svcrt_app_start.s` — RESET 向量，导出 APPSTART 入口点

## 分区布局

| 区域 | 起始地址 | 大小 | 说明 |
|------|----------|------|------|
| App ROM | 0x08020000 | 128KB | 应用代码只读区 |
| App RAM | 0x20010000 | 16KB | 应用读写数据区 |
| App 栈 | 0x20014000 | 2KB | 应用独立栈空间 |

> 内核 ROM 在 0x08000000，内核 RAM 不与 App RAM 重叠（从 0x20010000 起）。
> 驱动固件位于 0x08040000，与 App 互不冲突。如需修改地址请同步更新
> `app_demo.sct` 与 `app_config.c` 中的配置项。

## 编译步骤

1. 使用 Keil MDK（ARMCC V5.06）打开 `MDK-ARM/app_demo.uvprojx`
2. 确认 Device 为 STM32F427VGTx
3. 编译生成 `APP_DEMO/app_demo.axf` 或 `.hex`
4. 用 fromelf 转换为 bin：`fromelf --bin --output=app_demo.bin app_demo.axf`

## 烧录与运行

1. 烧录内核固件（SVCRTOS_TEST）到 0x08000000
2. 烧录应用固件 `app_demo.bin` 到 0x08020000
3. 系统启动后内核会加载并调用 `AppMain()` 开始执行

## 示例功能概览

`app_demo.c` 中的 `AppMain()` 演示了：
- **设备 IO**：`svcrt_dev_open/write/read`（LED 与串口）
- **任务延时**：`svcrt_task_wait`
- **信号量**：`svcrt_sem_create/post`
- **互斥锁**：`svcrt_mutex_create/lock/unlock`

## 注意事项

1. 应用代码不能直接访问硬件寄存器，必须通过 `svcrt_dev_*` 接口
2. `AppMain()` 不能返回，必须包含无限循环
3. 不要在栈上分配大数组，栈空间仅 2KB
4. 仅需包含 `svcrt.h` 即可使用全部 API