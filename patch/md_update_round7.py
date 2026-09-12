# -*- coding: utf-8 -*-
"""
文档同步（第 7 轮）：把说明/教程类 md 拉齐到当前代码

覆盖的过期陈述：
  1. SVCrtOS内核文件详解.md —— event_wait 还写着"timeout_ms == 0 永久等待"
     （已被 400b3f9 改成 0=不等待），且节拍链路图没有板级 HAL 时基桥接
  2. kernelsrc/porting_manual.md —— SysTick_Handler 示例没有 HAL 时基桥接，
     HardFault_Handler 还是 while(1) 死循环（真实板级早已改成交内核故障处理）
  3. SVCrtOS移植手册.md —— 中断分节没提 HAL 时基是板级职责
  4. docs/Loader工程化落地说明.md / docs/SVCrtOS应用安装与调试指南.md ——
     APP_ALLOW_RAW_IMAGE 的默认值写反了（现已是"config 默认 0 + 板级显式置 1"）
  5. 从零实现RTOS教程.md —— 心跳章节补一句 HAL 时基桥接

脚本只改 md（全部 UTF-8），逐条断言命中次数，避免误替换。
"""
import io
import os
import sys

ROOT = r'D:\工作\git_project\svcrtos_new'

EDITS = []


def E(path, old, new, note):
    EDITS.append((path, old, new, note))


# ------------------------------------------------------------------ 1. 内核文件详解
E('SVCrtOS内核文件详解.md',
  """1. 把当前任务加入该事件的 waiting_tasks 列表
2. 若 timeout_ms > 0: 调用 svcrt_task_wait_internal(timeout_ms)（限时等待）
3. 若 timeout_ms == 0: 调用 svcrt_task_wait_period_internal()（永久等待，直到被 set 唤醒）
```""",
  """1. 句柄校验（标志位 / 索引范围 / 槽位已用）
2. 事件已置位：直接消费掉这次置位并返回 0，不阻塞（否则"先 set 后 wait"会丢事件）
3. 若 timeout_ms == 0：只试一次，未置位立即返回超时，不登记等待者、不阻塞
4. 若 timeout_ms > 0：登记到该事件的 waiting_tasks 列表后限时等待
5. 若 timeout_ms < 0：登记后永久等待，直到被 set 唤醒
```

> **超时语义全系统统一**（与 FreeRTOS / RT-Thread / Zephyr 一致）：
> **0 = 不等待（只试一次），负值 = 永久等待**。event / sem / mutex 三套接口都在登记等待者
> 之前先判 `timeout_ms == 0` 提前返回，不再依赖内部原语把 0 当永久等待来解释。""",
  '修正 event_wait 的超时语义描述')

E('SVCrtOS内核文件详解.md',
  """  ├─ SysTick_Handler()
  │   └─ svcrt_kernel_tick_handler()""",
  """  ├─ SysTick_Handler()                # 板级实现（board/<芯片>/svcrt_board.c）
  │   ├─ 每 2 个节拍补一次 HAL_IncTick()  # 用 STM32 HAL 的板子必须补，见下方说明
  │   └─ svcrt_kernel_tick_handler()""",
  '节拍链路图补板级 HAL 时基桥接')

# ------------------------------------------------------------------ 2. 移植层说明
E('kernelsrc/porting_manual.md',
  """/* 中断服务程序入口 */
void SysTick_Handler(void)
{
    svcrt_kernel_tick_handler();
}

void HardFault_Handler(void)
{
    while(1);
}""",
  """/* 中断服务程序入口 */
void SysTick_Handler(void)
{
    /* 用 STM32 HAL 的板子：必须在这里把 HAL 的毫秒时基推进起来。
     * CubeMX 生成的 SysTick_Handler（内含 HAL_IncTick()）被本工程在
     * stm32f4xx_it.c 里整体屏蔽了，不补这一句，HAL_GetTick() 会永远是 0，
     * 任何 HAL_Delay() 都会死等（HAL_Delay 等的是 GetTick 变化）。
     * 内核节拍 500us = 2kHz，HAL 时基 1ms = 1kHz，所以每 2 个节拍补一次；
     * 同时要保证 SVCRT_TICK_PERIOD_US 能整除 1000，否则补出来的时基不是整毫秒。 */
    if((svcrt_kernel_get_tick() % (1000u / SVCRT_TICK_PERIOD_US)) == 0u)
    {
        HAL_IncTick();
    }

    svcrt_kernel_tick_handler();
}

void HardFault_Handler(void)
{
    /* 不要在这里 while(1) 死循环：交内核故障处理，才会写故障记录、
     * 按策略重建任务栈帧恢复，并在连续故障达到上限时禁用该 App。 */
    uint32 sp = svcrt_hardfault_handler();

    if(sp != 0u)
    {
        svcrt_port_resume_task(sp);   /* 内核选出了可运行任务，直接恢复（本函数不返回） */
    }

    while(1) { }
}""",
  '移植手册的中断入口示例补 HAL 时基与故障委托')

# ------------------------------------------------------------------ 3. 移植手册
E('SVCrtOS移植手册.md',
  """| 函数 | 实现位置 |
|------|---------|
| `HardFault_Handler` | `svcrt_board.c` |
| `SysTick_Handler` | `svcrt_board.c` |""",
  """| 函数 | 实现位置 | 说明 |
|------|---------|------|
| `HardFault_Handler` | `svcrt_board.c` | 必须转交内核故障处理，不能 `while(1)` |
| `SysTick_Handler` | `svcrt_board.c` | 除 `svcrt_kernel_tick_handler()` 外，**用 HAL 的板子还要补 `HAL_IncTick()`** |""",
  '移植手册中断表补 HAL 时基职责')

