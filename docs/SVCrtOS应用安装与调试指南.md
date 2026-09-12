# SVCrtOS 应用与驱动：安装、调试全流程

> 本文是**操作手册**——回答"今天我要怎么干活"。
> 设计原理、改动记录见同目录《Loader工程化落地说明.md》。

---

## 0. 三条落位路径总览

同一个 App/驱动工程，有三种把代码送上板子的方式，**三者最终都汇入同一条启动流程**
（内核扫描槽位 → 认定镜像 → 注册任务）：

| 路径 | 用于 | 产物 | 落位方式 | 需要内核重启吗 |
|---|---|---|---|---|
| **A 开发调试** | 日常写代码、下断点 | 裸镜像（`.axf`） | Keil 直接 Download | 不需要（只重下 App） |
| **B 串口安装** | 现场升级、无烧录器 | `.svcapp` | 内核安装任务经 COM1 接收 | 不需要 |
| **C 烧录写入** | 产线首件、批量 | `.svcapp` | 烧录器直接写到槽位地址 | — |

> 路径 B 目前只支持 **App**；驱动区只有扫描/启动能力，尚无流式安装（见 §10 待办）。

---

## 1. 速查表

### 1.1 分区布局（当前 `config/svcrt_partition.h`）

**Flash（1 MB）**

| 分区 | 地址范围 | 大小 | 内容 |
|---|---|---|---|
| KERNEL | `0x08000000 ~ 0x0803FFFF` | 256 KB | 内核（含安装任务） |
| DRIVER_POOL | `0x08040000 ~ 0x0807FFFF` | 256 KB | 驱动固件 |
| APP_SLOT0 | `0x08080000 ~ 0x080FFFFF` | 512 KB | App 镜像 |

**RAM（128 KB @ `0x20000000`）**

| 分区 | 地址范围 | 大小 | 用途 |
|---|---|---|---|
| SHARE_RAM | `0x20000000 ~ 0x20001FFF` | 8 KB | 分区表 |
| KERNEL_RAM | `0x20002000 ~ 0x20017FFF` | 88 KB | 内核数据 / 内核任务栈 |
| DRIVER_RAM | `0x20018000 ~ 0x2001BFFF` | 16 KB | 驱动 `.data/.bss` + 驱动栈 |
| APP_RAM | `0x2001C000 ~ 0x2001FFFF` | 16 KB | App `.data/.bss` + App 栈 |

> **改芯片容量只需改 `CHIP_FLASH_SIZE` / `CHIP_RAM_SIZE`**，其余地址与所有 `.sct` 自动重算。

### 1.2 栈的位置（由内核与 App 双方各自推导，结果必须一致）

| 映像 | RW/ZI 可用范围 | 栈顶 | 栈大小 |
|---|---|---|---|
| App | `0x2001C000` + 12 KB | `0x20020000` | 4 KB（`APP_TASK_STACK_SIZE`） |
| 驱动 | `0x20018000` + 15 KB | `0x2001C000` | 1 KB（`DRIVER_TASK_STACK_SIZE`） |

- 栈从各自 RAM 区**顶部**切出，**RW/ZI 的上限已扣掉栈区**——RW 一旦要撞栈，**链接阶段就报错**。
- App 侧 `.sct` 里的 `ARM_LIB_STACK` 指向的正是内核推导的同一个栈顶，
  所以 `__main` 设置 SP 后与内核认知一致（不存在"两份栈"）。
- App/驱动均**不生成 `ARM_LIB_HEAP`**，应用不要依赖 `malloc`。

### 1.3 策略开关（都在 `config/svcrt_partition.h`）

| 宏 | 默认 | 作用 | 什么时候改 |
|---|---|---|---|
| `APP_AUTO_START` | 1 | 上电扫描到有效 App 后自动启动 | **调试时置 0**，避免内核抢先启动 |
| `DRIVER_AUTO_START` | 1 | 扫描到有效驱动后自动启动 | 调试驱动时置 0 |
| `APP_ALLOW_RAW_IMAGE` | 1 | 允许槽位内是"裸镜像"（开发期）。置 0 则只认带镜像头的 `.svcapp` | **发布固件时置 0** |
| `APP_CRASH_RESTART_MAX` | 3 | App/驱动连续故障重启上限，达到即禁用；0 = 不限次 | 按产品可靠性要求调整 |
| `INSTALLER_ENABLE` | 1 | 是否启用内核内安装任务（占 COM1） | **不用串口安装 / 调试串口时置 0** |
| `INSTALLER_AUTO_START` | 1 | 安装完成后是否立即启动该 App | 需要"先装好、稍后手动启动"时置 0 |
| `INSTALLER_DEV_NAME` / `_ARG` | `COM1` / `115200` | 镜像接收设备与波特率 | 按实际接线调整 |

