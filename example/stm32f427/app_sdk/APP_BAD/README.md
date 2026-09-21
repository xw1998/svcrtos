# SVCrtOS App SDK 越权探针工程（APP_BAD）

**这不是示例工程，是内存保护的反向验证镜像。**

它和普通 App 一样以**非特权态**任务运行，但启动后故意做两件被 MPU 禁止的事，用来确认：

1. 保护真的拦得住；
2. 拦下来之后故障被如实记录（shell 的 `fault`）；
3. 整机不受影响（内核、其它槽、shell 都还活着）。

| 探针 | 行为 | 期望 |
|---|---|---|
| probe1 | 写内核私有 RAM 的字（`0x20003000`，不在本 App 的 MPU 窗口内） | MemManage |
| probe2 | 非特权态写 `SCB->SHCSR`（`0xE000ED24`） | BusFault / HardFault |

两个探针**各自只跑一次**（用内核的故障累计数当"走到哪一步"的标记，见 `app_bad.c` 注释）：
一路撞下去的话第三次会把整槽禁掉，而故障环又会在随后的复位里被清空，什么都看不到。

## 目录结构

```
APP_BAD/
├── MDK-ARM/
│   └── app_bad.uvprojx     # Keil MDK 工程（scatter 由 BeforeMake 钩子生成）
└── Src/
    └── app_bad.c           # 探针主体，含 AppMain() 入口
```

## 用法

```bash
py -3 tools/pack_app.py --project example/stm32f427/app_sdk/APP_BAD/MDK-ARM/app_bad.uvprojx \
      --type app --name APP_BAD --out build/APP_BAD/APP_BAD.svcapp
py -3 tools/fs_put.py build/APP_BAD/APP_BAD.svcapp --port COM3 --path /APP_BAD.svcapp

# 设备 shell：
#   fs mount
#   app install /APP_BAD.svcapp
#   fault          <- 期望看到 MEMFAULT / BUSFAULT + RECOVER，task 指向本槽
#   app list       <- 期望 crash 计数 +1，槽仍 RUNNING（连续 3 次才 INVALID + crash-limit）
```

完整结论与已知边界见 [`docs/内存保护与App越权验证.md`](../../../../docs/内存保护与App越权验证.md)。
