# SVCrtOS Loader 工程化落地说明（灵活分区版）

> 落地范围：按《SVCRTOS Loader 工程化落地指导（灵活分区版）》实现分区配置、分散加载自动生成、
> 共享分区表、镜像格式、Flash 驱动、分区内加载器、SVC 0x18 用户接口与 MDK 工程挂接。
> 采用**单区间最小闭环**（`APP_MAX_COUNT = 1`）与 **Loader 内核内服务**（不新建独立固件工程）。

## 1. 新增文件

| 文件 | 作用 |
|---|---|
| `config/svcrt_partition.h` | 全工程唯一地址源头：芯片 4 参数 + 分区策略 + 全部派生地址 |
| `tools/gen_scatter.py` | 由配置头生成各工程 `.sct`，支持 `--target/--output/--check/--dump/--header` |
| `tools/pack_app.py` | 把 Keil 的 `.axf`/`.bin` 打包为 `.svcapp`（256 字节头 + CRC32），支持 `--info/--verify` |
| `kernelsrc/include/svcrt_share.h` | 共享内存分区表结构（运行期接口，运行于 Loader/App 亦可读） |
| `kernelsrc/include/svcrt_app_image.h` | App 镜像 256 字节头 + CRC32 声明 |
| `kernelsrc/include/svcrt_ptable.h` / `src/svcrt_ptable.c` | 分区表运行期初始化与槽位状态维护 |
| `kernelsrc/include/svcrt_loader.h` / `src/svcrt_loader.c` | 镜像校验、擦写、流式加载、任务启动/停止 |
| `board/stm32f427/drvflash.h` / `drvflash.c` | 片内 Flash 擦除/编程（HAL 实现，支持 1MB/2MB 扇区划分） |
| `build/*.sct` | 由脚本产出的分散加载文件（构建产物，禁止手工编辑） |

## 2. 当前 Flash / RAM 布局

Flash（1MB，`CHIP_FLASH_SIZE`）：

| 分区 | 基址 | 大小 |
|---|---|---|
| KERNEL | `0x08000000` | 256 KB |
| DRIVER_POOL | `0x08040000` | 256 KB |
| APP_SLOT0 | `0x08080000` | 512 KB |

RAM（128KB，`CHIP_RAM_SIZE`）：

| 分区 | 基址 | 大小 |
|---|---|---|
| SHARE（分区表） | `0x20000000` | 8 KB |
| KERNEL | `0x20002000` | 88 KB |
| DRIVER | `0x20018000` | 16 KB |
| APP | `0x2001C000` | 16 KB |

**修改芯片容量只需改 `CHIP_FLASH_SIZE` / `CHIP_RAM_SIZE`**，其余地址与 `.sct` 均由脚本推导。
验收测试：把 `CHIP_FLASH_SIZE` 改为 2MB 后，`APP_SLOT0` 自动扩展到 1536KB，其他文件无需改动。

## 3. 运行期接口

### 3.1 SVC 0x18（APP_MGR）

| 子命令 | 功能 | 参数（`args[]`） |
|---|---|---|
| 2 | 从设备加载 App 镜像 | `args[1]=dev`，`args[2]=image_len` |
| 3 | 启动 App | `args[1]=slot` |
| 4 | 停止 App | `args[1]=slot` |
| 5 | 查询槽位状态 | `args[1]=slot` |
| 6 | 从设备安装驱动镜像到驱动区 | `args[1]=dev`，`args[2]=image_len` |

子命令 2 / 6 的镜像类型是**强校验**的：内核会把镜像头里的 `type` 与目标区比对，
App 镜像进不了驱动区、驱动镜像也进不了 App 槽位（`svcrt_loader_identify()` 的
`expect_type` 参数同理，上电扫描时一并生效），避免“烧错分区”这类事故。

> 原先的「子命令 1：返回分区表地址」**已移除**。把共享内存地址交给用户态，
> 意味着 App 能篡改自己的 `slot_state` 伪造“已加载”，也能改写其它槽位记录。
> 分区表现在只由内核/安装器通过 `svcrt_ptable_get()` 访问，用户态只能用
> `svcrt_app_status()` 这类查询接口。

### 3.2 用户 API

`svcrt_app_load(dev, image_len)`、`svcrt_app_start(slot)`、
`svcrt_app_stop(slot)`、`svcrt_app_status(slot)`（`svcrt.h` 声明，`oslib.c` / `svcrt_oslib.c` 实现）。