改完任何一个开关**只需重新编译内核**（`.sct` 会在编译前自动重新生成）。

---

## 2. 路径 A：开发调试（日常用这条）

### 2.1 一次性准备：确认 Keil 工程已挂接

App / 驱动 / 内核工程都已配好，检查三项（工程 → Options for Target）：

1. **Target → 不使用 Target Dialog 内存布局**（`<umfTarg>0`，即 scatter 文件生效）
2. **Linker → Scatter File** 指向生成的 `.sct`：
   - 内核 → `build/kernel.sct`
   - App → `build/app.sct`
   - 驱动 → `build/driver.sct`
3. **User → Before Build/Rebuild** 已挂上生成命令（示例）：
   ```
   python ..\..\..\..\..\tools\gen_scatter.py --target app --output ..\..\..\..\..\build\app.sct
   ```
   > 该命令需要 `python` 在系统 PATH 中。若不想让编译依赖 Python，
   > 可清空这一栏，改为手工运行 `python tools/gen_scatter.py --target all --output build`。

### 2.2 调试 App（步骤）

1. 打开 `config/svcrt_partition.h`，确认开发期设置：
   - `APP_ALLOW_RAW_IMAGE = 1`（必须，否则裸镜像不被认定）
   - `APP_AUTO_START = 1`
   - `INSTALLER_ENABLE = 0`（避免安装任务抢占 COM1，也避免内核抢先启动）
2. 编译内核 → 烧录到 `0x08000000`
3. 打开 App 工程（如 `APP_DEMO`）→ 编译（Before Build 会自动生成 `build/app.sct`）
4. **Keil 直接 Download**（会按 `.sct` 的加载区域写入 `0x08080000`）
5. 复位运行
6. 内核启动时扫描槽位 → 认定为裸镜像 → 入口取 `0x08080000 | 1` → 注册为任务并启动
7. 在 App 代码里下断点 → 命中

### 2.3 调试驱动

步骤同上，只是：

- 工程换成 `BLED_DRV` / `DRV_DEMO`，链接到 `build/driver.sct`
- 下载地址是 `0x08040000`（`DRIVER_POOL`）
- 内核先启动驱动（优先级 9，高于 App 的 10），再启动 App

### 2.4 只重下 App、不重烧内核

**这是路径 A 的核心价值**：内核不用动。

只要 App 的**链接基址没变**（即 `config/svcrt_partition.h` 没改），
重新编译 + Download App 即可，复位后内核会重新扫描并拉起新版本。

### 2.5 为什么 App 必须在"有内核"的环境里跑

App 工程里**没有 startup 文件、没有向量表**，它本身不是从头复位运行的固件：

- 它的入口 `APPSTART` 是被内核当作**任务函数**跳进来的；
- 它调用 SVC 请求内核服务，而 SVC 处理程序在**内核的向量表**（`0x08000000`）里。

所以 App 与内核是"共生"的：调试 App 时板子上必须有运行中的内核。

### 2.6 调试期建议

| 建议 | 原因 |
|---|---|
| `APP_AUTO_START = 0` | 内核不会在你下断点前就启动槽位 |
| `INSTALLER_ENABLE = 0` | 安装任务不会跟你的串口调试抢 COM1 |
| 保留 `APP_ALLOW_RAW_IMAGE = 1` | 否则裸镜像不被认定，App 不会启动 |

---

## 3. 路径 B：串口安装

### 3.1 协议

**没有任何额外帧封装**——把 `.svcapp` 文件原样连续发给串口即可：

