# 内存保护与外设权限（App 越权验证）

> 结论分三档：**仅编译通过** / **上板验证过** / **未验证**。
> 本文记录的内容全部是**上板验证过**，命令与读数逐条可复现。
> 平台：STM32F427VGTx（DAPLink / SWD），内核 F427 工程，App 走 `app_sdk/APP_DEMO`。

## 1. 这份文档回答什么

「让 MCU 像手机一样安装应用」的前提是：**装进来的第三方 App 不能碰内核和别的 App**。
本文说明这条路现在走到了哪一步：机制是什么、怎么验证的、哪几条边界**没有**解决（写清楚，
免得被当成已经解决）。

## 2. 机制：三层缺一不可

| 层 | 做什么 | 位置 |
|---|---|---|
| 编译期 | App 按标称布局链接，安装时由内核把 RAM/ROM 绝对地址重定位到**实际落点** | `tools/gen_scatter.py`、`svcrt_loader_reloc_apply()` |
| 任务建立期 | 每个任务的 TCB 带 `is_priv`；App 任务 = 非特权（CONTROL.nPRIV = 1），内核任务 = 特权 | `svcrt_task.h:is_priv`、`svcrt_cfg.c`、`svcrt_task.c:svcrt_sched_activate()` |
| 运行期 | MPU 为每个任务建 region：本任务的 RAM 窗口（RW，XN）、内核 RAM / 外设（按需只读或禁入） | `svcrt_mpu.c:svcrt_mpu_build_task()` |

App 的 RAM 窗口来自 `config/svcrt_partition.h` 的 `SLOT_RAM_*`：安装时内核从镜像池里分配
一个 2 的幂大小的块，把基址交给 MPU 建 region，同时把**这个基址**用于 RAM 重定位。

## 3. 本轮修复：RAM 基址必须固化（否则 App 一启动就复位）

### 症状

`app start <slot>` 后设备**复位**（`RCC_CSR` 读出 IWDGRSTF），槽位被 crash-limit 禁用。
用 `DEMCR.VC_MMERR` 向量捕获抓到出错现场：

```
PC  = 0x080650AA   (__scatterload_copy: stmhs r1!,{r3,r4,r5,r6})
R1  = 0x20014000   (写入目标 = App 自己 RAM 窗口的排他上界)
CFSR = 0x82 (MMARVALID | DACCVIOL)   MMFAR = 0x20014000
```

### 根因

App 启动时 `__scatterload` 按镜像里**已固化的** scatter 表把 RW 初始化数据搬到 RAM，
表里的 `dest` 就是安装那一刻的 RAM 基址。而 **RAM 分配结果不落盘**：上电扫描会按
「池内地址升序」重新分配一次。只要两次分配的结果不同（**重复安装**时旧绑定仍占着池，
就会不同），镜像里所有 RAM 绝对地址立刻作废 —— 于是写到 `0x20014000`，正好越出当前
8 KB 窗口，撞 MPU。

板上两组读数（同一份镜像，同一块板）：

| | RAM 基址 | 来源 |
|---|---|---|
| 安装当时（固化进 Flash） | `0x20014000` | 设备 Flash 里 scatter 表 `dest` 字段 |
| 上电重建（原来的行为） | `0x20012000` | 板上分区表 `slot_ram_base[1]` |

原代码注释里写着「分配顺序 = 池内地址升序，重建结果与装载时一致」——这个假设在重复安装
下不成立。

### 修法

**把安装时分配到的 RAM 基址写进镜像头，上电扫描优先复用它，不再重新分配。**

| 改动 | 文件 |
|---|---|
| 镜像头末尾新增 `runtime_ram_base`（头长与其它字段偏移**不变**；老镜像该处本来就是 0 → 天然向后兼容） | `kernelsrc/include/svcrt_app_image.h` |
| 新增 `svcrt_ptable_ram_reserve(base, size)`：按指定基址占用，校验越界 / 对齐 / 重叠 | `kernelsrc/src/svcrt_ptable.c` |
| 安装时写入分配到的基址；扫描时**优先**按它预留，**要不到就明确报错、让该镜像暂时没有 RAM 块**（`start` 会拒启并要求重装），而不是换一个地址把它跑起来 | `kernelsrc/src/svcrt_loader.c` |
| CRC：`runtime_ram_base` 与 `crc32`/`state` 一样**按 0 代入**（它不是"传输来的内容"，是安装方落盘前才填的本地信息；算进去会让每次安装都误报 CRC 不符） | `svcrt_loader.c` 两处 CRC |
| 打包侧：`verify` 时若该字段非 0 直接报错（那说明拿板上拆下来的镜像当打包产物了） | `tools/pack_app.py` |

**为什么要"宁可报错"**：RAM 基址一变，镜像里成百上千个绝对地址同时失效。换一个地址把它
启动起来，只会得到一次看不懂的 MemManage；报错说清楚"要重装"，是唯一诚实的答案。

