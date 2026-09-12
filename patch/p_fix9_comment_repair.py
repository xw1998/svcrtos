# -*- coding: utf-8 -*-
"""
修复 18：把中文注释被替换成 '?' 的文件补回人话；并修掉注释本来要记录的真问题

事实：以下文件的注释里出现了成片的字面 '?'，原本的中文已被有损转换破坏
（信息不可从文件恢复）。代码本身完好，因此按代码实际行为重写注释。

  kernelsrc/sdk/driver_sdk/svcrt_types.h           4 行
  kernelsrc/sdk/driver_sdk/svcrt_driver_sdk.h      9 行
  kernelsrc/sdk/app_sdk/svcrt_oslib.c              2 行
  board/stm32f427/svcrt_board.c                   15 行
  example/.../kernel/SVCRTOS_TEST/Core/Src/main.c   7 行
  example/.../Core/Src/stm32f4xx_it.c               6 行
  example/.../app_sdk/APP_DEMO/Src/app_demo.c       6 行
  example/.../driver_sdk/DRV_DEMO/Src/temp_drv.c   13 行

顺带修掉两条注释所指的真问题：

A) HAL 毫秒基准根本没有推进。
   stm32f4xx_it.c 里 CubeMX 生成的 SysTick_Handler（内含 HAL_IncTick()）已在
   源码内用 #if 0 整体屏蔽，而 board 接管的 SysTick_Handler 只调
   svcrt_kernel_tick_handler()——全工程再无第二处 HAL_IncTick()。
   后果：HAL_GetTick() 永远返回 0，任何用 HAL_Delay() 的驱动死等。
   修法：在 board 的 SysTick_Handler 里按整毫秒补调 HAL_IncTick()
   （内核节拍 500us = 2kHz，HAL 时基 1ms = 1kHz，故每 2 个节拍补一次），
   并对“节拍周期必须整除 1000us”加编译期断言（svcrt_task.c 已有同样假设）。

B) svcrt_driver_sdk.h 的注释提到 SVCRT_DRV_MODE_KERNEL / SVCRT_DRV_MODE_USER
   两个宏，而全仓并不存在这两个宏（实际开关是 SVCRT_DRV_USER_MODE）。
   按实际存在的宏重写。

替换方式：按行号定位 + 复核该行的特征子串，避免手抄乱码行造成误替换。
"""
import os
import shutil

ROOT = r'D:\工作\git_project\svcrtos_new'
BK = r'C:\Users\14905\OneDrive\Documents\lingxi-claw\20260911-13-47-19-445\backup_fix9'


def detect_enc(raw):
    try:
        raw.decode('utf-8')
        return 'utf-8'
    except UnicodeDecodeError:
        return 'gbk'


class Doc(object):
    def __init__(self, rel):
        self.rel = rel
        self.p = os.path.join(ROOT, rel.replace('/', os.sep))
        shutil.copy2(self.p, os.path.join(BK, rel.replace('/', '__')))
        self.raw = open(self.p, 'rb').read()
        self.enc = detect_enc(self.raw)

    def patch_lines(self, repl):
        text = self.raw.decode(self.enc)
        lines = text.split('\n')
        for lno, expect, new in repl:
            cur = lines[lno - 1]
            if cur.rstrip('\r') == new:
                continue        # 幂等：该行已经是修好的内容
            if expect is None:
                # 该行原注释已整行退化成 '?'，没有 ASCII 锚点可用；
                # 只校验它确实是一条注释行（避免行号错位改到代码）
                st = cur.strip()
                if '?' not in cur and not st.startswith('*') and not st.startswith('/*'):
                    raise AssertionError('%s L%d 期待是注释行，实际: %r'
                                         % (self.rel, lno, cur))
            elif expect not in cur:
                raise AssertionError('%s L%d 复核失败\n  期望含: %r\n  实际内容: %r'
                                     % (self.rel, lno, expect, cur))
            cr = '\r' if cur.endswith('\r') else ''
            lines[lno - 1] = new + cr
        self.raw = '\n'.join(lines).encode(self.enc)

    def insert_after(self, lno, expect, new, label):
        text = self.raw.decode(self.enc)
        lines = text.split('\n')
        cur = lines[lno - 1]
        if expect not in cur:
            raise AssertionError('%s L%d 复核失败: %r' % (self.rel, lno, cur))
        cr = '\r' if cur.endswith('\r') else ''
        lines.insert(lno, new + cr)
        self.raw = '\n'.join(lines).encode(self.enc)
        print('   插入: %s' % label)

    def save(self):
        open(self.p, 'wb').write(self.raw)
        print('   saved %s [%s]' % (self.rel, self.enc))


