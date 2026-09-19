# SVCrtOS 应用与驱动：安装、调试全流程

> 本文是**操作手册**——回答"今天我要怎么干活"。
> 配置区与两种安装策略的原理见《配置区与安装策略.md》；
> 分区与镜像格式的设计动因见《Loader工程化落地说明.md》。

---

## 0. 三条落位路径总览

同一个 App/驱动工程，有三种把代码送上板子的方式，**三者最终都汇入同一条启动流程**
（内核扫描池 → 认定镜像 → 登记槽位 → 注册任务）：

| 路径 | 用于 | 产物 | 落位方式 | 需要重启内核吗 |
|---|---|---|---|---|
| **A 开发调试** | 日常写代码、下断点 | 裸镜像（`.axf`） | Keil 直接 Download 到固定开发槽位 | 不需要 |
| **B 串口安装** | 现场升级、无烧录器 | `.svcapp` | 内核经串口接收后写入池 | 不需要 |
| **C 烧录写入** | 产线首件、批量 | `.svcapp` 或裸镜像 | 烧录器直接写到槽位地址 | 不需要（但通常一起烧内核） |

三条路径的落点由**安装策略**决定（见《配置区与安装策略.md》）：

- **fixed 模式**：落点固定，路径 A 是主力（客户二次开发场景）；
- **auto 模式**：落点由内核在池内分配，路径 B 是主力（现场升级场景）。

> 路径 B 支持 App 与驱动（内核按镜像头里的 `type` 自动分流）。
> 但镜像侧的多槽位支持尚未打通，详见 §10。

---

## 1. 速查表

### 1.1 Flash 分区（F427，1 MB；源头 `config/svcrt_partition.h`）

| 分区 | 地址范围 | 大小 | 内容 |
|---|---|---|---|
| KERNEL | `0x08000000 ~ 0x0801FFFF` | 128 KB | 内核（含 shell、安装逻辑） |
| CONFIG | `0x08020000 ~ 0x0803FFFF` | 128 KB | 设备端配置记录（只用开头 512 B） |
| IMAGE_POOL | `0x08040000 ~ 0x080FFFFF` | 768 KB | **统一镜像池**：App 与驱动共用，6 个 128 KB 扇区 |

池的分配粒度是 1 KB（`SVCRT_POOL_ALLOC_UNIT`），擦除粒度是扇区（128 KB）。
一个 10 KB 的 App 在 auto 模式下只占约 11 KB，不再整扇区占用。

**F401（512 KB）**：KERNEL 128 KB + CONFIG 128 KB + 池 256 KB（2 个扇区）。

> **改芯片容量只需改 `CHIP_FLASH_SIZE`**，其余地址与所有 `.sct` 自动重算。

### 1.2 开发生命周期

四个 App/驱动工程与它们的固定落点（由 `config/svcrt_partition.h` 第八节的
开发槽位表决定，`.sct` 由 `tools/gen_scatter.py` 生成）：

| 工程 | 类型 | Flash 落点 | RAM 窗口 | 栈顶 |
|---|---|---|---|---|
| `DRV_DEMO` | 驱动 | `0x08040000`（单元 0） | `0x20010000 +16 K` | `0x20014000` |
| `APP_DEMO` | App | `0x08060000`（单元 1） | `0x20014000 +16 K` | `0x20018000` |
| `BLED_DRV` | 驱动 | `0x08080000`（单元 2） | `0x20018000 +16 K` | `0x2001C000` |
| `BLED_APP` | App | `0x080A0000`（单元 3） | `0x2001C000 +16 K` | `0x20020000` |

对应 `.sct` 分别是 `build/drv_demo.sct` / `app_demo.sct` / `bled_drv.sct` / `bled_app.sct`。

### 1.3 RAM 布局（F427，128 KB）

| 分区 | 地址范围 | 大小 | 用途 |
|---|---|---|---|
| SHARE_RAM | `0x20000000 ~ 0x20001FFF` | 8 KB | 分区表（内核与 App 的数据交换面） |
| KERNEL_RAM | `0x20002000 ~ 0x2000FFFF` | 56 KB | 内核数据 / 内核任务栈 |
| SLOT_RAM | `0x20010000 ~ 0x2001FFFF` | 64 KB | 镜像 RAM 池 |