| 项目 | 说明 |
|---|---|
| 设备 | `INSTALLER_DEV_NAME`（默认 COM1） |
| 波特率 | `INSTALLER_DEV_ARG`（默认 115200） |
| 数据格式 | 8N1，无流控 |
| 同步方式 | 内核在字节流中搜索镜像头魔数 `SVCA`（字节序 `41 43 56 53`），**逐字节重新同步**，混杂的杂散字节会被自动跳过 |
| 结束 | 不需要结束标记；内核按镜像头里的 `image_size` 读满即止 |
| 分流 | 内核看**镜像头里的 `type`** 自行决定去向：驱动镜像进驱动池，其余进空闲 App 槽位。装什么由包自己说了算，上位机不需要事先声明 |

### 3.2 打包

```bash
# 由 Keil 生成的 .axf 直接打包（推荐，自动从符号表解析入口偏移）
python tools/pack_app.py --axf build/APP_DEMO/APP_DEMO.axf \
    --type app --version 1.0.0 --name "LED 闪烁示例" \
    --out build/APP_DEMO/APP_DEMO.svcapp

# 由 .bin 打包（需明确入口偏移）
python tools/pack_app.py --bin build/APP_DEMO/APP_DEMO.bin \
    --type app --version 1.0.0 --entry-offset 0x0 \
    --out build/APP_DEMO/APP_DEMO.svcapp

# 驱动镜像：把 --type 换成 driver
python tools/pack_app.py --axf build/BLED_DRV/bled_drv.axf \
    --type driver --version 1.0.0 --name "蓝灯驱动" \
    --out build/BLED_DRV/bled_drv.svcapp
```

`--type` 必须与实际分区匹配：内核会校验镜像头里的 `type`，App 镜像进不了驱动区、
驱动镜像也进不了 App 槽位（上电扫描与运行期安装都会拦）。


打包工具会**强制校验**"ELF 镜像基址 == 分区槽位基址"，不一致直接报错——
这是防止链接布局与打包布局悄悄错位的护栏。

入口符号约定：

| 符号 | 说明 |
|---|---|
| `APPSTART` / `DRVSTART`（默认） | SDK 启动入口，先做 C 运行库初始化（`.data` 拷贝、`.bss` 清零）再进 `AppMain`/`DrvMain`。**镜像含初始化全局变量时必须用它** |
| `AppMain` / `DrvMain` | 直接进业务函数，跳过运行时初始化 |

### 3.3 发送

1. 内核编译时 `INSTALLER_ENABLE = 1`，烧录并复位
2. 上位机打开对应串口（115200 8N1）
3. 把 `.svcapp` 文件以**二进制方式**发送（不要用文本模式，会被改写行尾）
   - 命令行示例：`copy /b build\APP_DEMO\APP_DEMO.svcapp COM1`
   - 串口工具示例：选"发送文件"，不要勾"按行发送"/"添加换行"
4. 发送完成后按 `INSTALLER_AUTO_START` 决定是否立即启动

### 3.4 结果查询

安装任务写入完成后槽位状态变为 `LOADED`（随后 `RUNNING`）。应用侧可查询：

```c
uint32 st  = svcrt_app_status(0);  /* 0=空 1=已加载 2=运行中 3=无效/被禁用 */
int32  tid = svcrt_app_start(0);   /* 手动启动（INSTALLER_AUTO_START=0 时用） */
int32  r   = svcrt_app_stop(0);    /* 停止（镜像保留在 Flash） */
int32  d   = svcrt_driver_load(dev, image_len);  /* 从设备装驱动到驱动区 */
```

驱动区是**单入口**：同一时刻只驻留一份驱动，重新安装会整体覆盖，
安装成功后 `driver_state` 变 `LOADED`、`driver_entry` 指向新入口，
并按 `DRIVER_AUTO_START` 决定是否立即（重）启动驱动任务。


### 3.5 掉电/中断会怎样

安装期间槽位状态是 **`INSTALLING`**，只有**全部写完且 CRC 回读复核通过**才置 `LOADED`。

> 写一半掉电 → 状态随 RAM 一起消失 → 重新上电后内核按 CRC 校验那个半成品 →
> CRC 不过 → 判为 `INVALID` → **不会被启动**。

所以中断安装是安全的：不会留下一个"看起来可用"的坏槽位。重新发一遍即可。

---

## 4. 路径 C：打包后用烧录器写入