（`svcrt_partition_table_addr()` 已收回，不再对内提供。）

返回错误码：`0..`=槽位号；负数见 `svcrt_loader.h`（-1 参数、-2 魔数、-3 兼容签名、-4 长度、
-5 CRC、-6 Flash、-7 无空闲槽位、-8 任务注册失败、-9 状态不允许）。

## 4. 加载流程（`svcrt_loader_load_*`）

1. 校验镜像头：`magic == 'SVCA'`、`hw_compat_id == SVCRT_HW_COMPAT_ID`、`image_size != 0`；
2. 整镜像 CRC32 与头中 `crc32` 比对（`crc32` 字段按 0 参与，算法 IEEE 802.3 反射多项式，与打包工具一致）；
3. 查找空闲槽位，擦除槽位所在 Flash 扇区；
4. 写入镜像（256 字节头 + 负载）；设备路径按 512 字节分块流式读写，不要求整镜像驻留 RAM；
5. 从 Flash 回读复算 CRC 确认；
6. 槽位状态置 `LOADED`，记录入口 `slot_base + entry_offset`（ARM 自动置 Thumb 位）；
7. `svcrt_loader_start()` 用 `APP_TASK_PRIORITY` / `APP_TASK_STACK_SIZE` 注册任务并触发调度。

驱动路径（`svcrt_loader_load_driver*`）复用同一条流水线：

- 镜像头 / CRC / 流式写入 / 回读复算 CRC 的逻辑抽成了公共函数
  （`svcrt_loader_stream_payload()` + `svcrt_loader_load_*_dev()`），App 与驱动**共用一套代码**，
  避免两边各自演化出“只修了一边”的 bug；
- 差异只在目标区：驱动区是**单入口**，不做槽位分配，按 `DRIVER_POOL_SIZE` 判容量，
  写入前整体擦除目标区间，成功后置 `driver_state = LOADED` 并记 `driver_entry`；
- 上电扫描同理：`svcrt_loader_scan_driver()` 用 `expect_type = SVCRT_APP_TYPE_DRIVER` 调用
  `svcrt_loader_identify()`，驱动区里放的是 App 镜像会被直接判为 `INVALID`。

### 4.1 安装任务如何分流

内核安装任务（`svcrt_installer.c`）收到镜像、魔数同步并读出 256 字节头后，
**只看镜像头里的 `type` 决定去向**，不需要用户事先指定：

| `hdr.type` | 目标 | 装载函数 | 成功后是否自启 |
|---|---|---|---|
| `SVCRT_APP_TYPE_DRIVER` | 驱动池（单入口，覆盖） | `svcrt_loader_load_driver_dev()` | 看 `DRIVER_AUTO_START` |
| 其它（App） | 空闲 App 槽位 | `svcrt_loader_load_dev_hdr()` | 看 `INSTALLER_AUTO_START` |

这一条把“装驱动”变成了与“装 App”完全同构的动作：**装什么由包本身说了算**，
这正是“像手机应用商店一样装应用与驱动”在接口层面的最小必要形态。

## 5. MDK 工程挂接

5 个工程统一改为：

- `<umfTarg>0</umfTarg>`（不使用 Target Dialog 内存布局）
- `<ScatterFile>` 指向 `build\{kernel,driver,app}.sct`
- `Before Build/Rebuild` 自动执行 `python tools/gen_scatter.py --target <t> --output build\<t>.sct`
- 内核工程额外：IncludePath 追加 `config`；纳入 `svcrt_ptable.c`、`svcrt_loader.c`、`drvflash.c`

映射关系：

| 工程 | target | 分散加载文件 |
|---|---|---|
| SVCRTOS_TEST | kernel | `build/kernel.sct` |
| APP_DEMO / BLED_APP | app | `build/app.sct` |
| BLED_DRV / DRV_DEMO | driver | `build/driver.sct` |

## 5.1 栈模型：从 App 自己的 RAM 区切出（与 AnOs 一致）

栈**不是**内核另外分配的缓冲区，而是从 App 自己的 RAM 区顶部切出来的：

```
stack_top    = APP_RAM_BASE + APP_RAM_SIZE
stack_bottom = stack_top - APP_TASK_STACK_SIZE
```

- 内核侧：`svcrt_loader_start()` 按上式推导后交给 `svcrt_task_register()`，并在栈底写入哨兵字；
- App 侧：`gen_scatter.py` 生成 `ARM_LIB_STACK 0x20020000 EMPTY -0x1000`，
  即 `__main` 设置 SP 后与内核推导值**完全一致**——不存在“双份栈”，
  也因此保住了 **`.data` 拷贝 / `.bss` 清零**（不必像 AnOs 那样跳 `main` 而放弃运行时初始化）；