镜像的 `.data/.bss/栈` 从 `SLOT_RAM` 按**伙伴分配**切块，块必须是 2 的幂且在
`[1 KB, 32 KB]` 之间（MPU region 的硬约束）。

**开发槽位走的是静态窗口**：每个槽位 16 KB（`SVCRT_DEV_RAM_WINDOW`），
4 个槽位正好占满 `SLOT_RAM`。窗口基址公式：

```
窗口基址 = SLOT_RAM_BASE + (起始单元号 % SVCRT_DEV_SLOT_MAX) × SVCRT_DEV_RAM_WINDOW
```

这条公式在 `config/svcrt_partition.h` 的 `SVCRT_DEV_SLOT_RAM_BASE(unit)` 与
`tools/gen_scatter.py` 的 `dev_ram_base(unit)` 各实现一次，**两边必须一致**，
否则 App 链接时以为栈在 A、内核把栈放在 B，表现为"能下载、能启动、跑一会儿就崩"。

栈从窗口**顶部**切出：App 4 KB（`APP_TASK_STACK_SIZE`），
驱动 1 KB（`DRIVER_TASK_STACK_SIZE`）。窗口里扣除栈后的部分才是 RW/ZI 上限——
**RW 一旦要撞栈，链接阶段就报错**，不会拖到运行期。

App/驱动均不生成 `ARM_LIB_HEAP`；要动态内存请用 `svcrt_posix_malloc`（见
《POSIX与Windows兼容层说明.md》）。

### 1.4 策略开关

**编译期**（`config/svcrt_partition.h`）：

| 宏 | 默认 | 作用 | 什么时候改 |
|---|---|---|---|
| `APP_ALLOW_RAW_IMAGE` | `config/` 里 0；两块板在 `svcrt_board_config.h` 显式置 1 | 允许池内的**裸镜像**（开发期）。为 0 则只认带头的 `.svcapp` | 发布固件时删掉板级覆盖或置 0 |
| `APP_AUTO_START` | 1 | 扫描到有效 App 后自动启动 | 调试时置 0，免得内核抢在你下断点前启动 |
| `DRIVER_AUTO_START` | 1 | 扫描到有效驱动后自动启动 | 同上 |
| `APP_CRASH_RESTART_MAX` | 3 | 连续故障重启上限，达到即禁用；0 = 不限 | 按可靠性要求调整 |
| `SHELL_ENABLE` | 1 | 内核 shell 控制台任务（占串口读权） | 不用控制台时置 0 |
| `INSTALLER_ENABLE` | 1 | 启用内核内安装逻辑 | 不用串口安装时置 0 |
| `INSTALLER_AUTO_START` | 1 | 安装完成后立即启动该 App | 需要"先装好、稍后手动启动"时置 0 |

改完任何一个开关**只需重新编译内核**（`.sct` 会在编译前自动重新生成）。

**运行期**（设备端 CONFIG 扇区，改它不用重新编译）：

| 项 | 作用 |
|---|---|
| `mode` | 0 = 自动选址，1 = 固定槽位（整机二选一） |
| `flags` | bit0 = `RAW_ALLOW`：这台设备走不走裸镜像旁路 |
| `boot_delay_ms` | 自启前先忙等这么久（给调试器留挂接窗口），上限 60000 |
| `log_level` / `fault_restart_max` | 内核日志级别 / 崩溃重启上限 |
| 槽位表 | fixed 模式下的落点、类型、RAM 窗口、自启标志 |

写入方法与字段清单见《配置区与安装策略.md》§4/§6。

---

## 2. 路径 A：开发调试（日常用这条）

### 2.1 一次性准备

App / 驱动 / 内核工程都已经配好了。检查三项（工程 → Options for Target）：