产线或首件需要一次性把内核 + App 都烧好时：

| 映像 | 烧录地址 | 来源 |
|---|---|---|
| 内核 | `0x08000000` | 内核工程的构建产物 |
| 驱动 | `0x08040000` | 驱动工程打包后的 `.svcapp` |
| App | `0x08080000` | App 工程打包后的 `.svcapp` |

- `.svcapp` 是"256 字节头 + 负载"的整体，**从头开始写到槽位基址即可**，不需要拆开。
- 注意：**同一个槽位要么放 `.svcapp`，要么放裸镜像，不能混**——
  带头的镜像入口是 `基址 + entry_offset`，裸镜像入口是 `基址`，两种布局不同。

---

## 5. 上电启动时序（内核视角）

```
main()
 └─ svcrt_kernel_init()
     ├─ svcrt_ptable_init()        在共享内存建立分区表，所有槽位/驱动状态清零
     ├─ svcrt_loader_scan_driver() 认定驱动区内容
     ├─ DRIVER_AUTO_START ?  → svcrt_loader_start_driver()
     ├─ svcrt_loader_scan()        逐个槽位认定内容
     ├─ APP_AUTO_START ?     → svcrt_loader_start(0)
     └─ svcrt_installer_init()     注册安装任务（INSTALLER_ENABLE=1 时）
 ...
 调度器开始运行
```

启动顺序上**驱动先于 App**，因为驱动力图先就绪供 App 使用（驱动优先级 9 > App 10）。

---

## 6. 镜像认定规则（内核怎么判断槽位里是什么）

`svcrt_loader_identify()` 对每个分区依次判断：

| 分区首字 | `APP_ALLOW_RAW_IMAGE` | 判定结果 | 入口 |
|---|---|---|---|
| `SVCA`（`0x53564341`） | — | 带镜像头 → 校验兼容签名与 CRC | `基址 + entry_offset` |
| 非 `SVCA` 且非擦除态 | 1 | **开发期裸镜像** | `基址`（ARM 置 Thumb 位） |
| 非 `SVCA` 且非擦除态 | 0 | 无效 | — |
| `0xFFFFFFFF` / `0x00000000` | — | 空槽位 | — |

- 有内容但认定失败 → 槽位置 `INVALID`（**不会被误启动**）
- `INVALID` 在语义上区分于 `EMPTY`，便于宿主机识别"这里有个坏东西"

---

## 7. 故障处理：崩溃重启与禁用

### 7.1 策略

```
App/驱动 任务 HardFault
   → 该槽位崩溃计数 +1
      ├─ 未达 APP_CRASH_RESTART_MAX(3) → 重建栈帧、重新调度（等效任务复位重启）
      └─ 达到上限                      → 禁用：置 INVALID、脱离调度、不再重启
```

- 计数**按槽位累计**，重启不会清零
- **重新安装镜像时清零**（新镜像 = 重新开始）
- 内核任务（不属于任何槽位）不受此策略影响

### 7.2 怎么观测

```c
uint32 st  = svcrt_app_status(0);      /* 3(INVALID) 且之前是 RUNNING → 多半是被禁用 */
int32  cnt = svcrt_fault_record_count();
uint32 rec[3];
for (int32 i = 0; i < cnt; i++) {
    svcrt_fault_record_read(i, rec);
    /* rec[0]=类型  rec[1]=任务号  rec[2]=tick */
}
```

故障类型：

| 值 | 含义 |
|---|---|
| 1 | HardFault |
| 2 | 任务栈溢出 |
| 3 | 任务被终止 |
| 4 | 任务故障恢复（重启了一次） |
| 5 | 调度器锁定期间调用阻塞接口（编程错误） |
| **6** | **App 连续故障达上限被禁用** |

### 7.3 被禁用后如何恢复

重新安装镜像即可（路径 B 重发一次，或路径 A/C 重新写入）——安装成功会清零计数。

### 7.4 已知边界

计数保存在**共享 RAM**，掉电即清零。所以：

- ✅ 能挡住"App 反复崩溃 → 重启 → 再崩溃"的死循环
- ❌ **挡不住"崩溃导致整机复位"的启动环**（上电 → 自动启动 → 崩溃 → 复位 → …）

