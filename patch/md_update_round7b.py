# -*- coding: utf-8 -*-
"""
文档同步（第 7 轮，第二部分）：
  A. readme.md 补充四处当前代码事实：超时语义约定、板级 HAL 时基职责、
     分区/镜像策略开关、源码编码约定、验证状态
  B. docs/README.md 索引补上《死代码与未接线审计》
  C. docs/死代码与未接线审计.md 追加第六节（本轮 db594a5 的记录）
"""
import os

ROOT = r'D:\工作\git_project\svcrtos_new'

EDITS = []


def E(path, old, new, note):
    EDITS.append((path, old, new, note))


# ============================================================ A. readme.md
E('readme.md',
  """用户程序包含 `svcrt.h` 即可使用所有操作系统接口，所有调用通过 SVC 指令陷入内核态执行。
""",
  """用户程序包含 `svcrt.h` 即可使用所有操作系统接口，所有调用通过 SVC 指令陷入内核态执行。

### 阻塞接口的超时语义（统一约定）

event / sem / mutex / mq 的等待接口共用同一套 `timeout_ms` 约定，与 FreeRTOS、
RT-Thread、Zephyr 一致：

| 取值 | 语义 |
|------|------|
| `timeout_ms == 0` | **不等待**：只试一次，条件不满足立即返回超时 |
| `timeout_ms > 0` | 最多等待 `timeout_ms` 毫秒，超时返回 |
| `timeout_ms < 0` | **永久等待**，直到条件满足 |

```c
svcrt_mutex_lock(mtx, -1);   /* 永久等待（推荐写法，语义一眼可见） */
svcrt_mutex_lock(mtx, 0);    /* 只试一次，拿不到就返回超时 */
```

> 早期版本把 0 当作"永久等待"，与主流 RTOS 相反，且同一头文件里三类接口写法不一致。
> 现已统一为上面的约定，仓库内的调用点也全部改成显式 `-1`。
""",
  'readme 补超时语义约定')

E('readme.md',
  """## 快速移植

SVCrtOS 可移植到任何 ARM Cortex-M MCU，只需在 `board/` 目录下创建新的芯片移植目录。""",
  """## 板级职责：节拍入口与 HAL 毫秒时基

`SysTick_Handler` 由**板级**实现，内核只对外提供 `svcrt_kernel_tick_handler()`。
除了喂内核节拍，如果板级用了 STM32 HAL，还必须在这里把 HAL 的毫秒时基推进起来：

```c
void SysTick_Handler(void)
{
    /* 内核节拍 500us = 2kHz，HAL 时基 1ms = 1kHz，所以每 2 个节拍补一次 */
    if((svcrt_kernel_get_tick() % (1000u / SVCRT_TICK_PERIOD_US)) == 0u)
    {
        HAL_IncTick();
    }

    svcrt_kernel_tick_handler();
}
```

**为什么必须补**：CubeMX 生成的 `SysTick_Handler`（全工程唯一调用 `HAL_IncTick()`
的地方）会被本工程在 `stm32f4xx_it.c` 里整体屏蔽。不补这一句，`HAL_GetTick()`
恒为 0，任何 `HAL_Delay()` 都会死等；`HAL_Delay()` 等的正是 GetTick 的变化。
HAL 属芯片相关代码，桥接只能放在 `board/`，内核保持零芯片依赖。

## 快速移植

SVCrtOS 可移植到任何 ARM Cortex-M MCU，只需在 `board/` 目录下创建新的芯片移植目录。""",
  'readme 补板级 HAL 时基职责')

E('readme.md',
  """3. **实现 `svcrt_board.c`**：实现 `svcrt_port.h` 中的所有接口函数 + 中断入口 + MPU 操作（M3 可跳过）
4. **移植上下文汇编**：参考 `kernelsrc/port/arm/cortex-m4/svcrt_context.S`（不同内核版本需适配 FPU 处理）
5. **编写板载驱动**：UART、LED 等""",
  """3. **实现 `svcrt_board.c`**：实现 `svcrt_port.h` 中的所有接口函数 + MPU 操作（M3 可跳过）
4. **写中断入口**：`SysTick_Handler` 转 `svcrt_kernel_tick_handler()`（用 STM32 HAL 的板子还要补 `HAL_IncTick()`，见上一节）；`HardFault_Handler` 必须转交内核故障处理，不要 `while(1)` 死循环
5. **移植上下文汇编**：参考 `kernelsrc/port/arm/cortex-m4/svcrt_context.S`（不同内核版本需适配 FPU 处理）
6. **编写板载驱动**：UART、LED 等""",
  'readme 移植步骤补中断入口一项')