## 4. 验证

### 4.1 正向：App 在非特权态跑完整自检

```
app install /APP_DEMO.svcapp      ->  app install: ok, slot 2
APP_TEST summary: pass=78 fail=0
APP_ALIVE t=... cpu=0% ...        （持续 110 s 无复位）
```

覆盖设备 IO、任务/延时、信号量/互斥量/事件、消息队列、软件定时器、时间基、CPU 占用、
故障计数、槽位状态、日志、shell 注册、**FPU**、POSIX/Win 兼容层、文件系统。
全部通过，说明非特权态下 SVC 通道、共享窗口、FPU 使用都没有被 MPU 误伤。

### 4.2 反向：两个故意越权的镜像

`example/stm32f427/app_sdk/APP_BAD`（非示例，是探针）：

| 探针 | 行为 | 期望 |
|---|---|---|
| probe1 | 写内核私有 RAM 的字（`0x20003000`，不在本 App 窗口内） | MemManage |
| probe2 | 非特权态写 `SCB->SHCSR`（`0xE000ED24`） | BusFault / HardFault |

设备 `fault` 读数：

```
#   tick      task  type
 0   134746    7     MEMFAULT    <- probe1
 1   134746    7     RECOVER
 2   190824    8     BUSFAULT    <- probe2
 3   190824    8     RECOVER
```

同时：

- 探针后面的 `"NOT blocked"` 分支**一次都没打印** → 两次越权都被拦下了
- 两个探针槽（4、5）`crash=1` 且仍然 `RUNNING` → 内核按终局策略**恢复任务**而不是整机崩
- 另有一个把探针一路撞下去的实例（槽 3）`crash=3`、`INVALID`、`held=crash-limit` → **连续越权会被禁用**，正是想要的终局
- 期间 shell 一直可敲（`app list` / `fault` 正常应答），槽 2 的 APP_DEMO 继续心跳 → **整机不受影响**

## 5. 已知边界（**没有**解决的，别当成已解决）

1. **只约束非特权任务。** 内核任务（含驱动框架里以特权态跑的代码）不受 MPU 限制；这是设计，不是遗漏。
2. **共享窗口对 App 仍可写。** 共享分区表与崩溃日志同处一个窗口，把该窗口改成只读会挡住内核自己的写路径，所以保持可写。App 理论上能改分区表字段（当前不作为安全边界）。
3. **App 注册的 shell 回调在特权上下文执行 App 代码。** shell 在内核任务里跑，回调里的 App 代码因此拿到特权；要修需要一个 SVC 蹦床 + 非特权栈，成本不低，暂未做。
4. **`svcrt_loader_accept` 不校验负载 CRC（只校验头）**，`signature[64]` 也没有验签 —— 按既定裁决留给后续（当前只做完整性：版本单调 + image_id + CRC）。
5. **故障环在复位后清空**（在 RAM）。要看"刚发生那一刻"的记录，必须在复位前读；跨复位的崩溃计数由 `svcrt_crash_journal`（`.noinit` 区）承担。

## 6. 复现步骤

```
# 1. 编译并烧录内核（F427）
UV4 -r example/stm32f427/kernel/SVCRTOS_TEST/MDK-ARM/SVCRTOS_TEST.uvprojx
UV4 -f example/stm32f427/kernel/SVCRTOS_TEST/MDK-ARM/SVCRTOS_TEST.uvprojx

# 2. 打包并上传 App
py -3 tools/pack_app.py --project example/stm32f427/app_sdk/APP_DEMO/MDK-ARM/app_demo.uvprojx \
      --type app --name APP_DEMO --out build/APP_DEMO/APP_DEMO.svcapp
py -3 tools/fs_put.py build/APP_DEMO/APP_DEMO.svcapp --port COM3 --path /APP_DEMO.svcapp

# 3. 在 shell 里
fs mount
app install /APP_DEMO.svcapp     # 期望 pass=78 fail=0

# 4. 反向验证（可选）
py -3 tools/pack_app.py --project example/stm32f427/app_sdk/APP_BAD/MDK-ARM/app_bad.uvprojx \
      --type app --name APP_BAD --out build/APP_BAD/APP_BAD.svcapp
py -3 tools/fs_put.py build/APP_BAD/APP_BAD.svcapp --port COM3 --path /APP_BAD.svcapp
# app install /APP_BAD.svcapp ; fault ; app list
```

> 取证提示：长时内存转储前先写 `DBGMCU_APB1_FZ` bit12（`DBG_IWDG_STOP`）冻结独立看门狗，
> 否则转储本身就会把现场复位掉；抓"出错指令"用 `DEMCR` bit4（`VC_MMERR`）做向量捕获，
> 故障处理是微秒级的，halt/read/resume 轮询必然错过。