1. **Target → 不使用 Target Dialog 内存布局**（`<umfTarg>0`，让 scatter 文件生效）
2. **Linker → Scatter File** 指向对应的 `.sct`：
   - 内核 → `build/kernel.sct`
   - `APP_DEMO` → `build/app_demo.sct`；`BLED_APP` → `build/bled_app.sct`
   - `DRV_DEMO` → `build/drv_demo.sct`；`BLED_DRV` → `build/bled_drv.sct`
3. **User → Before Build/Rebuild** 留空（`.sct` 已经生成好放在 `build/` 里）

需要重新生成 `.sct` 时（例如改了槽位表或 RAM 窗口）：

```bash
# 内核
python tools/gen_scatter.py --target kernel --output build/kernel.sct
# 裸镜像（--raw --dev-slot 指定开发槽位号；App 与驱动用同一个 target=image）
python tools/gen_scatter.py --target image --raw --dev-slot 1 --type app    --output build/app_demo.sct
python tools/gen_scatter.py --target image --raw --dev-slot 0 --type driver --output build/drv_demo.sct
python tools/gen_scatter.py --target image --raw --dev-slot 3 --type app    --output build/bled_app.sct
python tools/gen_scatter.py --target image --raw --dev-slot 2 --type driver --output build/bled_drv.sct

python tools/gen_scatter.py --dump      # 打印当前布局，确认地址再生成
```

> `.sct` 是**构建产物，不要手工编辑**：下次生成会覆盖它，而且手工改过的地址
> 会和内核的推导结果悄悄分叉。

### 2.2 调试 App（步骤）

1. 确认开发期设置：
   - `APP_ALLOW_RAW_IMAGE = 1`（两块板已在 `svcrt_board_config.h` 置 1）
   - `APP_AUTO_START = 1`
   - `boot_delay_ms` 可以给一点（比如 500 ms），给调试器留出挂接窗口
2. 编译内核 → 烧录到 `0x08000000`
3. 打开 App 工程（如 `APP_DEMO`）→ 确认 Scatter File 是 `build/app_demo.sct`
4. **Keil 直接 Download**（按 `.sct` 的加载区域写入 `0x08060000`）
5. 复位运行
6. 内核扫描池 → 认定为裸镜像 → 入口取 `0x08060000 | 1` → 绑定 RAM 窗口
   `0x20014000` → 注册任务并启动
7. 在 App 代码里下断点 → 命中

### 2.3 调试驱动

步骤同上，只是：

- 工程换成 `DRV_DEMO` / `BLED_DRV`，`.sct` 换成 `build/drv_demo.sct` / `build/bled_drv.sct`
- 下载地址分别是 `0x08040000` / `0x08080000`
- 内核先启动驱动（优先级 9，高于 App 的 10），再启动 App

### 2.4 只重下 App、不重烧内核

**这是路径 A 的核心价值**：内核不用动。

只要 App 的**链接基址没变**（即槽位表与 RAM 窗口没改、`.sct` 没换），
重新编译 + Download App 即可，复位后内核重新扫描并拉起新版本。

### 2.5 为什么 App 必须在"有内核"的环境里跑

App 工程里**没有 startup 文件、没有向量表**，它本身不是从头复位运行的固件：

- 它的入口 `APPSTART` 是被内核当作**任务函数**跳进来的；
- 它调用 SVC 请求内核服务，而 SVC 处理程序在**内核的向量表**（`0x08000000`）里。

App 与内核是"共生"的：调试 App 时板子上必须有运行中的内核。

### 2.6 调试期建议

| 建议 | 原因 |
|---|---|
| `APP_AUTO_START = 0` | 内核不会在你下断点前就启动槽位 |
| `boot_delay_ms` 给 300~1000 | 内核起来后先停一会儿，方便挂接调试器 |
| 保留板级的 `APP_ALLOW_RAW_IMAGE = 1` | 否则裸镜像不被认定，App 不会启动 |
| 一次只开一个 Keil 实例 | 多个实例共用 UVSOCK 端口时会互相抢 |

---

## 3. 路径 B：串口安装

> **开启内核 Shell 后，串口读权归控制台。** 默认 `SHELL_ENABLE = 1`，
> 先在控制台敲 `install` 打开一次性接收窗口，再发送镜像——
> 避免两个读者把同一个串口 FIFO 的字节流随机分掉。
> 完整命令与示例见《内核Shell控制台使用说明.md》。