E('readme.md',
  """| `SVCRT_USE_STACK_CHECK` / `SVCRT_STACK_END_FLAG` | 1 / 0xed01 | 栈溢出检测开关与栈底保护字 |
""",
  """| `SVCRT_USE_STACK_CHECK` / `SVCRT_STACK_END_FLAG` | 1 / 0xed01 | 栈溢出检测开关与栈底保护字 |

所有配置宏都用 `#ifndef` 包裹，板级配置先于 `svcrt_config.h` 加载，因此**覆盖板级配置
是唯一入口**，不需要改 `kernelsrc/`。

### 分区与镜像策略开关（`config/svcrt_partition.h`）

地址与分区布局只在 `config/svcrt_partition.h` 里定义一次，其余地址一律派生；
`.sct` 由 `tools/gen_scatter.py` 生成，属构建产物，**不要手工编辑**。
几个影响运行行为的策略开关：

| 宏 | `config/` 默认 | 说明 |
|----|----|----|
| `APP_AUTO_START` / `DRIVER_AUTO_START` | 1 | 上电扫描到有效镜像后自动启动；调试时置 0 |
| `APP_ALLOW_RAW_IMAGE` | 0 | 只认带镜像头的 `.svcapp`。本板在 `board/stm32f427/svcrt_board_config.h` 里显式置 1，保住"固定地址烧录 + MDK 下断点调试"的旁路 |
| `APP_CRASH_RESTART_MAX` | 3 | App/驱动连续故障重启上限，达到即禁用；0 = 不限次 |
| `INSTALLER_ENABLE` | 1 | 内核内安装任务（占用 COM1）；不用串口安装时置 0 |

> 注意：Loader / App 工程**禁止** `#include "svcrt_partition.h"`，布局在运行期经 SVC 0x18 获取。
""",
  'readme 补分区策略开关小节')

E('readme.md',
  """| 配置宏 | `SVCRT_USE_特性` | `SVCRT_USE_FPU`, `SVCRT_USE_MPU` |""",
  """| 配置宏 | `SVCRT_USE_特性` | `SVCRT_USE_FPU`, `SVCRT_USE_MPU` |

## 源码编码约定

- 历史源文件为 **GBK**（保证 Keil 编辑器里中文注释不乱码），近几轮新增模块为 **UTF-8**
- 修改 GBK 文件时按**字节级补丁**插入内容，**不要整文件转码**，否则 Keil 里的中文注释会变乱码
- `*.md` 文档统一 UTF-8
- `tools/gen_api_doc.py` 会先在系统临时目录生成一份 UTF-8 副本再跑 Doxygen，
  不改动仓库内任何源文件
- `patch/encoding_audit.py` 可扫描全仓编码，区分"整文件 GBK / 整文件 UTF-8 / 混合编码"

## 验证状态

当前仓库的改动只验证到两层证据：**AC6 `armclang -fsyntax-only` 语法校验** +
**Keil UV4 全量重建（内核 + 4 个示例工程，0 Error / 0 Warning）**。

**所有改动尚未上板**：MPU 隔离、故障恢复的"连续重启 3 次禁用"、串口安装的 256B
契约、HAL 毫秒时基的补 tick 精度，都还需要在真实硬件上跑一遍才算闭环。
已知未闭环项与每一轮的改动记录见
[死代码与未接线审计](docs/死代码与未接线审计.md)。""",
  'readme 补编码约定与验证状态')

# ============================================================ B. docs/README.md
E('docs/README.md',
  """| 查 API 签名与参数 | [api/SVCrtOS_API参考.md](api/SVCrtOS_API参考.md) |""",
  """| 查 API 签名与参数 | [api/SVCrtOS_API参考.md](api/SVCrtOS_API参考.md) |
| 想知道哪些东西坏了、哪些没接线、验证到哪一层 | [死代码与未接线审计.md](死代码与未接线审计.md)（诚实记录，含每轮改动表） |""",
  'docs/README 索引补审计文档')

E('docs/README.md',
  """├── SVCrtOS变更说明_自旋锁与栈分析.md        # 【变更说明】自旋锁/调度器锁/栈用量/文档生成""",
  """├── SVCrtOS变更说明_自旋锁与栈分析.md        # 【变更说明】自旋锁/调度器锁/栈用量/文档生成
├── 死代码与未接线审计.md                    # 【审计】已知缺陷/死代码/未接线清单 + 每轮改动与验证边界""",
  'docs/README 目录树补审计文档')

