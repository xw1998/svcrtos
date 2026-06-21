# 从零开始实现一个 RTOS —— SVCrtOS 完全教程

> 本文面向**完全没有写过操作系统、但会一点 C 语言和单片机**的初学者。
> 我们会从"什么是任务切换"讲起，一步步把 SVCrtOS 这个真实的 RTOS 拆开揉碎，
> 让你不仅看懂代码，更明白**每一行代码为什么要这么写**。
>
> 读完本文，你将理解：任务是怎么"同时"运行的、CPU 是怎么在任务间切换的、
> 系统调用为什么要用 SVC 指令、内存保护怎么做、设备驱动框架怎么搭。
>
> 配套代码就是本仓库的 `kernelsrc/`，所有代码片段都可在源码中找到对应。

---

## 目录

- [第 0 章 写在前面：RTOS 到底是什么](#第-0-章-写在前面rtos-到底是什么)
- [第 1 章 预备知识：Cortex-M 内核基础](#第-1-章-预备知识cortex-m-内核基础)
- [第 2 章 第一块基石：让两个任务"轮流跑"](#第-2-章-第一块基石让两个任务轮流跑)
- [第 3 章 调度器：决定"该谁跑"](#第-3-章-调度器决定该谁跑)
- [第 4 章 时间管理：SysTick 心跳与延时](#第-4-章-时间管理systick-心跳与延时)
- [第 5 章 系统调用：用 SVC 进入内核态](#第-5-章-系统调用用-svc-进入内核态)
- [第 6 章 内存保护：MPU 与特权隔离](#第-6-章-内存保护mpu-与特权隔离)
- [第 7 章 设备驱动框架](#第-7-章-设备驱动框架)
- [第 8 章 任务间同步：事件、信号量、互斥锁](#第-8-章-任务间同步事件信号量互斥锁)
- [第 9 章 进阶：外部分区固件](#第-9-章-进阶外部分区固件)
- [第 10 章 踩过的坑与调试方法](#第-10-章-踩过的坑与调试方法)

---

## 第 0 章 写在前面：RTOS 到底是什么

### 0.1 从"裸机"说起

如果你写过单片机程序，多半写过这样的代码：

```c
int main(void)
{
    init();
    while(1)
    {
        read_sensor();      // 读传感器
        update_display();   // 刷新显示
        handle_button();    // 处理按键
    }
}
```

这叫**裸机（bare-metal）轮询**。三件事在一个大循环里排队执行。问题是：如果 `read_sensor()` 要等 100ms，那这 100ms 里按键就没人理了。事情一多，代码就变成一团乱麻。

**RTOS（实时操作系统）** 解决的就是这个问题。它让你把每件事写成一个独立的"任务（task）"，每个任务自己有一个 `while(1)` 循环，看起来像是它们在**同时运行**：

```c
void sensor_task(void)   { while(1){ read_sensor();    svcrt_task_wait(100); } }
void display_task(void)  { while(1){ update_display(); svcrt_task_wait(50);  } }
void button_task(void)   { while(1){ handle_button();  svcrt_task_wait(10);  } }
```

### 0.2 "同时运行"是假象

单核 CPU 任何时刻只能执行一条指令，不可能真同时跑三个任务。RTOS 玩的是**快速切换**的把戏：跑一会儿 sensor_task，存档；切去跑 button_task，存档；再切回来……切换够快，人眼就觉得是"同时"。

这个"存档 / 读档"的动作，就是 RTOS 最核心的技术——**上下文切换（context switch）**。整个教程的灵魂就在这里。

### 0.3 SVCrtOS 的特别之处

市面上的 RTOS 很多（FreeRTOS、RT-Thread……）。SVCrtOS 的特色是用了 **SVC 指令做特权隔离**：用户任务跑在"非特权态"，不能直接碰内核数据；要调用系统功能（如打开设备）必须通过 `SVC` 指令"陷入"内核态。再配合 **MPU（内存保护单元）**，一个任务跑飞了也不会破坏别的任务——这就是名字里 "SVC" 的由来。

别担心这些名词，我们会逐个拆解。

---

## 第 1 章 预备知识：Cortex-M 内核基础

要实现 RTOS，必须先认识我们的"舞台"——ARM Cortex-M 处理器。只讲实现 RTOS 必须知道的部分。

### 1.1 寄存器：CPU 的"草稿纸"

CPU 干活时，数据不是直接在内存里算的，而是先搬到**寄存器**里。Cortex-M 有这些寄存器：

| 寄存器 | 用途 |
|--------|------|
| R0 ~ R12 | 通用寄存器，存中间数据 |
| R13 (SP) | 栈指针，指向当前栈顶 |
| R14 (LR) | 链接寄存器，存函数返回地址 |
| R15 (PC) | 程序计数器，指向**下一条要执行的指令** |
| xPSR | 程序状态寄存器，存标志位（如运算是否进位） |

**关键认知**：一个任务"执行到哪了、算到一半的数据是什么"，**完全由这十几个寄存器的值决定**。所以"存档"就是把这些寄存器存起来，"读档"就是把它们恢复回去。这组寄存器的快照，就叫**上下文（context）**。

### 1.2 两个栈指针：MSP 与 PSP

这是 Cortex-M 为 RTOS 量身定做的设计。它有**两个**栈指针：

- **MSP（主栈指针）**：复位后默认用它，中断处理（异常）时强制用它。
- **PSP（进程栈指针）**：可以让普通线程代码用它。

`SP` 这个名字在不同模式下指向 MSP 或 PSP 之一。**RTOS 的经典做法**：

- 每个任务有自己独立的栈，任务运行时 `SP = PSP`，指向该任务的栈。
- 进入中断（如切换中断）时，硬件自动切到 `SP = MSP`，用系统的栈。

这样**任务栈和中断栈分开**，任务切换时只需要换 PSP 指向哪块内存，就等于换了一个任务的栈。这是上下文切换的物理基础。

> SVCrtOS 中，[svcrt_port.c](file:///d:/项目文件/SVCRTOS/kernelsrc/port/arm/cortex-m4/svcrt_port.c) 的 `svcrt_port_set_psp()` 就是封装了设置 PSP 的 CMSIS 函数 `__set_PSP()`。

### 1.3 特权级与 CONTROL 寄存器

Cortex-M 的线程代码可以运行在两种级别：

- **特权级（Privileged）**：能访问所有寄存器和内存，能改系统配置。
- **非特权级（Unprivileged）**：受限，碰不了某些系统寄存器，配合 MPU 还能限制内存访问。

切换由 `CONTROL` 寄存器的两个 bit 控制：
- bit0：0=特权，1=非特权
- bit1：0=线程用 MSP，1=线程用 PSP

SVCrtOS 启动时这样设置（见 [svcrt_port.c](file:///d:/项目文件/SVCRTOS/kernelsrc/port/arm/cortex-m4/svcrt_port.c) 的 `svcrt_port_enter_idle`）：

```c
void svcrt_port_enter_idle(uint32 psp, uint32 use_priv)
{
    svcrt_port_set_psp(psp);                                  // 设置 PSP
    if(use_priv)
        svcrt_port_set_control(0x3 | svcrt_port_get_control());  // bit0,bit1=1: 非特权+PSP
    else
        svcrt_port_set_control(0x2 | svcrt_port_get_control());  // bit1=1: 特权+PSP
    svcrt_port_isb();   // 指令同步屏障，确保设置生效
}
```

> `0x3` = 二进制 `11` = 非特权 + 用 PSP；`0x2` = `10` = 特权 + 用 PSP。
> 用户任务跑在非特权态，是 SVC 隔离的前提。

### 1.4 异常与中断：硬件帮你压栈

Cortex-M 有个非常贴心的特性：**进入异常/中断时，硬件会自动把 8 个寄存器压栈**（R0、R1、R2、R3、R12、LR、PC、xPSR），退出时自动弹出。这 8 个寄存器叫**异常栈帧（exception stack frame）**。

为什么 RTOS 要利用这一点？因为上下文切换需要保存全部 R0-R15。硬件已经帮我们存了 8 个，**剩下的 R4-R11 我们手动存**就行——这正是后面 PendSV 汇编要做的事。

### 1.5 PendSV：专为任务切换设计的异常

Cortex-M 有一个低优先级的异常叫 **PendSV（可挂起的系统调用）**。它的设计目的就是给 RTOS 做上下文切换用：

- 你随时可以"挂起"它（设置一个寄存器的 bit），CPU 会在合适的时机进入它的处理函数。
- 把它设成**最低优先级**，保证它只在所有中断都处理完后才执行，不会打断别的中断。

SVCrtOS 触发切换就是挂起 PendSV（见 [svcrt_port.c](file:///d:/项目文件/SVCRTOS/kernelsrc/port/arm/cortex-m4/svcrt_port.c)）：

```c
void svcrt_port_switch_task(void)
{
    SCB->ICSR = SCB_ICSR_PENDSVSET_Msk;   // 挂起 PendSV
}
```

记住这个函数，第 2 章它就是切换的"扳机"。

---

## 第 2 章 第一块基石：让两个任务"轮流跑"

这一章是整个 RTOS 的心脏。我们要搞清楚一件事：**CPU 怎么从任务 A 切到任务 B？**

### 2.1 任务的"档案"：TCB

每个任务都要有一份"档案"，记录它的全部信息。这份档案叫 **TCB（Task Control Block，任务控制块）**。SVCrtOS 的 TCB 定义在 [svcrt_task.h](file:///d:/项目文件/SVCRTOS/kernelsrc/include/svcrt_task.h)：

```c
typedef struct {
    uint32 ram_start;       // 任务 RAM 区起始（给 MPU 用）
    uint32 ram_size;        // 任务 RAM 区大小
    uint32 stack_size;      // 栈大小
    uint32 rom_start;       // 任务 ROM 区起始
    uint32 rom_size;        // 任务 ROM 区大小
    int32  period;          // 周期（周期任务用）
    uint8  priority;        // 优先级，数字越小越高
    uint8  shm_attri;       // 共享内存属性
    uint32 stack_top;       // 栈顶地址
    uint32 *stack_bottom;   // 栈底地址（栈溢出检测用）
    uint32 mpu_bar[8];      // MPU 区域基址寄存器快照
    uint32 mpu_asr[8];      // MPU 区域属性寄存器快照
    svcrt_task_status_t status;  // 任务状态（见下）
    int32  period_time;     // 周期倒计时
    int32  wait_time;       // 等待倒计时
    uint32 tim_tick;        // 上次更新时间戳
    uint32 touch_tick;      // 上次被调度时间戳（轮转用）
    uint32 stack_ptr;       // ★ 最关键：保存的栈指针（PSP）
} svcrt_task_t;
```

**最重要的字段是 `stack_ptr`**。它就是这个任务的"存档槽"——任务被切走时把当时的 PSP 存进这里；切回来时从这里读出 PSP，就能恢复现场。

所有任务的 TCB 排成数组，就是**任务表**：

```c
svcrt_task_t svcrt_task_table[SVCRT_TASK_MAX_NUM];   // 任务表
uint8        svcrt_task_count;                        // 当前任务数
int32        svcrt_current_task_id;                   // 当前任务编号（0=空闲，1..N=任务）
```

> 注意约定：`svcrt_current_task_id` **0 表示空闲（idle）**，1 到 N 表示第几个任务。这个 +1 偏移在调度代码里到处出现，记住它能少绕很多弯。

### 2.2 任务的状态机

任务有四种状态（[svcrt_task.h](file:///d:/项目文件/SVCRTOS/kernelsrc/include/svcrt_task.h)）：

```c
typedef enum {
    SVCRT_TASK_INVALID,   // 无效（栈溢出/被杀，永不调度）
    SVCRT_TASK_READY,     // 就绪（随时可以跑）
    SVCRT_TASK_WAIT,      // 等待（在睡觉，等时间到或被唤醒）
    SVCRT_TASK_RUNNING    // 正在跑（同一时刻只有一个）
} svcrt_task_status_t;
```

调度器永远从 **READY** 的任务里挑一个让它变 **RUNNING**。

### 2.3 准备一个"假现场"：栈帧初始化

先有鸡还是先有蛋的问题：切换是"恢复任务之前保存的现场"，但任务**第一次**运行时哪来的现场？

**办法**：手动在任务栈里**伪造一个现场**，让它看起来像"曾经跑过、刚被切走"。这样第一次切到它时，正常的恢复流程就能让它从入口函数开始跑。由 [svcrt_port.c](file:///d:/项目文件/SVCRTOS/kernelsrc/port/arm/cortex-m4/svcrt_port.c) 的 `svcrt_port_stack_init` 完成：

```c
uint32 svcrt_port_stack_init(uint32 stack_top, void (*entry)(void))
{
    uint32 *p_sp = (uint32 *)stack_top;
    /* 第一部分：硬件异常栈帧（退出 PendSV 时硬件自动弹出这 8 个） */
    *(--p_sp) = 0x01000000;        // xPSR：bit24=1 表示 Thumb 状态（必须！）
    *(--p_sp) = (uint32)entry;     // PC：任务入口 → 恢复后从这里开始跑
    *(--p_sp) = 0;                 // LR
    *(--p_sp) = 0;                 // R12
    *(--p_sp) = 0;                 // R3
    *(--p_sp) = 0;                 // R2
    *(--p_sp) = 0;                 // R1
    *(--p_sp) = 0;                 // R0
    /* 第二部分：我们手动保存/恢复的寄存器 R4-R11 + EXC_RETURN */
    *(--p_sp) = 0xFFFFFFFD;        // LR(EXC_RETURN)：返回 Thread 模式、用 PSP、无 FPU 帧
    *(--p_sp) = 0;                 // R11 ~ R4 全置 0
    *(--p_sp) = 0; *(--p_sp) = 0; *(--p_sp) = 0;
    *(--p_sp) = 0; *(--p_sp) = 0; *(--p_sp) = 0; *(--p_sp) = 0;
    return (uint32)p_sp;           // 返回伪造后的栈顶 → 存进 TCB.stack_ptr
}
```

**栈布局**（栈从高地址往低地址生长）：

```
高地址  ┌─────────────┐  ← stack_top
        │  xPSR       │  0x01000000 (Thumb 位)
        │  PC         │  entry      ← 关键！恢复后 PC 跳到这里
        │  LR R12 R3  │
        │  R2 R1 R0   │  这 8 个是「硬件异常栈帧」，退出 PendSV 时硬件自动弹出
        ├─────────────┤
        │  EXC_RETURN │  0xFFFFFFFD
        │  R11 ... R4 │  这 9 个由我们手动管理
低地址  └─────────────┘  ← stack_ptr（存进 TCB）
```

两个致命细节：

1. **xPSR 的 Thumb 位（0x01000000）必须置 1**。Cortex-M 只支持 Thumb 指令集，这一位是 0 会立刻 HardFault。新手最常踩。
2. **EXC_RETURN = 0xFFFFFFFD**：魔法值，告诉 CPU"异常返回后回到 Thread 模式、用 PSP、栈里无 FPU 帧"。

### 2.4 切换的核心：PendSV 汇编

切换动作发生在 **PendSV_Handler**，必须用汇编（要直接操作寄存器和栈）。完整代码在 [svcrt_context.S](file:///d:/项目文件/SVCRTOS/kernelsrc/port/arm/cortex-m4/svcrt_context.S)：

```asm
PendSV_Handler  PROC
    IMPORT  svcrt_sched_is_switching   ; C 函数：判断要不要切、切到谁
    IMPORT  svcrt_sched_activate       ; C 函数：保存旧任务、激活新任务
    PUSH    {R0, LR}                   ; R0、LR 先存到 MSP（中断栈）
    LDR     R2, =svcrt_sched_is_switching
    BLX     R2                         ; 返回值在 R0
    CMN     R0, #0
    BMI     endpend                    ; R0 < 0 表示不用切，直接退出
    MRS     R1, PSP                    ; R1 = 旧任务 PSP（硬件已压 8 个寄存器）
 IF {FPU} != "SoftVFP"
    TST     LR, #0x10                  ; EXC_RETURN bit4=0 表示用了 FPU
    IT      EQ
    VSTMDBEQ R1!, {S16-S31}            ; 用了 FPU 才存 S16-S31
 ENDIF
    STMDB   R1!, {R4-R11, LR}          ; ★ 手动保存 R4-R11 和 LR 到旧任务栈
    LDR     R2, =svcrt_sched_activate
    BLX     R2                         ; 入参(新任务号, 旧PSP)，返回新任务栈指针到 R0
    LDMIA   R0!, {R4-R11, LR}          ; ★ 从新任务栈恢复 R4-R11 和 LR
 IF {FPU} != "SoftVFP"
    TST     LR, #0x10
    IT      EQ
    VLDMIAEQ R0!, {S16-S31}
 ENDIF
    MSR     PSP, R0                    ; ★ PSP 指向新任务栈
endpend
    POP     {R0, PC}                   ; PC=之前压的 LR(EXC_RETURN) → 异常返回 → 跑新任务
    ENDP
```

**大白话总结**：① 问 C "该不该切、切给谁" ② 不切就走人 ③ 要切就把旧任务剩下的寄存器压进它自己的栈 ④ 问 C 要新任务的栈指针 ⑤ 从新任务栈恢复寄存器 ⑥ PSP 指向新任务栈、异常返回，硬件帮我们弹出剩下 8 个寄存器，CPU 就跳到新任务跑起来了。

> **为什么硬件存 8 个、我们存 9 个？** 进异常时硬件只自动存 R0-R3、R12、LR、PC、xPSR 这 8 个；R4-R11 得我们自己存；再加一个 LR(EXC_RETURN) 记住"用没用 FPU"，共 9 个。

### 2.5 切换的大脑：sched_activate

汇编调用的 C 函数 `svcrt_sched_activate`（[svcrt_task.c](file:///d:/项目文件/SVCRTOS/kernelsrc/src/svcrt_task.c)）负责**存旧的、激活新的**：

```c
int32 svcrt_sched_activate(int32 new_task, uint32 old_psp)
{
    int32 tid = svcrt_current_task_id - 1;          // 旧任务下标
    if(svcrt_current_task_id == 0)
        svcrt_idle_stack_ptr = old_psp;             // 旧的是空闲任务
    else
    {
        svcrt_task_table[tid].stack_ptr = old_psp;  // ★ 存旧任务栈指针
        if(svcrt_task_table[tid].status == SVCRT_TASK_RUNNING)
        {
            if(old_psp < (uint32)svcrt_task_table[tid].stack_bottom)
                svcrt_task_table[tid].status = SVCRT_TASK_INVALID;  // PSP 越界=栈溢出
            else
                svcrt_task_table[tid].status = SVCRT_TASK_READY;    // RUNNING 退回 READY
        }
        /* 旧任务若是 WAIT（主动睡了），这里什么都不做，保持 WAIT */
    }
    svcrt_current_task_id = new_task;
    if(svcrt_current_task_id > 0)
    {
        tid = svcrt_current_task_id - 1;
        svcrt_task_table[tid].touch_tick = svcrt_kernel_tick;
        svcrt_task_table[tid].status = SVCRT_TASK_RUNNING;          // 新任务变 RUNNING
        svcrt_port_mpu_set_app(svcrt_task_table[tid].mpu_bar,
                               svcrt_task_table[tid].mpu_asr);      // 切 MPU 权限
        return svcrt_task_table[tid].stack_ptr;                     // ★ 返回新任务栈指针
    }
    svcrt_current_task_id = 0;
    return svcrt_idle_stack_ptr;
}
```

> **这里藏着本项目调试最久的大坑**：栈溢出检测外层的 `if(status == RUNNING)` 条件**至关重要**。早期版本不分状态对所有任务都检测栈，把正常 WAIT 的任务误判为溢出标成 INVALID，导致它再也不被调度。详见第 10 章。

至此"两个任务轮流跑"的机制完整了。但还缺一问：`svcrt_sched_is_switching` 怎么决定"切给谁"？这就是下一章。

---

## 第 3 章 调度器：决定"该谁跑"

调度器（scheduler）回答一个问题：**所有就绪的任务里，下一个该让谁跑？**

### 3.1 调度策略：优先级 + 时间片轮转

SVCrtOS 用**抢占式优先级调度 + 同优先级时间片轮转**：

- **优先级**：数字越小优先级越高，永远优先跑最高优先级的就绪任务。
- **时间片轮转**：同优先级的任务轮流跑，谁等得最久谁先上（靠 `touch_tick` 字段判断，值越小越久没跑）。

### 3.2 挑选下一个任务：sched_next

`svcrt_sched_next`（[svcrt_task.c](file:///d:/项目文件/SVCRTOS/kernelsrc/src/svcrt_task.c)）遍历任务表挑出最该跑的：

```c
int32 svcrt_sched_next(void)
{
    uint8  tmp_pri = 255;            // 目前找到的最高优先级（255=最低）
    uint32 tmp_touch = 0xffffffff;
    uint8  idx;
    int32  r = -1;                   // 选中下标，-1=没有可跑的
    for(idx = 0; idx < svcrt_task_count; idx++)
    {
        svcrt_tick_tasks(&svcrt_task_table[idx]);   // 顺便更新计时（见第 4 章）
        if((svcrt_task_table[idx].status == SVCRT_TASK_READY) ||
           (svcrt_task_table[idx].status == SVCRT_TASK_RUNNING))
        {
            if(svcrt_task_table[idx].priority < tmp_pri)        // 更高优先级
            {
                tmp_pri = svcrt_task_table[idx].priority;
                tmp_touch = svcrt_task_table[idx].touch_tick;
                r = idx;
            }
            else if(svcrt_task_table[idx].priority == tmp_pri)  // 同优先级
            {
                if(svcrt_task_table[idx].touch_tick < tmp_touch)// 谁等得久选谁
                {
                    tmp_touch = svcrt_task_table[idx].touch_tick;
                    r = idx;
                }
            }
        }
    }
    return r;
}
```

逻辑朴素：扫一遍，记住"优先级最高、同级里等最久"的那个。

### 3.3 判断要不要切换：sched_is_switching

PendSV 汇编第一件事就是调它（[svcrt_task.c](file:///d:/项目文件/SVCRTOS/kernelsrc/src/svcrt_task.c)）：

```c
int32 svcrt_sched_is_switching(void)
{
    int32 new_idx = svcrt_sched_next() + 1;   // +1 转成 task_id
    if(new_idx == svcrt_current_task_id)      // 下一个还是当前这个？
    {
        if(new_idx > 0)
            svcrt_task_table[new_idx - 1].touch_tick = svcrt_kernel_tick;
        return -1;                            // -1：不用切换
    }
    return new_idx;                           // 需要切换
}
```

返回 -1 时 PendSV 的 `BMI endpend` 直接退出，避免"切给自己"的无用功。

> **完整切换链路**：
> ```
> 触发 → 挂起 PendSV → PendSV_Handler
>   → sched_is_switching（sched_next 挑人）
>   → 存旧任务寄存器 → sched_activate（存旧 TCB、激活新 TCB）
>   → 恢复新任务寄存器 → 异常返回 → 新任务运行
> ```

### 3.4 谁来触发切换

切换必须有人"挂起 PendSV"，两个触发源：
1. **时间到了**：SysTick 中断每 tick 调一次 `SVCRT_SWITCH_TASK()`（第 4 章）。
2. **任务主动让出**：调用 `svcrt_task_wait()` 等接口睡觉时主动触发。

`SVCRT_SWITCH_TASK()` 最终调用第 1 章的 `svcrt_port_switch_task()` → 挂起 PendSV。

---

## 第 4 章 时间管理：SysTick 心跳与延时

RTOS 需要"心跳"推动时间流逝、唤醒睡眠任务。心跳就是 **SysTick**——Cortex-M 内置的周期定时器。

### 4.1 心跳处理：kernel_tick_handler

SysTick 每隔固定时间（默认 500 微秒，`SVCRT_TICK_PERIOD_US` 配置）触发中断，板级中断入口调用内核的 `svcrt_kernel_tick_handler`（[svcrt_task.c](file:///d:/项目文件/SVCRTOS/kernelsrc/src/svcrt_task.c)）：

```c
void svcrt_kernel_tick_handler(void)
{
    svcrt_kernel_tick++;        // 全局时间戳 +1（系统的"现在几点"）
    SVCRT_SWITCH_TASK();        // 每 tick 尝试触发一次切换
    #if (SVCRT_USE_CPU_LOAD == 1)
    if(svcrt_current_task_id > 0)   // 在跑真实任务（非空闲）
    {
        svcrt_cpu_load_counter++;
        if((svcrt_kernel_tick & 0x3ff) == 0)
        {
            svcrt_cpu_idle_millis = svcrt_cpu_load_counter;
            svcrt_cpu_load_counter = 0;
        }
    }
    #endif
}
```

每 tick 都尝试切换，但若挑出的还是当前任务，`sched_is_switching` 返回 -1，PendSV 立刻退出，几乎零开销。

### 4.2 唤醒睡眠任务：tick_tasks（三种等待语义）

`svcrt_tick_tasks` 在每次 `sched_next` 遍历时给每个任务"走表"（[svcrt_task.c](file:///d:/项目文件/SVCRTOS/kernelsrc/src/svcrt_task.c)）：

```c
static void svcrt_tick_tasks(svcrt_task_t *p_task)
{
    uint32 used_tick = svcrt_kernel_tick;
    int32  escape_tick = (int32)(used_tick - p_task->tim_tick);
    if(p_task->status == SVCRT_TASK_INVALID) return;
    p_task->tim_tick = used_tick;
    if(escape_tick <= 0) return;
    if(p_task->status == SVCRT_TASK_RUNNING) return;
    if(p_task->wait_time < 0) return;        // 无限阻塞：只能被 post/unlock 唤醒

    if(p_task->wait_time > 0)                // ① 有限等待 svcrt_task_wait(ms)
    {
        p_task->wait_time -= escape_tick;
        if(p_task->wait_time <= 0)
        {
            p_task->wait_time = 0;
            if(p_task->status == SVCRT_TASK_WAIT)
                p_task->status = SVCRT_TASK_READY;
        }
        return;
    }
    p_task->period_time -= escape_tick;      // ② 周期等待 svcrt_task_wait_period
    if(p_task->period_time <= 0)
    {
        p_task->period_time += p_task->period;
        if(p_task->period_time <= 0) p_task->period_time = p_task->period;
        if(p_task->status == SVCRT_TASK_WAIT)
            p_task->status = SVCRT_TASK_READY;
    }
}
```

**三种等待语义**（初学者必须分清）：

| `wait_time` | 含义 | 唤醒方式 |
|-------------|------|----------|
| `> 0` | 有限等待（睡 N 毫秒） | 倒计时到 0 自动唤醒 |
| `== 0` | 周期等待 | 按 `period` 周期唤醒 |
| `< 0` | 无限阻塞 | 只能被 `post`/`unlock` 显式唤醒 |

> **第二个大坑**：早期把"信号量阻塞"也用 `wait_time == 0` 处理，被周期唤醒逻辑误处理，导致互斥锁竞争时唤醒错乱。引入 `wait_time < 0` 无限阻塞语义后解决。详见第 10 章。

### 4.3 任务主动睡觉：wait_internal

`svcrt_task_wait(ms)` 最终执行（[svcrt_task.c](file:///d:/项目文件/SVCRTOS/kernelsrc/src/svcrt_task.c)）：

```c
void svcrt_task_wait_internal(uint32 ms)
{
    svcrt_task_t *p_tsk;
    SVCRT_DISABLE_IRQ();                       // 关中断保护关键区
    if(svcrt_current_task_id > 0)
    {
        p_tsk = &svcrt_task_table[svcrt_current_task_id - 1];
        p_tsk->wait_time = SVCRT_MS_TO_TICK(ms);
        p_tsk->status = SVCRT_TASK_WAIT;
        SVCRT_SWITCH_TASK();                   // 主动让出 CPU
    }
    SVCRT_ENABLE_IRQ();
}
```

`SVCRT_DISABLE_IRQ`/`SVCRT_ENABLE_IRQ` 这对**关/开中断**保护"修改任务状态"这个关键区的原子性，否则中途被 SysTick 打断会出乱子——这是 RTOS 编程基本功。

---

## 第 5 章 系统调用：用 SVC 进入内核态

到这里，一个能跑多任务的 RTOS 内核已经成型。但 SVCrtOS 的招牌特性还没登场——**用户任务跑在非特权态，怎么调用内核功能？** 答案就是 SVC。

### 5.1 为什么需要"陷入"内核

回忆第 1 章：用户任务运行在**非特权态**，它不能直接改系统寄存器、不能直接访问内核数据结构。这是为了安全——一个跑飞的任务不能破坏整个系统。

但任务总得用系统功能吧（打开设备、睡眠、创建事件……）。这些功能的代码在内核里，需要特权。怎么让非特权的任务"借用"一下特权？

这就是 **SVC（Supervisor Call，监督调用）指令**的用途。它像一扇受控的门：用户执行 `SVC #n` 指令会触发一个异常，CPU 自动切到特权态，跳进 `SVC_Handler`。内核在这里检查请求、干完活、再返回用户态。用户全程碰不到内核内部，只能通过这扇门"提需求"。

> 这和 Linux 上的 `syscall` 是一个道理——用户程序通过软中断陷入内核。SVCrtOS 把这套机制搬到了单片机上。

### 5.2 SVC 号的分配

SVCrtOS 用不同的 SVC 号区分不同功能大类（[svcrt_def.h](file:///d:/项目文件/SVCRTOS/kernelsrc/include/svcrt_def.h)）：

```c
#define SVCRT_SVC_DEV_IO       (0x10)   // 设备 I/O（open/read/write/close/ctrl）
#define SVCRT_SVC_TASK_CTRL    (0x11)   // 任务控制（wait/delay/kill）
#define SVCRT_SVC_SYS_INFO     (0x12)   // 系统信息（时间/CPU 占用）
#define SVCRT_SVC_EVENT_CTRL   (0x13)   // 事件
#define SVCRT_SVC_DRV_MGR      (0x14)   // 驱动注册管理
#define SVCRT_SVC_SYNC_CTRL    (0x15)   // 信号量/互斥锁
```

### 5.3 SVC 入口汇编：取出参数

执行 `SVC #n` 后，CPU 进入 `SVC_Handler`（[svcrt_context.S](file:///d:/项目文件/SVCRTOS/kernelsrc/port/arm/cortex-m4/svcrt_context.S)）：

```asm
SVC_Handler  PROC
    EXPORT  SVC_Handler
    IMPORT  SVC_Server
    TST     LR, #4              ; 检查 EXC_RETURN bit2：判断进 SVC 前用的哪个栈
    ITE     EQ
    MRSEQ   R0, MSP             ; bit2=0：之前用 MSP（特权代码调的）
    MRSNE   R0, PSP             ; bit2=1：之前用 PSP（用户任务调的）
    B       SVC_Server          ; R0=栈帧地址，跳到 C 函数处理
    ENDP
```

**关键技巧**：进 SVC 异常时，硬件已经把调用现场的 8 个寄存器（R0-R3、R12、LR、PC、xPSR）压到了**调用者的栈**上。我们只要找到那个栈的地址（MSP 还是 PSP），就能读到调用时传的参数（在 R0-R3 里）和返回地址（PC）。这里把栈帧地址放进 R0 传给 C 函数。

### 5.4 SVC 分发：SVC_Server

C 函数 `SVC_Server`（[svcrt_task.c](file:///d:/项目文件/SVCRTOS/kernelsrc/src/svcrt_task.c)）是所有系统调用的总入口：

```c
void SVC_Server(svcrt_svc_context_t *p_svc_ctx)
{
    // p_svc_ctx 指向硬件压栈的现场，按结构体布局就能读到 r0~xpsr
    uint32 svc_num = ((char *)p_svc_ctx->pc)[-2];   // ★ 从指令里取出 SVC 号
    uint32 *p;

    switch(svc_num)
    {
    case SVCRT_SVC_TASK_CTRL:           // 0x11 任务控制
        switch(p_svc_ctx->r0)           // r0 是子功能号
        {
            case 1: svcrt_task_wait_internal(p_svc_ctx->r1); break;  // wait(ms)，ms 在 r1
            case 2: svcrt_task_wait_period_internal();        break;
            case 3: svcrt_task_delay_internal(p_svc_ctx->r1); break;
            case 4: svcrt_task_kill_internal();               break;
        }
        break;

    case SVCRT_SVC_DEV_IO:              // 0x10 设备 I/O
        p = (uint32 *)p_svc_ctx->r0;    // r0 指向参数数组
        switch(p[0])                    // p[0] 子功能号
        {
            case 1: p_svc_ctx->r0 = svcrt_dev_open_internal((char *)p[1], p[2]); break;
            case 2: p_svc_ctx->r0 = svcrt_dev_read_internal(p[1], (uint8 *)p[2], p[3]); break;
            // ... write/close/ctrl
        }
        break;
    // ... 其他 SVC 号
    }
}
```

两个精妙之处：

1. **怎么知道用户调的是哪个 SVC 号？** `SVC #n` 这条指令编码里就含 n。`p_svc_ctx->pc` 是 SVC 指令的下一条地址，往回退 2 字节（`[-2]`）正好是 SVC 指令的低字节，即 n。

2. **怎么传返回值？** 把结果写回 `p_svc_ctx->r0`。因为异常返回时硬件会把这个栈上的 r0 恢复到真正的 R0 寄存器，用户函数读 R0 就拿到了返回值。

### 5.5 用户侧：__svc 内联

用户调用 `svcrt_task_wait(100)` 时，这个函数其实是个 `__svc` 包装，编译器会生成 `SVC #0x11` 指令。用户完全感觉不到"陷入内核"的过程，用起来就像普通函数调用。这层封装在 SDK 的 oslib 里实现。

> **完整系统调用链路**：
> ```
> 用户：svcrt_task_wait(100)
>   → 编译器生成 SVC #0x11，参数进 R0/R1
>   → 硬件异常，切特权态，压栈现场，进 SVC_Handler
>   → 取栈帧地址 → SVC_Server
>   → 读 SVC 号 0x11、子功能、参数 → svcrt_task_wait_internal(100)
>   → 写返回值到栈帧 r0 → 异常返回 → 用户态拿到结果
> ```

---

## 第 6 章 内存保护：MPU 与特权隔离

SVC 解决了"用户怎么安全地调用内核"。MPU 解决另一半："怎么防止任务访问不该碰的内存"。

### 6.1 MPU 是什么

**MPU（Memory Protection Unit，内存保护单元）** 是 Cortex-M4/M7 里的一个硬件部件。它能把内存划分成若干"区域（region）"，给每个区域设访问权限（可读/可写/可执行、特权/非特权）。一旦代码访问了没权限的区域，立刻触发 HardFault。

在 RTOS 里，MPU 的价值是**任务隔离**：给每个任务只开放它自己的 ROM/RAM 区域。任务 A 跑飞了去写任务 B 的内存？MPU 直接拦下，A 崩溃但 B 安然无恙。

### 6.2 每个任务带一份 MPU 配置

回看第 2 章的 TCB，有两个字段：

```c
uint32 mpu_bar[8];   // 8 个区域的"基址寄存器"值
uint32 mpu_asr[8];   // 8 个区域的"属性和大小寄存器"值
```

每个任务都预先算好"我能访问哪些区域"，存成这两组寄存器快照。任务切换时（第 2 章 `svcrt_sched_activate` 里）有这么一行：

```c
svcrt_port_mpu_set_app(svcrt_task_table[tid].mpu_bar, svcrt_task_table[tid].mpu_asr);
```

它把新任务的 MPU 配置一次性写进硬件。于是切到哪个任务，MPU 就只放行哪个任务的内存——**内存权限随任务切换而切换**。

### 6.3 配置开关

MPU 和特权隔离不是必须的，由配置控制（`svcrt_config.h`）：

```c
#define SVCRT_USE_MPU    1   // 是否启用 MPU 内存保护
#define SVCRT_USE_PRIV   1   // 是否启用特权级分离（依赖 MPU）
```

- Cortex-M3 没有 MPU（或简化版），可以关掉，任务跑在特权态，没有隔离但更简单。
- M4/M7 推荐打开，获得完整的内存保护。

> **设计取舍**：开启 MPU 隔离会增加每次切换的开销（要写 MPU 寄存器），换来的是健壮性。对安全要求高的产品值得，对资源极度紧张的场景可以关闭。这就是 RTOS 配置化的意义——同一份内核，按需裁剪。

---

## 第 7 章 设备驱动框架

到目前为止，我们的 RTOS 能跑多任务、能系统调用、能内存保护。但任务怎么操作硬件（LED、串口、传感器）？总不能每个任务都去直接读写寄存器——那样既不安全（违反隔离），又没法复用。

SVCrtOS 借鉴 Linux 的思想：**一切皆设备**。所有硬件都封装成统一的 `open/close/read/write/ctrl` 五个接口。

### 7.1 驱动接口结构体

任何驱动都要实现这五个函数指针（组成 `svcrt_dev_drv_t`）：

```c
typedef struct {
    int32 (*drv_open)(uint32 param);
    int32 (*drv_close)(int32 handle);
    int32 (*drv_read)(int32 handle, uint8 *buf, int32 len);
    int32 (*drv_write)(int32 handle, uint8 *buf, int32 len);
    int32 (*drv_ctrl)(int32 handle, int32 code, int32 value);
} svcrt_dev_drv_t;
```

写一个 LED 驱动，就是填好这五个函数，然后注册。

### 7.2 设备注册：挂到设备表

内核维护一张设备表 `svcrt_dev_list`，每个表项记录"设备名 + 驱动接口"。注册函数 `svcrt_dev_register`（[svcrt_dev.c](file:///d:/项目文件/SVCRTOS/kernelsrc/src/svcrt_dev.c)）做三件事：查重名、拷贝名字、登记驱动指针：

```c
int32 svcrt_dev_register(const char *name, svcrt_dev_drv_t *drv, uint32 dev_num)
{
    if(name == 0 || drv == 0) return -2;
    if(svcrt_dev_count >= SVCRT_DEV_MAX_NUM) return -1;
    // ... 查重名（略）...
    // 拷贝设备名（最多 7 字符 + 结束符）
    for(i = 0; i < 7; i++) {
        svcrt_dev_list[svcrt_dev_count].dev_name[i] = name[i];
        if(name[i] == 0) break;
    }
    svcrt_dev_list[svcrt_dev_count].dev_name[7] = 0;
    svcrt_dev_list[svcrt_dev_count].drv = drv;          // ★ 登记驱动接口
    svcrt_dev_list[svcrt_dev_count].dev_num = dev_num;
    svcrt_dev_count++;
    return 0;
}
```

### 7.3 设备使用：通过名字找驱动

任务调用 `svcrt_dev_open("LED2", 0)` 时，内核拿名字去设备表里查，找到对应的 `drv`，调用它的 `drv_open`，返回一个句柄。后续 `read/write/ctrl` 都通过句柄定位到驱动。

> **这套设计的妙处**：
> - **解耦**：应用只认"LED2"这个名字，不关心它是哪个引脚、什么芯片。换硬件只改驱动，应用不动。
> - **安全**：应用通过 SVC + 设备框架间接操作硬件，配合 MPU 还能禁止应用直接碰外设寄存器。
> - **统一**：串口、SPI、LED……所有外设一套 API，学一次到处用。

---

## 第 8 章 任务间同步：事件、信号量、互斥锁

多个任务协作时，需要"同步"手段。比如任务 A 算完数据通知任务 B、两个任务抢同一个串口要排队。

### 8.1 三种同步原语

| 原语 | 用途 | 比喻 |
|------|------|------|
| **事件（event）** | 一个任务等另一个任务发信号 | 等红绿灯 |
| **信号量（semaphore）** | 控制对"有限个资源"的访问 | 停车场剩余车位 |
| **互斥锁（mutex）** | 保护"同一时刻只能一个人用"的资源 | 厕所门锁 |

### 8.2 互斥锁与优先级反转

互斥锁是最常用也最容易出问题的。SVCrtOS 的互斥锁带**优先级继承**，专门解决"优先级反转"问题。

**什么是优先级反转？** 设想：
- 低优先级任务 L 锁住了串口。
- 高优先级任务 H 也想用串口，被迫等 L 释放。
- 这时中优先级任务 M 跑出来抢占了 L……于是 H 实际上被 M "插队"了——高优先级反被中优先级拖死。

**优先级继承**的解法：当 H 等 L 持有的锁时，临时把 L 的优先级**提升到和 H 一样高**，让 L 赶紧跑完释放锁，M 就插不了队了。锁释放后 L 恢复原优先级。

SVCrtOS 在 [svcrt_sync.c](file:///d:/项目文件/SVCRTOS/kernelsrc/src/