- **链接期强制**：`RW_APP` 的上限已扣除栈区（`APP_RAM_SIZE - APP_TASK_STACK_SIZE`），
  App 的 RW/ZI 一旦要撞栈，**链接阶段就失败**，不依赖开发者自觉（AnOs 是把整块 RAM 给 RW/ZI，靠约定）；
- 驱动区同理，使用 `DRIVER_TASK_STACK_SIZE`；App/驱动均不再生成 `ARM_LIB_HEAP`（堆会与受保护的 RW 区重叠）。

配套字段：注册后内核会把 TCB 的 `ram_start/ram_size/rom_start/rom_size` 修正为**整个 App 分区**，
而不是只有栈——这是为后续 MPU 隔离预留的元数据。

## 5.2 安装任务（方案A：设备自己安装）

常驻内核任务把“烧录器刷固件”变成“设备自己安装”，配置项集中在
`config/svcrt_partition.h` 的「安装器策略」一节（`INSTALLER_*`）。

接收协议：主机把 `.svcapp`（256 字节头 + 负载）**原样连续发到串口**即可，无需额外帧封装；
安装任务在字节流中搜索镜像头魔数 `SVCA` 做**逐字节重新同步**，因此混杂的杂散字节会被自动跳过。

流程：打开设备 → 同步并读出 256 字节头 → `svcrt_loader_load_dev_hdr()`
（长度/兼容签名校验 → 擦除 → 分块流式写入 → 回读复算 CRC）→ 按 `INSTALLER_AUTO_START` 启动。

掉电安全：写入期间槽位为 **`INSTALLING`**，只有全部写完且 CRC 复核通过才置 `LOADED`；
中断安装留下的半成品会被 `svcrt_loader_scan()` 依 CRC 判为 `INVALID`，不会被启动。

## 5.3 故障围栏：崩溃重启上限（3 次后禁用）

App 任务发生 HardFault 时，原有逻辑是**无条件重建栈帧重启**（两阶段恢复：`svcrt_task_recover()` 先脱离调度，
`svcrt_task_recover_pending()` 下次调度扫描时重建栈帧），一个持续崩溃的应用会把整机拖进“崩溃-重启”死循环。
现按策略收敛：

```
故障 → 计数 +1
  ├─ 未达 APP_CRASH_RESTART_MAX  → 重启该 App（沿用两阶段恢复）
  └─ 达到上限                    → 禁用该 App：槽位置 INVALID、任务脱离调度、不再重启
```

- 策略项：`APP_CRASH_RESTART_MAX`（默认 3，0 表示不限次自动重启），位于 `config/svcrt_partition.h`
- 计数存放在分区表的 `slot_crash_cnt[8]`，**按槽位累计**；重新安装镜像时清零
- 禁用时额外记一条 `SVCRT_FAULT_APPDISABLED(6)`，上位机可用 `svcrt_fault_record_read()` 查到原因
- 槽位置为 `INVALID` 而不是 `EMPTY`，宿生可通过 `svcrt_app_status()` 区分“被禁用的应用”与“空槽位”
- 内核任务（不属于任何槽位）返回 `-1`，沿用默认处理，不受该策略影响

**边界（重要）**：计数保存在共享 RAM，掉电即清零。因此它能挡住“App 反复崩溃重启”，
但挡不住“崩溃导致整机复位”的启动环。要彻底堵住启动环，需把计数持久化——
芯片侧最划算的做法是 **RTC 备份寄存器**（STM32F427 有 20 个 4 字节备份寄存器，
跨复位与掉电保持，且无 Flash 写损耗），后续可作为板级接口接入。

## 5.4 两条并存的路径：开发调试 vs 发布安装

**开发期「固定地址烧录 + MDK 下断点调试」是硬需求，不能被安装流程取代。**
两条路用的是**同一个链接基址**（都源于 `config/svcrt_partition.h`），因此调试态与发布态的代码布局完全一致。