# ------------------------------------------------------------------ 1. types
TYPES = [
    (2, 'SVCrtOS', '* @brief SVCrtOS Driver SDK 基础类型定义'),
    (3, '@details', '* @details 为 Driver SDK 提供与内核一致的基础整型别名，'),
    (4, None, '*          不含任何 MCU 头文件与 OS 依赖，驱动可独立编译。'),
    (5, None, '*          定义与 kernelsrc/include/svcrt_types.h 保持一致。'),
]

# ------------------------------------------------------------------ 2. driver sdk
DRVSDK = [
    (2, 'SVCrtOS', '* @brief SVCrtOS Driver SDK - 驱动开发接口'),
    (3, '@details', '* @details 驱动有两种部署形态，注册/注销/计数接口相同，'),
    (4, None, '*          驱动代码不需要区分自己是哪种形态：'),
    (5, None, '*          由宏 SVCRT_DRV_USER_MODE 选择：'),
    (6, None, '*          - 未定义（默认）：内核态模式，随内核一起编译，直接调用 svcrt_dev_register 等'),
    (7, None, '*          - 已定义：用户态模式，编译为独立固件，经 SVC 0x14 与内核交互'),
    (8, None, '*          用户态模式下任务/事件接口由 svcrt_drv_oslib.c 提供 SVC 封装。'),
    (9, None, '*          本头文件不依赖任何 MCU 头文件，SDK 可独立使用。'),
    (58, 'oslib', '/* 以下任务/事件接口由 svcrt_drv_oslib.c 提供 SVC 封装 */'),
]

# ------------------------------------------------------------------ 3. oslib
OSLIB = [
    (2, 'SVCrtOS', '* @brief SVCrtOS App SDK - 系统调用封装实现'),
    (3, '@details', '* @details 把应用侧 API 封装成 SVC 调用；应用通过 svcrt.h 使用，不直接接触内核。'),
]

# ------------------------------------------------------------------ 4. board
BOARD = [
    (2, 'STM32F427', '* @brief SVCrtOS 板级移植层 - STM32F427'),
    (3, 'port', '* @details 实现 port 层要求板级提供的基础配置：'),
    (4, 'CPACR', '*          - FPU 使能（CPACR + FPCCR）'),
    (5, 'NVIC', '*          - NVIC 优先级分组与内核异常优先级'),
    (6, 'COM1', '*          - 板载设备注册（COM1 / LED / LED2）'),
    (7, '?', '*          - CPU 异常向量与系统节拍入口'),
    (8, 'CPU', '*          其余 CPU 相关实现全部在 port/arm/cortex-m4/，'),
    (9, 'port/arm/cortex-m4/', '*          board/ 只承担芯片相关的差异。'),
    (24, 'port', ' * 板级初始化 - 实现 port 层板级接口'),
    (33, 'port', ' * 中断优先级初始化 - 实现 port 层板级接口'),
    (53, 'MPU', ' * 空闲任务 MPU 配置 - 实现 port 层板级接口'),
    (67, '?', ' * 板载设备注册'),
    (80, '?', ' * 内核异常与节拍向量'),
    (81, 'Cortex-M', ' * @brief Cortex-M 核内异常向量的板级接线'),
    (82, None, ' *          硬故障/内存管理/总线/用法异常统一交内核故障处理，'),
]

# 第 83 行（=shift）是注释块的 '====*/' 收尾行，不能动；
# 要补的说明插在它前面。
BOARD_NOTE = (' *          节拍中断交内核 tick 处理，并同步推进 HAL 毫秒基准。')