### 3.1 协议

**没有任何额外帧封装**——把 `.svcapp` 文件原样连续发给串口即可：

| 项目 | 说明 |
|---|---|
| 设备 | `INSTALLER_DEV_NAME`（默认 COM1） |
| 波特率 | `INSTALLER_DEV_ARG`（默认 115200） |
| 数据格式 | 8N1，无流控 |
| 同步方式 | 内核在字节流中搜索镜像头魔数 `SVCA`，**逐字节重新同步**，杂散字节会被跳过 |
| 结束 | 不需要结束标记；内核按镜像头里的 `image_size` 读满即止 |
| 分流 | 内核看**镜像头里的 `type`** 自行决定去向 |

### 3.2 打包

```bash
# 由 Keil 工程直接打包（推荐：自动编译四遍做差分重定位，并解析入口符号）
python tools/pack_app.py --project example/stm32f427/app_sdk/APP_DEMO/MDK-ARM/app_demo.uvprojx \
    --type app --version 1.0.0 --name "LED 闪烁示例" \
    --out build/APP_DEMO/APP_DEMO.svcapp

# 由已有的 .axf 打包（离线模式）
python tools/pack_app.py --axf build/APP_DEMO/APP_DEMO.axf \
    --type app --version 1.0.0 --out build/APP_DEMO/APP_DEMO.svcapp

python tools/pack_app.py --info build/APP_DEMO/APP_DEMO.svcapp      # 看镜像头
python tools/pack_app.py --verify build/APP_DEMO/APP_DEMO.svcapp    # 校验完整性
```

镜像头的 `hw_compat_id` 必须与本机一致（F427 = `0x42700005`，F401 = `0x40100005`），
否则内核拒装——这是防止把 F427 的镜像装到 F401 上的护栏。

入口符号约定：

| 符号 | 说明 |
|---|---|
| `APPSTART` / `DRVSTART`（默认） | SDK 启动入口，先做 C 运行库初始化（`.data` 拷贝、`.bss` 清零）再进 `AppMain`/`DrvMain`。**镜像含初始化全局变量时必须用它** |
| `AppMain` / `DrvMain` | 直接进业务函数，跳过运行时初始化 |

### 3.3 发送

1. 内核烧好并复位，串口控制台出现 `ark>` 提示符
2. 敲 `install` 打开接收窗口
3. 把 `.svcapp` 以**二进制方式**发给串口（不要用文本模式，会被改行尾）
   - `copy /b build\APP_DEMO\APP_DEMO.svcapp COM3`
4. 发送完成后按 `INSTALLER_AUTO_START` 决定是否立即启动
5. `app list` 查看结果

### 3.4 掉电/中断会怎样

安装期间槽位状态是 **`INSTALLING`**，只有**全部写完且 CRC 回读复核通过**才置 `LOADED`。

> 写一半掉电 → 状态随 RAM 一起消失 → 重新上电后内核按 CRC 校验那个半成品 →
> CRC 不过 → 判为 `INVALID` → **不会被启动**。

所以中断安装是安全的：不会留下一个"看起来可用"的坏槽位。重新发一遍即可。

---

## 4. 路径 C：打包后用烧录器写入

产线或首件需要一次性把内核 + 镜像都烧好时：

| 映像 | 烧录地址 | 来源 |
|---|---|---|
| 内核 | `0x08000000` | 内核工程构建产物 |
| 配置记录（可选） | `0x08020000` | `tools/svcrt_layout.py build --bin` 的输出 |
| 驱动 `.svcapp` | 池内任意空闲槽位（auto）或配置指定的槽位（fixed） | `pack_app.py` 输出 |
| App `.svcapp` | 同上 | `pack_app.py` 输出 |
| 裸镜像 | 开发槽位表指定的地址（见 §1.2） | App 工程的 `.axf`/`.bin` |