| | 开发调试路径 | 发布/安装路径 |
|---|---|---|
| 产物 | 裸镜像（`.axf` / `.bin`，无镜像头） | `.svcapp`（256B 头 + CRC32） |
| 落位 | Keil 直接 Download 到分区固定地址 | 安装任务经 COM1 流式写入 |
| 入口 | 分区基址（\|1，即 `APPSTART`） | 基址 + `entry_offset` |
| 校验 | 无（开发期不逐镜像校验） | 魔数 + 硬件兼容签名 + CRC32 |
| App | APP_DEMO / BLED_APP 工程直接编译调试 | 打包后由安装任务写入 |
| 驱动 | BLED_DRV / DRV_DEMO 工程直接编译调试 | 已支持（裸镜像识别 + `svcrt_driver_load` + 安装任务分流） |

实现方式：`svcrt_loader_identify()` 对同一个分区先试「带头 .svcapp」，不成立再看是不是擦除态；
不是擦除态且 `APP_ALLOW_RAW_IMAGE = 1` 时，当作开发期裸镜像，入口取分区基址。

- **开发期**：`APP_ALLOW_RAW_IMAGE = 1`（默认）—— 直接用 Keil 下载到 `0x08080000` / `DRIVER_POOL` 即可下断点调试
- **发布固件**：把 `APP_ALLOW_RAW_IMAGE` 置 0 —— 只接受带镜像头的 `.svcapp`，裸镜像会被判为 `INVALID`
- 驱动区同样支持：`svcrt_loader_scan_driver()` / `svcrt_loader_start_driver()`，
  栈从 `DRIVER_RAM` 顶部切出，参数取 `DRIVER_TASK_*`；安装路径见 §4.1，
  驱动镜像也会被安装任务按 `type` 自动分流进驱动区

> 调试时建议同时关掉 `APP_AUTO_START` / `INSTALLER_ENABLE`，避免内核在调试会话中抢先启动槽位。

## 6. 硬编码清理

这一节记录的是**过程**，最终态是「一个地址都不抄」：

1. 第一轮先把 `main.c` 里的字面量 `0x08060000` / `0x08080000` 换成 `DRIVER_POOL_BASE | 1u` /
   `APP_SLOT0_BASE | 1u`，这一步只是把抄第二遍的地址换成抄第三遍的宏；
2. 第二轮（提交 `3296e6c`）直接删掉这两个宏和 `svcrt_register_tasks()` 里的静态注册——
   入口改由 `svcrt_loader_scan()` / `svcrt_loader_scan_driver()` 从分区表推导，
   `main.c` 现在**只剩一个 `svcrt_kernel_module_init()` 调用**。

内核初始化入口统一收敛到 `kernelsrc/src/svcrt_init.c` 的 `svcrt_kernel_module_init()`：
分区表 → 故障 → 各内核模块 → 板级设备 → 加载器扫描 → 安装任务，顺序固定在一处，
避免「内核弱 `main`」与「板级 `main.c`」两条启动路径各自演化（启动流程漂移）。

板级 `main.c` 只负责 STM32 的时钟 / 外设初始化，然后调 `svcrt_kernel_module_init()`。
Loader / App 工程**不包含** `config/svcrt_partition.h`，布局经 SVC 0x18 子命令 1 在运行期获取。

## 7. 验证记录

- AC6（armclang）语法校验：`kernelsrc/src/*.c`（含新增 `svcrt_ptable.c`、`svcrt_loader.c`）、`board/stm32f427/drvflash.c`、`main.c` —— 全部零错误零告警；
- AC5（armcc）语法校验：`kernelsrc/sdk/app_sdk/svcrt_oslib.c`（含 `__svc(0x18)`；用户态 oslib 现已收敛为唯一一份）—— 通过；
- 脚本验收：`gen_scatter.py --check/--dump` 通过；改 `CHIP_FLASH_SIZE` 为 2MB 后 `APP_SLOT0` 自动变 1536KB；
- 5 个 `.uvprojx` 经 XML 解析校验合法。

## 8. 镜像打包工具（`tools/pack_app.py`）

把 App / 驱动工程的编译产物打包成 Loader 可加载的 `.svcapp`。地址、槽位容量、
`hw_compat_id` 全部由 `config/svcrt_partition.h` 推导，不硬编码。

```bash
# 由 .axf 打包（推荐，自动从符号表解析入口偏移）
python tools/pack_app.py --axf build/APP_DEMO/APP_DEMO.axf \
    --type app --version 1.0.0 --name "LED 闪烁示例" --out build/APP_DEMO/APP_DEMO.svcapp

# 由 .bin 打包（需显式给出入口偏移）
python tools/pack_app.py --bin build/BLED_DRV/bled_drv.bin \
    --type driver --version 1.0.0 --entry-offset 0x0 --out build/BLED_DRV/BLED_DRV.svcapp

# 查看 / 校验
python tools/pack_app.py --info   build/APP_DEMO/APP_DEMO.svcapp
python tools/pack_app.py --verify build/APP_DEMO/APP_DEMO.svcapp
```

