# SVCrtOS Driver SDK 用户态驱动示例工程

本示例展示如何使用 **Driver SDK 用户态模式** 开发一个独立编译的虚拟温度传感器驱动，编译为 `.bin` 固件，通过 SVC 调用注册到内核，供应用程序通过 `svcrt_dev_open("TEMP", 0)` 使用。

## 目录结构

```
DRV_DEMO/
├── MDK-ARM/
│   ├── drv_demo.uvprojx     # Keil MDK 工程文件
│   └── drv_demo.sct         # 链接 scatter file
└── Src/
    └── temp_drv.c           # 驱动实现，含 DrvMain() 入口
```

工程引用的 SDK 文件位于 `kernelsrc/sdk/driver_sdk/`：
- `svcrt_drv_main.c`  — 提供 `main()` 入口，调用用户实现的 `DrvMain()`
- `svcrt_driver_bridge.c` — 注册桥接层，通过 SVC 0x14 调用内核
- `svcrt_drv_oslib.c` — 用户态 OS 接口封装
- `svcrt_drv_start.s` — RESET 向量，导出 DRVSTART 入口点

## 编译模式

工程已定义宏 **`SVCRT_DRV_USER_MODE`**，使 `svcrt_driver_bridge.c` 通过 SVC 0x14
陷入内核完成驱动注册。未定义此宏时 SDK 默认为内核态模式（静态库链接）。

## 分区布局

| 区域 | 起始地址 | 大小 | 说明 |
|------|----------|------|------|
| 驱动 ROM | 0x08040000 | 128KB | 驱动代码只读区 |
| 驱动 RAM | 0x20018000 | 8KB | 驱动读写数据区 |
| 驱动栈 | 0x2001A000 | 2KB | 驱动独立栈空间 |

> 内核 ROM 位于 0x08000000，App ROM 位于 0x08020000，互不冲突。

## 编译步骤

1. 使用 Keil MDK（ARMCC V5.06）打开 `MDK-ARM/drv_demo.uvprojx`
2. 确认 Device 为 STM32F427VGTx，宏定义包含 `SVCRT_DRV_USER_MODE`
3. 编译生成 `DRV_DEMO/drv_demo.axf` 或 `.hex`
4. 转换为 bin：`fromelf --bin --output=drv_demo.bin drv_demo.axf`

## 烧录与运行流程

1. 烧录内核固件到 0x08000000
2. 烧录 App 固件 `app_demo.bin` 到 0x08020000
3. 烧录驱动固件 `drv_demo.bin` 到 0x08040000
4. 内核启动后加载 App 和驱动，驱动 `DrvMain()` 注册 "TEMP" 设备并进入等待循环

## 驱动实现说明

`temp_drv.c` 实现了一个虚拟温度传感器：
- **5 个标准接口**：open/close/read/write/ctrl
- **虚拟设备**：无硬件依赖，write 返回错误
- **温度单位切换**：通过 `TEMP_CTRL_SET_UNIT` 切换摄氏度/华氏度
- **驱动注册**：在 `DrvMain()` 中调用 `svcrt_drv_register("TEMP", ...)`

## 应用侧调用示例

```c
int32 temp = svcrt_dev_open("TEMP", 0);
uint8 buf[2];
svcrt_dev_read(temp, buf, 2);          // 读取温度值（0.1 度精度）
int16 t = (int16)(buf[0] | (buf[1] << 8));
svcrt_dev_ctrl(temp, 0x0100, 1);       // 切换为华氏度
```

## 注意事项

1. 设备对象结构体第一个成员必须是 `svcrt_dev_hdr_t`
2. `DrvMain()` 不能返回，必须包含无限循环
3. 用户态驱动不能直接操作硬件寄存器，需通过内核代理
4. 仅需包含 `svcrt_driver_sdk.h` 即可使用全部 API