要堵住启动环需把计数持久化。芯片侧最划算的做法是 **RTC 备份寄存器**
（STM32F427 有 20×4 字节，跨复位/掉电保持且无 Flash 写损耗），属板级待办。

---

## 8. 发布固件检查清单

- [ ] `APP_ALLOW_RAW_IMAGE = 0` —— 只接受带镜像头的 `.svcapp`
- [ ] `INSTALLER_ENABLE` 按产品形态决定（不用串口升级就关掉）
- [ ] `APP_AUTO_START` / `DRIVER_AUTO_START` 按需求确认
- [ ] `APP_CRASH_RESTART_MAX` 按可靠性要求确认
- [ ] `SVCRT_HW_COMPAT_ID` 与本批次硬件一致（改硬件必须改它，否则旧镜像会被拒装）
- [ ] 所有镜像用 `pack_app.py --verify` 过一遍
- [ ] 预留：`.svcapp` 的 `signature[64]` 字段目前为占位，**签名校验尚未实现**（见 §10）

---

## 9. 排错表

| 症状 | 可能原因 | 处理 |
|---|---|---|
| App 下载了但不运行 | `APP_ALLOW_RAW_IMAGE = 0`，裸镜像不被认定 | 开发期置 1 |
| App 不运行，`svcrt_app_status(0)` 返回 3 | 内容认定失败（被烧成了别的格式，或 `.svcapp` 损坏） | 重新下载/重新打包；用 `pack_app.py --verify` 检查 |
| 槽位空空（状态 0） | 下载地址不对，没写到分区基址 | 核对 `.sct` 与烧录地址（App `0x08080000`／驱动 `0x08040000`） |
| 链接报错说 RW 区放不下 | App/驱动的 `.data+.bss` 超出扣除栈后的 RW 上限 | 精简静态变量，或调大对应 RAM 分区与栈 |
| 内核编译前报找不到 `gen_scatter.py` | Keil 的 Before Build 里 `python` 不在 PATH | 把 Python 加入 PATH，或手工生成 `.sct` 后清空该栏 |
| 串口安装没反应 | `INSTALLER_ENABLE = 0`；串口被调试会话占用；发送用了文本模式 | 打开开关、释放串口、改二进制发送 |
| 安装后立即回到 INVALID | 传输丢字节导致 CRC 不过 | 降低波特率或加流控，重新发送 |
| App 跑一次就被禁用 | 代码里有 HardFault，连崩 3 次 | 查 `svcrt_fault_record_read()` 的类型与任务号定位 |
| 改了 `CHIP_FLASH_SIZE` 后地址全变 | 这是设计行为（地址只定义一次） | 只需重新编译内核与各映像，无需改任何其他文件 |

---

## 10. 相关工具与待办

### 工具

| 工具 | 用途 |
|---|---|
| `tools/gen_scatter.py` | 由配置头生成 `.sct`；`--dump` 看布局、`--check` 校验重叠越界 |
| `tools/pack_app.py` | 打包 `.svcapp`；`--info` 看镜像头、`--verify` 校验完整性 |
| `tools/gen_api_doc.py` | 生成 API 文档（见 `docs/README.md`） |

```bash
python tools/gen_scatter.py --dump                  # 打印当前分区布局
python tools/gen_scatter.py --check                 # 校验分区无重叠无越界
python tools/gen_scatter.py --target all --output build
```

### 尚未支持（按优先级）

1. **镜像签名校验**——`signature[64]` 是占位字段，还没有信任链
2. **崩溃计数持久化**——挡住"崩溃导致整机复位"的启动环（建议用 RTC 备份寄存器）
3. **槽位元数据持久化**——版本号、升级/回滚目前无法跨掉电保留
4. **多槽位 / A-B 回滚**——当前 `APP_MAX_COUNT = 1`，单区间最小闭环
5. **多驱动共存**——驱动区是单入口，同一时刻只驻留一份驱动
6. **SVC 边界零信任**——用户传入的裸指针尚未做范围校验，句柄也还是裸索引

> 本轮已补齐：驱动区的流式安装（`svcrt_driver_load` / SVC 0x18 子命令 6 /
> 安装任务按 `type` 自动分流）、任务上限 7→32、内核模块初始化统一入口。