**入口符号约定**：镜像入口是「任务型函数」（内部循环调用 `svcrt_task_wait` 等）。

| 符号 | 说明 |
|---|---|
| `APPSTART` / `DRVSTART`（默认，推荐） | SDK 启动入口，先执行 C 运行库初始化（`.data` 拷贝、`.bss` 清零）再进入 `AppMain`/`DrvMain`。**镜像含初始化全局变量时必须用它** |
| `AppMain` / `DrvMain` | 直接进入业务函数，跳过运行时初始化，仅当镜像无需初始化全局变量时可用 |

工具会自动校验「ELF 镜像基址 == 分区槽位基址」，不一致直接报错并提示检查 `.sct`——
这是防止链接布局与打包布局悄悄错位的关键护栏。

## 9. 已完成的清理（本轮）

| 项 | 处理 |
|---|---|
| 任务上限 `SVCRT_TASK_MAX_NUM` | 7 → 32（工业可用的起步值）；同时新增 `SVCRT_TASK_TABLE_RAM_MAX`（8KB）编译期预算断言，扩容不再靠拍脑袋 |
| 注册失败静默 | 改为记录 `SVCRT_FAULT_NOSLOT`，任务表满能被上层看见 |
| 启动流程漂移 | 抽出 `svcrt_kernel_module_init()`，弱 `main` 与板级 `main.c` 共用同一条初始化序列 |
| `kernelsrc/app/oslib.c` | 已删除（与 `sdk/app_sdk/svcrt_oslib.c` 重复且更旧），并从内核工程摘除 |
| `kernelsrc/app/appconfig.c`、`appstart.s` | 已删除（旧静态注册架构遗留，全工程零引用） |
| `svcrt_app_config.h` + 两份 `app_config.c` | 已删除（越界地址：表里写死 `0x20010000` / `0x08020000`，且无人读取） |
| `MDK-ARM` 下手写 `.sct`（5 份） | 已删除（已被 `build/*.sct` 取代，无工程引用） |
| `SVCRT_SHARE_MEM_ADDR` | 已从 `svcrt_config.h` / `svcrt_board_config.h` 删除（旧值 `0x20028000` 越界） |
| `drvflash.c` 的 `0x08000000u` | 改用 `CHIP_FLASH_BASE` |
| 仓库卫生 | 新增 `.gitignore`；`docs/.gen_utf8/`（Doxygen 临时树）与 `*.uvguix.*` 移出索引 |

## 10. 遗留待办

1. App 镜像运行在 `APP_RAM_BASE`，但当前单区间闭环未对 App 做 MPU 隔离（`SVCRT_USE_MPU = 0`），后续多区间时需补齐；
2. 槽位状态仍只存在于共享 RAM，掉电即丢失（`svcrt_loader_scan()` 可依 CRC 重新认定，`INSTALLING` 也靠它兜住），
   但版本/回滚等运行期状态无法持久化——需要把槽位元数据持久化到 Flash；
3. **故障围栏已完成「重启上限」一档**；尚需补：关键结构 magic/CRC 自检、独立看门狗（挡住 App 关中断/死循环）；
4. **SVC 边界仍非零信任**：内核当前仍直接解引用用户传入的指针（`p = (uint32 *)SVCRT_SVC_ARG(...)`），
   且对象句柄是裸索引、没有归属校验。这是无 MPU 平台唯一能拿到的「软件保护域」，应作为下一步重点；
5. 驱动区是**单入口**（同一时刻只驻留一份驱动），多驱动共存需要先扩分区策略与驱动表；
6. 崩溃计数尚未持久化（需板级备份寄存器或 Flash 元数据），「崩溃导致整机复位」的启动环仍挡不住；
7. 已补一轮**符号级链接审计**（把内核/App/驱动三个工程的全部源文件编成 .o，交叉比对未定义符号）：
   内核工程只剩 HAL/C 库符号（由工程内的 HAL 源文件提供），App/驱动工程只剩 armcc 的
   `Lib$$Request$$armlib`（链接时由 C 库解析）——无悬空符号。
   但**尚未在 Keil 里做过一次真实的 axf 链接与板上验证**，仍需上板后方可视为定稿。