- `.svcapp` 是"256 字节头 + 负载"的整体，**从头开始写到槽位基址即可**，不需要拆开。
- 注意：**同一个槽位要么放 `.svcapp`，要么放裸镜像，不能混**——
  带头的镜像入口是 `槽位基址 + 256 + entry_offset`，裸镜像入口是 `槽位基址`，
  两种布局相差 256 字节，混用会直接跑飞。

---

## 5. 上电启动时序（内核视角）

```
main()
 └─ svcrt_kernel_init()
     ├─ 模块初始化（日志 / 互斥 / 分区表 / 布局）
     ├─ svcrt_layout_init()        读 CONFIG 扇区；无效则回退编译期默认
     ├─ boot_delay_ms 非 0 ?       → 忙等（给调试器留挂接窗口）
     ├─ 扫描池 → 认定带头镜像
     ├─ RAW_ALLOW ?                → 认领开发槽位并绑定 RAM 窗口
     ├─ 回收（按 reclaim_mode）     → 压实/擦除空扇区
     ├─ 启动驱动（DRIVER_AUTO_START）
     ├─ 启动 App（APP_AUTO_START）
     └─ shell 控制台
 ...
 调度器开始运行
```

启动顺序上**驱动先于 App**，因为驱动力图先就绪供 App 使用。

---

## 6. 镜像认定规则（内核怎么判断池里是什么）

扫描从池基址开始，按 1 KB 粒度线性推进，每个位置依次判断：

| 首字 | `APP_ALLOW_RAW_IMAGE` | 判定 | 入口 |
|---|---|---|---|
| `SVCA` | — | 带镜像头 → 校验兼容签名与 CRC | `基址 + 256 + entry_offset` |
| 非 `SVCA`、非擦除态 | 1 且命中开发槽位表 | **开发期裸镜像** | `基址`（ARM 置 Thumb 位） |
| 非 `SVCA`、非擦除态 | 0 或不命中槽位表 | 无效 | — |
| `0xFFFFFFFF` / `0x00000000` | — | 空 | — |

- 有内容但认定失败 → 槽位置 `INVALID`（**不会被误启动**），与 `EMPTY` 区分开
- 裸镜像登记的槽位状态是 `RAW`（值 5），**不参与压实与回收**，也不能被卸载

**认领顺序不能反**：先扫描带头镜像、后认领开发槽位。因为安装器也从池底往后分配，
一个"装进去的带头镜像"完全可能正好落在开发槽位里；先认领会把带电位的那 256 字节
镜像头整段当成裸镜像，槽位表里就再也见不到它，上电也就不会自启。

---

## 7. 故障处理：崩溃重启与禁用

### 7.1 策略

```
App/驱动 任务 HardFault
   → 该槽位崩溃计数 +1
      ├─ 未达上限（APP_CRASH_RESTART_MAX，默认 3）→ 重建栈帧、重新调度
      └─ 达到上限                                  → 禁用：置 INVALID、脱离调度
```

- 计数**按槽位累计**，重启不清零；**重新安装镜像时清零**
- 上限也可来自设备端配置记录的 `fault_restart_max`
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

串口控制台：`fault` 看故障记录，`app list` 看槽位状态与崩溃计数。

故障类型：

| 值 | 含义 |
|---|---|
| 1 | HardFault |
| 2 | 任务栈溢出 |
| 3 | 任务被终止 |
| 4 | 任务故障恢复（重启了一次） |
| 5 | 调度器锁定期间调用阻塞接口（编程错误） |
| 6 | App 连续故障达上限被禁用 |

### 7.3 被禁用后如何恢复

重新安装镜像即可（路径 B 重发，或路径 A/C 重新写入）——安装成功会清零计数。

槽位复用规则：空槽 / 被禁用（INVALID）/ 已停止（LOADED）都可直接覆盖安装；
只有「运行中（RUNNING）」不行——先 `app stop 0`，否则安装被拒（`ERR_BUSY`）。
单槽闭环更新 App 的完整序列：`stop 0` → 安装 → `start 0`。

### 7.4 已知边界

计数保存在**共享 RAM**，掉电即清零。所以：