SYSTICK_NEW = """void SysTick_Handler(void)
{
    /* HAL 的毫秒基准必须跟着内核节拍一起走。
     * CubeMX 生成的那个 SysTick_Handler（内含 HAL_IncTick()）已在
     * stm32f4xx_it.c 里用 #if 0 整体屏蔽，全工程再无第二处调用 ——
     * HAL_GetTick() 永远返回 0，任何用 HAL_Delay() 的驱动都会死等。
     * 内核节拍周期是 SVCRT_TICK_PERIOD_US（本板 500us，即 2kHz），
     * 而 HAL 时基是 1ms（1kHz），所以每 2 个内核节拍补一次 HAL_IncTick()。 */
    #if ((1000 % SVCRT_TICK_PERIOD_US) != 0)
    #error "SVCRT_TICK_PERIOD_US 必须能整除 1000us，否则无法按整毫秒为 HAL 时基补 tick"
    #endif

    if((svcrt_kernel_get_tick() % (1000u / SVCRT_TICK_PERIOD_US)) == 0u)
    {
        HAL_IncTick();
    }

    svcrt_kernel_tick_handler();
}"""

# ------------------------------------------------------------------ 5. main.c
MAIN = [
    (72, None, '/* 两个 LED 任务共用一把互斥锁，保护对 LED 设备的写入 */'),
    (199, '?', '        /* 加锁后写设备：len>0 点亮、len==0 熄灭（约定见板级 drvled） */'),
    (201, '?', '        svcrt_dev_write_internal(led, &on, 1);      /* 点亮 */'),
    (206, '?', '        svcrt_dev_write_internal(led, &on, 0);      /* 熄灭 */'),
    (219, '?', '        /* 第二个 LED：先灭后亮，与 led_blink_task 相位相反 */'),
    (221, '?', '        svcrt_dev_write_internal(led, &on, 0);      /* 熄灭 */'),
    (226, '?', '        svcrt_dev_write_internal(led, &on, 1);      /* 点亮 */'),
]

# ------------------------------------------------------------------ 6. it.c
IT = [
    (83, 'svcrt_board.c', '  * @note  由 SVCRTOS 内核接管（见 svcrt_board.c 同名函数），此处整体屏蔽'),
    (158, 'context_rvds.S', '  * @note  由 SVCRTOS 内核接管（端口层汇编实现 SVC_Handler），此处整体屏蔽'),
    (187, 'context_rvds.S', '  * @note  由 SVCRTOS 内核接管（端口层汇编实现 PendSV_Handler），此处整体屏蔽'),
    (203, 'svcrt_board.c', '  * @note  由 SVCRTOS 内核接管（见 svcrt_board.c 同名函数），此处整体屏蔽'),
    (204, 'HAL_IncTick', '  *        本函数里的 HAL_IncTick() 随之一并屏蔽，改由 svcrt_board.c 的'),
    (205, 'svcrt_kernel_tick_handler', '  *        SysTick_Handler 按整毫秒补调（HAL_Delay/HAL_GetTick 依赖它）。'),
]

# ------------------------------------------------------------------ 7. app_demo
APP = [
    (2, 'SVCrtOS', '* @brief SVCrtOS App SDK 示例程序 - LED 闪烁与串口回显'),
    (3, '@details', '* @details 演示 App SDK 的基本用法：设备 IO、互斥锁、信号量与延时。'),
    (6, '?', '*          所有系统调用都经 SVC 陷入内核，App 不直接访问硬件。'),
    (23, '?', '        /* 互斥锁保护对 LED 设备的写入 */'),
    (36, '?', '        /* 串口回显 */'),
    (43, '?', '                /* 收到数据后释放信号量（此处仅演示 post） */'),
]