E('SVCrtOS移植手册.md',
  """| 中断 | 优先级 | 处理函数 | 所在文件 |
|------|--------|---------|---------|
| SysTick | 0x00（最高） | `SysTick_Handler` | `svcrt_board.c` |""",
  """| 中断 | 优先级 | 处理函数 | 所在文件 |
|------|--------|---------|---------|
| SysTick | 0x00（最高） | `SysTick_Handler` | `svcrt_board.c`（转内核节拍 + 补 HAL 毫秒时基） |""",
  '中断分配表补说明')

# ------------------------------------------------------------------ 4. Loader 说明
E('docs/Loader工程化落地说明.md',
  """- **开发期**：`APP_ALLOW_RAW_IMAGE = 1`（默认）—— 直接用 Keil 下载到 `0x08080000` / `DRIVER_POOL` 即可下断点调试
- **发布固件**：把 `APP_ALLOW_RAW_IMAGE` 置 0 —— 只接受带镜像头的 `.svcapp`，裸镜像会被判为 `INVALID`""",
  """- **默认（发布语义）**：`config/svcrt_partition.h` 里 `APP_ALLOW_RAW_IMAGE` 默认 **0** ——
  只接受带镜像头的 `.svcapp`，裸镜像被判为 `INVALID`
- **开发期（本板已打开）**：`board/stm32f427/svcrt_board_config.h` 里显式
  `#define APP_ALLOW_RAW_IMAGE 1` —— 直接用 Keil 下载到 `0x08080000` / `DRIVER_POOL` 即可下断点调试
- 该宏在 `config/` 里被 `#ifndef` 包裹，**板级覆盖是唯一入口**：切换发布/调试只改板级配置，
  不动 `config/`，`config/` 始终代表"默认拒绝裸镜像"的发布语义""",
  'Loader 说明修正 APP_ALLOW_RAW_IMAGE 默认值')

# ------------------------------------------------------------------ 5. 安装调试指南
E('docs/SVCrtOS应用安装与调试指南.md',
  """| `APP_ALLOW_RAW_IMAGE` | 1 | 允许槽位内是"裸镜像"（开发期）。置 0 则只认带镜像头的 `.svcapp` | **发布固件时置 0** |""",
  """| `APP_ALLOW_RAW_IMAGE` | `config/` 里为 0；本板 `svcrt_board_config.h` 显式置 1 | 允许槽位内是"裸镜像"（开发期）。为 0 则只认带镜像头的 `.svcapp` | 发布固件时**删掉板级覆盖或置 0** |""",
  '安装调试指南修正开关默认值')

E('docs/SVCrtOS应用安装与调试指南.md',
  """   - `APP_ALLOW_RAW_IMAGE = 1`（必须，否则裸镜像不被认定）""",
  """   - `APP_ALLOW_RAW_IMAGE = 1`（本板已在 `svcrt_board_config.h` 显式置 1；为 0 则裸镜像不被认定）""",
  '调试步骤补说明')

E('docs/SVCrtOS应用安装与调试指南.md',
  """| 保留 `APP_ALLOW_RAW_IMAGE = 1` | 否则裸镜像不被认定，App 不会启动 |""",
  """| 保留板级的 `APP_ALLOW_RAW_IMAGE = 1` 覆盖 | 否则裸镜像不被认定，App 不会启动 |""",
  '调试建议补说明')

# ------------------------------------------------------------------ 6. 教程
E('从零实现RTOS教程.md',
  """SysTick 每隔固定时间（默认 500 微秒，`SVCRT_TICK_PERIOD_US` 配置）触发中断，板级中断入口调用内核的 `svcrt_kernel_tick_handler`（[svcrt_task.c](file:///d:/项目文件/SVCRTOS/kernelsrc/src/svcrt_task.c)）：""",
  """SysTick 每隔固定时间（默认 500 微秒，`SVCRT_TICK_PERIOD_US` 配置）触发中断，板级中断入口调用内核的 `svcrt_kernel_tick_handler`（[svcrt_task.c](file:///d:/项目文件/SVCRTOS/kernelsrc/src/svcrt_task.c)）。

> **板级还要顺手做一件事**：如果板子用了 STM32 HAL，必须在同一个 `SysTick_Handler` 里
> 每 2 个节拍补一次 `HAL_IncTick()`。CubeMX 生成的那个 `SysTick_Handler`（内含 `HAL_IncTick()`）
> 会被本工程整体屏蔽，不补的话 `HAL_GetTick()` 永远是 0，`HAL_Delay()` 会死等。
> 内核自己不认识 HAL，这个桥接只能放在板级。""",
  '教程心跳章节补 HAL 时基桥接')


def main():
    changed = 0
    for path, old, new, note in EDITS:
        if old == new:
            continue
        p = os.path.join(ROOT, path.replace('/', os.sep))
        raw = open(p, 'rb').read()
        text = raw.decode('utf-8')          # 全部 md 均为 UTF-8
        eol = '\r\n' if '\r\n' in text else '\n'
        o = old.replace('\n', eol)
        n = new.replace('\n', eol)
        cnt = text.count(o)
        assert cnt == 1, '%s / %s: 命中 %d 次（应为 1）' % (path, note, cnt)
        text = text.replace(o, n)
        open(p, 'wb').write(text.encode('utf-8'))
        print('  [ok] %s —— %s' % (path, note))
        changed += 1
    print('共修改 %d 处' % changed)


if __name__ == '__main__':
    main()