- 能挡住"App 反复崩溃 → 重启 → 再崩溃"的死循环
- **挡不住"崩溃导致整机复位"的启动环**（上电 → 自启 → 崩溃 → 复位 → …）

要堵住启动环需把计数持久化。芯片侧最划算的是 **RTC 备份寄存器**
（STM32F427 有 20×4 字节，跨复位/掉电保持且无 Flash 写损耗），属板级待办。

---

## 8. 发布固件检查清单

- [ ] `APP_ALLOW_RAW_IMAGE = 0`（删掉 `svcrt_board_config.h` 里的板级覆盖）
- [ ] 设备端配置记录：`mode` 按交付形态（现场升级用 auto）、`flags` 清 0
- [ ] `INSTALLER_ENABLE` / `SHELL_ENABLE` 按产品形态决定
- [ ] `APP_AUTO_START` / `DRIVER_AUTO_START` / `APP_CRASH_RESTART_MAX` 按需求确认
- [ ] `SVCRT_HW_COMPAT_ID` 与本批次硬件一致（改硬件必须改它，否则旧镜像会被拒装）
- [ ] 所有镜像用 `pack_app.py --verify` 过一遍
- [ ] 预留：`.svcapp` 的 `signature[64]` 目前为占位，**签名校验尚未实现**（见 §10）

---

## 9. 排错表

| 症状 | 可能原因 | 处理 |
|---|---|---|
| App 下载了但不运行 | `APP_ALLOW_RAW_IMAGE = 0`，或运行期 `RAW_ALLOW` 位没开 | 开发期两者都打开 |
| App 不运行，`app list` 里状态是 3 | 内容认定失败（格式不对或 CRC 不过） | 重新打包/下载；`pack_app.py --verify` 检查 |
| `app list` 里 `ram` 列是 `0x00000000` | 裸镜像的 RAM 窗口没绑上（内核 bug 或槽位表没命中） | 核对开发槽位表的 `UNIT`/`TYPE`，与 `.sct` 的地址是否吻合 |
| 池里空空（`app list` 无输出） | 下载地址不对，没写到槽位 | 核对 `.sct` 与烧录地址（见 §1.2） |
| 链接报错说 RW 区放不下 | `.data+.bss` 超出窗口减去栈的部分 | 精简静态变量，或调大 `SVCRT_DEV_RAM_WINDOW` 并重新生成 `.sct` |
| 能下载、能启动、跑一会儿就崩 | `.sct` 的 RAM 窗口与内核推导的窗口不一致 | 两边用同一个 `tools/gen_scatter.py` 生成，别手工改 `.sct` |
| 内核编译前报找不到 `gen_scatter.py` | Keil 的 Before Build 里 `python` 不在 PATH | 把 Python 加入 PATH，或预先手工生成 `.sct` |
| 串口安装没反应 | 没先敲 `install`；串口被调试会话占用；文本模式发送 | 敲 `install`、释放串口、改二进制发送 |
| 安装后立即回到 INVALID | 传输丢字节导致 CRC 不过 | 降低波特率或加流控，重新发送 |
| `cfg show` 显示 `record : none` | 配置区没有有效记录（正常回退） | 要固化布局就用 `tools/svcrt_cfg.py write` 或 `tools/svcrt_host_gui.py` 在线写入 |
| `cfg load` 报 `bad magic`，但记录在主机侧校验没错 | 流被顶偏：命令行用了 CRLF，残留的 LF 占了记录的第 1 字节 | 用 `tools/svcrt_cfg.py`（CR 结尾），不要自己手写发送脚本；详见《配置区与安装策略.md》§6.3 |
| `cfg load` 报 `bad crc` / `bad hw` / 其它原因 | 记录真的有问题（内容被改过、芯片签名不符） | 原因行由设备给出，照它改记录；坏记录不会影响已有配置 |
| `cfg show` 报 `fixed`，但 `pool` 不列固定槽、装进去落点不对 | 共享分区表的布局字段被覆盖（历史缺陷 `7459e1d` 已修） | 先重启再复查；若复现，读共享分区表 `0x20000000+52` 的 `layout_mode` 与 `cfg show` 对比 |
| 写配置后立刻安装，落点与预期不符 | 布局策略要**重启**才完全生效（池扫描与启动在启动早期已做完） | 按设备提示重启再安装 |
| 改了 `CHIP_FLASH_SIZE` 后地址全变 | 这是设计行为（地址只定义一次） | 重新编译内核与各映像，无需改其他文件 |
| 串口输出出现半行/错字 | 已修：console 有行级锁与整串一次 SVC（提交 `01b9728`） | 若仍出现，先看 `log` 命令的 `lock_giveup` / `tx_drop` 计数，别直接归因于并发 |