# ------------------------------------------------------------------ 8. temp_drv
TEMP = [
    (2, 'SVCrtOS', '* @brief SVCrtOS Driver SDK 示例驱动 - 温度传感器（模拟）'),
    (3, '@details', '* @details 演示独立驱动的最小实现：svcrt_dev_drv_t 五个接口 + DrvMain 注册。'),
    (4, None, '*          可编译为独立 .bin 固件，烧录到驱动池分区（起始 0x08040000）。'),
    (5, None, '*          应用侧用 svcrt_dev_open("TEMP", 0) 即可按设备名访问。'),
    (6, None, '*          注册经 SVC 0x14，由 svcrt_driver_bridge.c 转发到内核。'),
    (11, '?', '/* 控制命令码 */'),
    (12, 'TEMP_CTRL_SET_UNIT', '#define TEMP_CTRL_SET_UNIT   (0x0100)   /* 0=摄氏度，1=华氏度 */'),
    (16, 'unit', '    uint8   unit;           /* 单位（0=摄氏度，1=华氏度） */'),
    (17, 'last_temp', '    int16   last_temp;      /* 最近一次读数，以 0.1 度为单位 */'),
    (27, 'last_temp', '    temp_obj.last_temp      = 250;       /* 25.0 度（以 0.1 度为单位） */'),
    (52, '9 / 5', '        v = (int16)(v * 9 / 5 + 320);   /* 摄氏度转华氏度，同样按 0.1 度的定点表示 */'),
    (62, 'SVCRT_DRV_ERROR', '    return SVCRT_DRV_ERROR;   /* 温度只读，不支持写入 */'),
    (94, '?', '        /* 注册完成后驱动无需后台动作：睡下去让出 CPU 给 App。 */'),
]


def main():
    os.makedirs(BK, exist_ok=True)

    d = Doc('kernelsrc/sdk/driver_sdk/svcrt_types.h')
    d.patch_lines(TYPES); d.save()

    d = Doc('kernelsrc/sdk/driver_sdk/svcrt_driver_sdk.h')
    d.patch_lines(DRVSDK); d.save()

    d = Doc('kernelsrc/sdk/app_sdk/svcrt_oslib.c')
    d.patch_lines(OSLIB); d.save()

    d = Doc('board/stm32f427/svcrt_board.c')
    # 若上次运行已经把 HAL 头插到第 15 行，那行号 >14 的锚点全部下移 1
    has_inc = '#include "stm32f4xx_hal.h"' in d.raw.decode(d.enc)
    shift = 1 if has_inc else 0
    d.patch_lines([(l + shift if l > 14 else l, e, n) for (l, e, n) in BOARD])
    if '节拍中断交内核 tick' not in d.raw.decode(d.enc):
        d.insert_after(82 + shift,
                       '硬故障/内存管理/总线/用法异常统一交内核故障处理',
                       BOARD_NOTE, 'svcrt_board.c 注释块补一行说明')
    if not has_inc:
        d.insert_after(14, 'stm32f4xx.h', '#include "stm32f4xx_hal.h"   /* HAL_IncTick(): 板级补 HAL 毫秒时基 */',
                       'svcrt_board.c 引入 HAL 头（HAL_IncTick）')
    d.save()

    # SysTick_Handler 整体替换（按内容定位，不依赖行号）
    text = d.raw.decode(d.enc)
    lines = text.split('\n')
    if 'HAL 的毫秒基准必须跟着内核节拍一起走' in text:
        pass                                   # 幂等：已经换过了
    else:
        idx = None
        for i, l in enumerate(lines):
            if l.strip().startswith('void SysTick_Handler'):
                idx = i
                break
        assert idx is not None, 'svcrt_board.c 里找不到 SysTick_Handler'
        old = lines[idx:idx + 4]
        assert '{' in old[1] and 'svcrt_kernel_tick_handler' in old[2] and '}' in old[3], old
        eol = '\r' if old[0].endswith('\r') else ''
        new_block = [l + eol for l in SYSTICK_NEW.split('\n')]
        lines[idx:idx + 4] = new_block
        d.raw = '\n'.join(lines).encode(d.enc)
        d.save()

    d = Doc('example/stm32f427/kernel/SVCRTOS_TEST/Core/Src/main.c')
    d.patch_lines(MAIN); d.save()

    d = Doc('example/stm32f427/kernel/SVCRTOS_TEST/Core/Src/stm32f4xx_it.c')
    d.patch_lines(IT); d.save()

    d = Doc('example/stm32f427/app_sdk/APP_DEMO/Src/app_demo.c')
    d.patch_lines(APP); d.save()

    d = Doc('example/stm32f427/driver_sdk/DRV_DEMO/Src/temp_drv.c')
    d.patch_lines(TEMP); d.save()

    print('OK')


if __name__ == '__main__':
    main()