# ============================================================ C. 审计文档第六节
AUDIT = """## 六、第四轮补齐（2026-09-12，提交 db594a5）

本轮处理外部复核报告点名的第 4 项（驱动入口忙等），以及复核报告没点到、
核查时发现的 HAL 毫秒时基断链，另把一批被有损破坏的中文注释按代码实际行为重写。

### 6.1 已改并验证

| # | 问题（改前的事实） | 处理 | 提交 |
|---|---|---|---|
| 1 | **HAL 毫秒时基不推进**：CubeMX 生成的 `SysTick_Handler`（全工程唯一调用 `HAL_IncTick()` 的地方）已在 `stm32f4xx_it.c` 里被 `#if 0` 整体屏蔽，板级接管的同名函数只调 `svcrt_kernel_tick_handler()` → `HAL_GetTick()` 恒 0，任何 `HAL_Delay()` 死等 | 板级 `SysTick_Handler` 每 2 个节拍补一次 `HAL_IncTick()`（500us × 2 = 1ms），并加编译期断言 `1000 % SVCRT_TICK_PERIOD_US == 0`。HAL 属芯片相关，只放板级，内核保持零芯片依赖 | db594a5 |
| 2 | 驱动入口 `svcrt_drv_main.c` 的 `main()` 在 `DrvMain()` 返回后空转 `while(1)`。该文件被 BLED_DRV / DRV_DEMO 两个驱动工程直接编译，而驱动任务优先级 9 高于 App 10 → 吃满 CPU，饿死所有 App | 改为 `while(1){ svcrt_task_wait(1000); }`，与 `svcrt_app_main.c` 一致 | db594a5 |
| 3 | 14 个文件的注释被有损替换成字面 `?`（信息不可从文件恢复）。本轮处理其中 8 个 | 按"行号 + 特征子串"复核后，按代码实际行为重写注释。其中 `svcrt_driver_sdk.h` 原注释引用了并不存在的 `SVCRT_DRV_MODE_KERNEL/USER`，改按真实存在的 `SVCRT_DRV_USER_MODE` 描述 | db594a5 |
| 4 | `svcrt_mq.c` / `svcrt_mq.h` 混合编码（少数行 UTF-8、其余 GBK），编辑器打开必乱码 | 统一回 GBK。GBK 与 UTF-8 的中文序列天然歧义，转换以"全仓确定 GBK 行的非 ASCII 字符集"作参照集打分消歧 | db594a5 |

验证手段与结果（可复现）：

- AC6 `-fsyntax-only`：`svcrt_board.c` / `main.c` / `stm32f4xx_it.c` / `app_demo.c` /
  `svcrt_mq.c` 全部 OK。`svcrt_oslib.c` 与 `svcrt_driver_sdk.h` 含 `__svc` 固有语法，
  AC6 不可解（既有约束），该侧由 UV4 真实链接覆盖。
- UV4 全量重建 5 个工程（内核 + `APP_DEMO` / `BLED_APP` / `BLED_DRV` / `DRV_DEMO`）：
  0 Error / 0 Warning；内核 `Code=18850 RO=490 RW=136 ZI=20808`
  （较上轮 18806 增加 44 字节，即新加的 HAL 补 tick 代码）。
- `patch/encoding_audit.py` 全仓编码扫描：GBK 整文件 59 个、UTF-8 整文件 69 个，
  无混合编码 / 无法解码文件。
- 文档同步：`patch/md_update_round7.py` 把 5 份说明/教程里与代码不符的陈述拉齐
  （超时语义、`APP_ALLOW_RAW_IMAGE` 默认值、中断入口职责）。

### 6.2 本轮**没有**改、仍然存在的缺口

- 5 个 SDK 示例文件（`kernelsrc/sdk/driver_sdk/examples/example_{i2c,adc,gpio,spi,uart}_drv.c`，
  共 103 行）的注释同样被破坏成 `?`，本轮未处理。
- 5.2 列出的其余缺口（MPU 四项、串口 ORE、自旋锁未接线、死代码清单、陈旧副本、
  git 索引跟踪构建产物、AC5→AC6）**全部依旧**。

### 6.3 本轮未做的验证（不要当成已验证）

- **仍未上板**。HAL 时基补 tick 只做了"每 2 拍补一次"的静态推理与编译期断言，
  没有在硬件上读 `HAL_GetTick()` 或测时基相位，1ms 精度与节拍相位未实测。
- 驱动入口改 `svcrt_task_wait(1000)` 只验证了链接通过，没有实测
  "驱动常驻 + App 正常运行"的调度行为。
"""


def main():
    for path, old, new, note in EDITS:
        p = os.path.join(ROOT, path.replace('/', os.sep))
        raw = open(p, 'rb').read()
        text = raw.decode('utf-8')
        eol = '\r\n' if '\r\n' in text else '\n'
        o = old.replace('\n', eol)
        n = new.replace('\n', eol)
        cnt = text.count(o)
        assert cnt == 1, '%s / %s: 命中 %d 次（应为 1）' % (path, note, cnt)
        text = text.replace(o, n)
        if not text.endswith(eol):
            text += eol
        open(p, 'wb').write(text.encode('utf-8'))
        print('  [ok] %s —— %s' % (path, note))

    p = os.path.join(ROOT, 'docs', '死代码与未接线审计.md')
    text = open(p, 'rb').read().decode('utf-8')
    eol = '\r\n' if '\r\n' in text else '\n'
    assert '## 六、第四轮补齐' not in text
    if not text.endswith(eol):
        text += eol
    text += AUDIT.replace('\n', eol)
    open(p, 'wb').write(text.encode('utf-8'))
    print('  [ok] docs/死代码与未接线审计.md —— 追加第六节')
    print('全部完成')


if __name__ == '__main__':
    main()