---

## 10. 相关工具与待办

### 工具

| 工具 | 用途 |
|---|---|
| `tools/gen_scatter.py` | 由配置头生成 `.sct`；`--dump` 看布局、`--check` 校验重叠越界 |
| `tools/svcrt_layout.py` | 设备端配置记录：模板 / 校验（`check`）/ 落盘 / 每槽 `.sct` |
| `tools/svcrt_cfg.py` | 经串口在线读写下发配置记录：`show` / `write` / `clear` |
| `tools/svcrt_host_gui.py` | 图形界面：连接 / 控制台 / 安装 / 布局配置（所有上位机功能的统一入口） |
| `tools/pack_app.py` | 打包 `.svcapp`；`--info` 看镜像头、`--verify` 校验 |
| `tools/send_image.py` | 串口安装镜像（等价于 GUI 的「安装」页签） |
| `tools/gen_api_doc.py` | 生成 API 文档（见 `docs/README.md`） |
| `skills/svcrtos/SKILL.md` | 给 AI 代理的调用入口（见 §11） |

```bash
python tools/gen_scatter.py --dump
python tools/gen_scatter.py --check
python tools/gen_scatter.py --target all --output build
```

### 尚未支持（按优先级）

1. ~~console 行级互斥~~——**已完成**（提交 `01b9728`）：console 加行级锁，`app_puts`
   改为整串一次 SVC，并新增 `lock_giveup` / `tx_drop` 计数（`log` 命令可见）。
   真机长跑（数百 KB 输出）`tx_drop = 0`。
2. **镜像签名校验**——`signature[64]` 是占位字段，还没有信任链。
3. **崩溃计数持久化**——挡住"崩溃导致整机复位"的启动环（建议 RTC 备份寄存器）。
4. **槽位元数据持久化**——版本号、升级/回滚目前无法跨掉电保留。
5. **A-B 回滚**——池已支持多槽位，但还没有升级/回滚策略。
6. **镜像侧多槽位**——内核侧已完整支持（扫描、启动、停止、状态、故障计数、
   按槽切栈、MPU 按槽隔离），但 `tools/pack_app.py` 的目标槽位仍按标称基址推导，
   跨槽安装的镜像侧流程尚未打通。
7. **SVC 边界零信任**——用户传入的指针已做"不得指向内核 RAM/Flash"的拒绝，
   但句柄仍是裸索引。

---

## 11. 交给 AI 代理调试

这块板子上跑的是一套**可被 AI 直接驱动的工程**：源码、配置源头、上位机工具与调试
通道都是确定性的，不需要人先做一遍再教给 AI。

### 11.1 项目内的 skill

`skills/svcrtos/SKILL.md` 是给 AI 代理用的调用说明，包含四件事：

| 内容 | 作用 |
|---|---|
| 工程事实 | 地址只在 `config/svcrt_partition.h` 定义、`.sct` 是构建产物、编码与行尾约定 |
| 命令行闭环 | 编译 / 烧录 / 安装 / 起停 / 查状态的准确命令与退出码含义 |
| 串口闭环 | shell 命令表、安装协议、配置写入的三个握手约定 |
| 调试通道 | 经 `mdk_agent_mcp`（[gitee](https://gitee.com/xw19981010/mdk_agent_mcp.git)）用 MCP 工具读写内存、变量、寄存器，替代手工下断点 |

配套的图形界面 `tools/svcrt_host_gui.py` 适用于人工操作，AI 则用
`tools/` 下的命令行脚本（同一套协议，输出可解析）。

### 11.2 为什么 AI 能直接上手

- **一个地址源头**：改了 `CHIP_FLASH_SIZE`，分区、池、`.sct`、每个槽位全部自动重算，
  AI 不需要在多个文件之间同步数字；
- **工具不猜原因**：设备拒绝配置记录时会在串口打印真实原因行，上位机原样转发
  （见 §6.3 / §9），AI 拿到的是设备的话，不是工具的推测；
- **状态可直读**：布局的权威副本在共享分区表，`layout_mode` 在 `0x20000000 + 52`
  （F427，u32 小端），可用调试器直接取，不必依赖任何命令的输出格式；
- **失败方式的约定**：编译 0 Error / 1 Warning（继承的 `svcrt_context.S A1581W`）、
  `UV4 -r` 返回 1 只表示有 warning 但**会让 `&&` 串联的 `-f` 不执行**。

### 11.3 交接清单

改完任何与布局相关的东西，按这个顺序验完再交：

1. 两块板内核各编译一次（`-r`，F427 + F401）；
2. F427 烧录（`-f`，独立调用，不要和 `-r` 串在一条 `&&` 里）；
3. `app list` 确认只有一个实例在跑（两个 APP_DEMO 同时自启会让输出逐字节交错，
   并让后注册者报 `shell.register ... FAIL (-2)`）；
4. 配置区改动走 `svcrt_cfg.py write` → `cfg show` 读回 → **重启** → 再 `cfg show`
   （策略类字段重启前不生效）；
5. 安装走 `send_image.py` 或 GUI，看设备侧 `INSTALL` 日志而非主机侧的"发送完成"。

### 11.4 真机验证记录（F427，2026-09-19）

用 `tools/svcrt_host_gui.py` 自身的代码路径（界面隐藏、`messagebox` 打桩）在板上逐项跑，
结论按「仅编译通过 / 上板验证过 / 未验证」分档：

| 验证项 | 结论 | 证据 |
|---|---|---|
| 固定槽模式配置写入并重启读回 | 上板验证过 | `cfg show` 读到 `layout : fixed`，`pool` 列出 3 个固定槽（`0x08040000` / `0x08080000` / `0x080A0000`，各 128K） |
| 按固定槽位安装的落点 | 上板验证过 | `install 2` → 设备回 `install: fixed slot 2 -> 0x080A0000`；`app` 显示新实例 `base 0x080A0000`、`ram 0x20014000`、`size 131072` |
| 不兼容镜像被拒时当场返回 | 上板验证过 | 设备回 `[E][INSTALL:150] rejected: err -3` 后 **0.7 s** 主机即结束（修复前要等满 5 s 的 ACK 超时） |
| 安装失败后设备回到干净的命令提示 | 未达成（已知副作用） | 见下 |
| 卸载与配置擦除后的还原流程 | 未验证 | 还原脚本把**配置槽序号**传给了 `app uninstall`，被 `slot_type` 校验挡下（见下） |

三点使用上的约定：

- **固定槽模式下同时存在两套编号，命令收的是分区表槽号。**
  配置里的槽序号（`svcrt_layout.py` / GUI 布局页看到的 0、1、2）只是「第几个固定槽」；
  真正落盘后，内核对每个镜像发一个**分区表槽号**，`install: ok, slot N`、`app` / `drv` 列表的
  `id` 列、以及 `app uninstall <n>` / `app start <n>` 的参数，全部用的是后者。
  上例中镜像装在配置槽 2，分区表槽号却是 1：`pool` 的 `fixed slots` 段显示配置槽 2 为
  `RUNNING`，而 `app` 列表显示 `id 1`。**要确认落点看 `base`，不要拿两套序号互相对照。**
- **帧在「头部」阶段就被拒时，控制台可能残留一行 `Command not found: ...`。**
  设备为了能校验重定位表，要求头和表**背靠背**发送；而兼容性判定只看了 256 B 头就
  NAK 早退，此时那张表已在途中，落进 shell 就被当成命令。这是半双工窗口的固有结果，
  不是安装失败的新原因，也不影响后续操作。
