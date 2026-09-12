# -*- coding: utf-8 -*-
"""
修复 14：超时语义统一 / 名字校验长度 / 裸镜像默认值 / Flash 扇区边界 / 定时器回调区

问题清单（每条都是静态可判定的事实，不含推测）：

1) 超时语义在 API 内部自相矛盾。
   svcrt.h 里 event 写的是“0 表示不等待，负值表示永久等待”，
   而 sem/mutex 写的是“非正值（<=0）表示永久等待”。同一套 API 出现两种约定，
   调用者按哪一条写都是错的。主流 RTOS（FreeRTOS 的 0 与 portMAX_DELAY、
   RT-Thread 的 0 与 RT_WAITING_FOREVER、Zephyr 的 K_NO_WAIT 与 K_FOREVER）
   统一采用“0=不等待、负值=永久等待”，因此这里统一到该约定。
   代码侧：sem/mtx/event 三个 internal 函数原先把 0 一路交给
   svcrt_task_block_in_critical（该原语把 <=0 当无限等待），行为上 0 等于永久阻塞。
   现在在计数/持有/置位检查之后插入 timeout==0 的提前返回（报超时），
   保证“0=不等待”真正成立。

2) 名字校验长度按对象写死为 8 字节，事件名实际是 16 字节。
   svcrt_kernel_user_name_ok 只校验 8 字节，但 svcrt_event_create_internal
   会把用户指针读到 15 字节（name[16]）。若用户把事件名放在可校验窗口的末尾，
   内核会越过窗口末尾读 7 个字节。改为把长度作为参数传入：
   事件用 16，其余（设备名/信号量/互斥锁/消息队列/定时器，字段均为 char[8]）用 8。

3) 裸镜像旁路默认开启，与“发布固件只接受 .svcapp”冲突。
   APP_ALLOW_RAW_IMAGE 默认 1，意味着任何烧到槽位里的非擦除内容都会被当可执行
   镜像启动（入口=分区基址），发布固件缺少这一层拒绝。改为默认 0 并加 #ifndef，
   由开发板板级配置显式打开，保留“固定地址烧录 + MDK 断点调试”的闭环。

4) Flash 扇区索引没有上界。
   svcrt_flash_sector_index 只挡 addr < FLASH_BASE 和第一个扇区之前的情况，
   对超出芯片 Flash 容量的地址会继续按公式算出 5~23 的“合法”扇区号并交给 HAL。
   本芯片 1MB 只有扇区 0~11，越界地址应当直接判非法。补上总容量上界检查。

5) 定时器回调只校验“不在内核 Flash”，允许跨区。
   SVC 定时器启动分支用 svcrt_kernel_ptr_flash_ok(p[4], 2) 校验回调，
   该函数放行整个用户 Flash 窗口（驱动池 + 全部 App 槽位 + 用户 RAM）。
   于是某个 App 可以把回调指向驱动池或另一个 App 的槽位，让内核特权态去执行
   别人安装的代码。改为要求回调落在调用者自身的固件区（TCB 的 rom_start/rom_size）。
"""
import os
import shutil

ROOT = r'D:\工作\git_project\svcrtos_new'
BK = r'C:\Users\14905\OneDrive\Documents\lingxi-claw\20260911-13-47-19-445\backup_fix5'

FILES = [
    'kernelsrc/include/svcrt.h',
    'kernelsrc/include/svcrt_task.h',
    'kernelsrc/src/svcrt_sync.c',
    'kernelsrc/src/svcrt_event.c',
    'kernelsrc/src/svcrt_task.c',
    'config/svcrt_partition.h',
    'board/stm32f427/svcrt_board_config.h',
    'board/stm32f427/drvflash.c',
    'example/stm32f427/app_sdk/APP_DEMO/Src/app_demo.c',
    'example/stm32f427/kernel/SVCRTOS_TEST/Core/Src/main.c',
    'kernelsrc/user_manual.md',
    'kernelsrc/sdk/sdk_user_manual.md',
    'docs/api/SVCrtOS_API参考.md',
]


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
        self.raw = open(self.p, 'rb').read()
        self.enc = detect_enc(self.raw)

    def sub(self, old, new, label, count=1, all=False):
        for eol in ('\r\n', '\n'):
            o = old.replace('\n', eol).encode(self.enc)
            n = new.replace('\n', eol).encode(self.enc)
            hit = self.raw.count(o)
            if all:
                if hit > 0:
                    self.raw = self.raw.replace(o, n)
                    print('  ok(%s)x%d: %s' % (repr(eol), hit, label))
                    return
            elif hit == count:
                self.raw = self.raw.replace(o, n)
                print('  ok(%s): %s' % (repr(eol), label))
                return
        raise AssertionError('[%s/%s] %s: 未找到匹配' % (self.rel, self.enc, label))

    def save(self):
        open(self.p, 'wb').write(self.raw)


# ---------------------------------------------------------------- 1. svcrt.h 文档
H_OLD = '* @param timeout 超时时间（ms），非正值（<=0）表示永久等待'
H_NEW = '* @param timeout 超时时间（ms），0 表示不等待，负值表示永久等待'

# ------------------------------------------------- 2. block_in_critical 文档澄清
BIC_OLD = """* @param timeout_ms >0 定时等待（ms）；<=0 无限等待（只能被显式唤醒）"""
BIC_NEW = """* @param timeout_ms >0 定时等待（ms）；<=0 无限等待（只能被显式唤醒）
* @note 这是内核内部原语，不直接对外。调用方（sem/mtx/mq/event）必须先把
*       对外 API 的“0=不等待、负值=永久等待”约定归一化后再调用，
*       不能把对外的 0 直接传进来（那会被本原语解释成无限等待）。"""

# --------------------------------------------------------- 3. sem_wait 提前返回
SEM_OLD = """    SVCRT_DISABLE_IRQ();
    if(svcrt_sems[idx].count > 0)
    {
        svcrt_sems[idx].count--;
        SVCRT_ENABLE_IRQ();
        return 0;
    }

    p_tsk = svcrt_task_get_current();"""

SEM_NEW = """    SVCRT_DISABLE_IRQ();
    if(svcrt_sems[idx].count > 0)
    {
        svcrt_sems[idx].count--;
        SVCRT_ENABLE_IRQ();
        return 0;
    }

    /* timeout==0 表示“只试一次、不等待”：计数为 0 立即报超时，
     * 不登记等待者、不阻塞（0 不再被解释成永久等待）。 */
    if(timeout_ms == 0)
    {
        SVCRT_ENABLE_IRQ();
        return SVCRT_SYNC_ERR_TIMEOUT;
    }

    p_tsk = svcrt_task_get_current();"""

# --------------------------------------------------------- 4. mtx_lock 提前返回
MTX_OLD = """    if(svcrt_mtxs[idx].owner == p_tsk)
    {
        /* 同一任务重复加锁，不支持递归，返回错误 */
        SVCRT_ENABLE_IRQ();
        return -1;
    }

    /* 优先级继承：若持有者优先级低于当前等待者，临时提升持有者优先级以避免优先级反转 */"""

MTX_NEW = """    if(svcrt_mtxs[idx].owner == p_tsk)
    {
        /* 同一任务重复加锁，不支持递归，返回错误 */
        SVCRT_ENABLE_IRQ();
        return -1;
    }

    /* timeout==0 表示“只试一次、不等待”：锁已被他人持有立即报超时。
     * 这里不登记等待者、也不做优先级继承——没建立等待关系就不该提权。 */
    if(timeout_ms == 0)
    {
        SVCRT_ENABLE_IRQ();
        return SVCRT_SYNC_ERR_TIMEOUT;
    }

    /* 优先级继承：若持有者优先级低于当前等待者，临时提升持有者优先级以避免优先级反转 */"""

# ------------------------------------------------------- 5. event_wait 提前返回
EV_OLD = """    /* 已经置位过：直接消费掉，不阻塞（否则“先 set 后 wait”会丢掉这次置位） */
    if(svcrt_events[idx].flag != 0u)
    {
        svcrt_events[idx].flag = 0u;
        SVCRT_ENABLE_IRQ();
        return 0;
    }

    p_tsk = svcrt_task_get_current();"""

EV_NEW = """    /* 已经置位过：直接消费掉，不阻塞（否则“先 set 后 wait”会丢掉这次置位） */
    if(svcrt_events[idx].flag != 0u)
    {
        svcrt_events[idx].flag = 0u;
        SVCRT_ENABLE_IRQ();
        return 0;
    }

    /* timeout==0 表示“只试一次、不等待”：当前未置位立即报超时，
     * 不登记等待者、不阻塞。 */
    if(timeout_ms == 0)
    {
        SVCRT_ENABLE_IRQ();
        return SVCRT_SYNC_ERR_TIMEOUT;
    }

    p_tsk = svcrt_task_get_current();"""

# ------------------------------------------- 6. 名字校验长度参数化 + 回调区校验
NAME_OLD = """/* 名字类参数（设备名/信号量名/消息队列名…）：内核最多读 8 字节（含 NUL），
 * 字符串常量通常在用户固件区，因此按“RAM 窗口 + 用户固件窗口”判定。 */
static uint8 svcrt_kernel_user_name_ok(const void *p)
{
    return svcrt_kernel_ptr_flash_ok(p, 8u);
}"""

NAME_NEW = """/* 名字类参数（设备名/信号量名/互斥锁名/消息队列名/定时器名/事件名）：
 * 内核最多读 len 字节（含 NUL），字符串常量通常在用户固件区，
 * 因此按“RAM 窗口 + 用户固件窗口”判定。
 * len 必须按目标对象名字段的真实大小给出：事件名是 char[16]（最多读 15 字节），
 * 其余对象是 char[8]。统一按 8 校验会让事件名有 7 个字节落在校验窗口之外。 */
static uint8 svcrt_kernel_user_name_ok(const void *p, uint32 len)
{
    return svcrt_kernel_ptr_flash_ok(p, len);
}

/* 定时器回调：必须落在调用者自身的固件区内。
 * 原判定只要求“不是内核 Flash”，于是某个 App 可以把回调指向驱动池或另一个
 * App 的槽位——等于借内核特权态去执行别人安装的代码。 */
static uint8 svcrt_kernel_cb_own_region_ok(const void *p)
{
    svcrt_task_t *p_tsk = svcrt_task_get_current();
    uint32 start;

    if(p == 0)
    {
        return 0u;
    }

    if((p_tsk != 0) && (p_tsk->rom_size != 0u))
    {
        start = (uint32)p;
        return ((start >= p_tsk->rom_start) &&
                (start < (p_tsk->rom_start + p_tsk->rom_size))) ? 1u : 0u;
    }

    /* 内核自身任务没有独立固件区（rom_size==0）：退回“用户可见代码区”判定 */
    return svcrt_kernel_ptr_flash_ok(p, 2u);
}"""

# 事件创建：长度 16
EVNAME_OLD = """                if(svcrt_kernel_user_name_ok((const void *)p[1]) == 0u)
                {
                    SVCRT_SVC_RET(p_svc_ctx, (uint32)(-1));
                    break;
                }
                SVCRT_SVC_RET(p_svc_ctx, svcrt_event_create_internal((char *)p[1]));"""

EVNAME_NEW = """                /* 事件名字段是 char[16]，校验窗口必须给到 16 字节 */
                if(svcrt_kernel_user_name_ok((const void *)p[1], 16u) == 0u)
                {
                    SVCRT_SVC_RET(p_svc_ctx, (uint32)(-1));
                    break;
                }
                SVCRT_SVC_RET(p_svc_ctx, svcrt_event_create_internal((char *)p[1]));"""

# 其余名字参数：长度 8（全部调用点一次替换）
NAME_CALL_OLD = 'svcrt_kernel_user_name_ok((const void *)p[1])'
NAME_CALL_NEW = 'svcrt_kernel_user_name_ok((const void *)p[1], 8u)'

# 定时器回调校验
CB_OLD = """            /* 回调会在内核特权态执行：函数指针与参数指针都必须位于用户可见区域 */
            if(svcrt_kernel_svc_args_ok(p, 24u) == 0u ||
               svcrt_kernel_ptr_flash_ok((const void *)p[4], 2u) == 0u ||
               svcrt_kernel_ptr_ram_ok((const void *)p[5], 1u) == 0u)"""

CB_NEW = """            /* 回调会在内核特权态执行：回调必须落在调用者自己的固件区内，
             * 参数指针必须位于用户可见 RAM */
            if(svcrt_kernel_svc_args_ok(p, 24u) == 0u ||
               svcrt_kernel_cb_own_region_ok((const void *)p[4]) == 0u ||
               svcrt_kernel_ptr_ram_ok((const void *)p[5], 1u) == 0u)"""

# ------------------------------------------- 7. 裸镜像开关默认值 + 板级打开
RAW_OLD = """#define APP_ALLOW_RAW_IMAGE     1                /* 1=允许槽位内直接烧录的裸镜像（开发调试用） */"""

RAW_NEW = """#ifndef APP_ALLOW_RAW_IMAGE
#define APP_ALLOW_RAW_IMAGE     0                /* 1=允许槽位内直接烧录的裸镜像（开发调试用） */
#endif
/* 默认 0：发布固件只接受带 CRC 的 .svcapp。
 * 开发板在 svcrt_board_config.h 里显式置 1 打开裸镜像旁路，
 * 以保留“固定地址烧录 + MDK 断点调试”的开发闭环。 */"""

BOARD_OLD = """#undef  SVCRT_USE_PRIV
#define SVCRT_USE_PRIV            0

#endif"""

BOARD_NEW = """#undef  SVCRT_USE_PRIV
#define SVCRT_USE_PRIV            0

/* 开发调试旁路：允许槽位内直接烧录的裸镜像（配合固定地址烧录 + MDK 断点调试）。
 * 发布固件时删掉本行即可回到“只接受带 CRC 的 .svcapp”的安全默认。 */
#define APP_ALLOW_RAW_IMAGE       1

#endif"""

# ------------------------------------------- 8. Flash 扇区索引上界
SECTOR_OLD = """    offset = addr - SVCRT_FLASH_BASE_ADDR;

    if(offset < (4u * SVCRT_FLASH_SMALL_SECTOR))"""

SECTOR_NEW = """    offset = addr - SVCRT_FLASH_BASE_ADDR;

    /* 上界：超出芯片 Flash 容量的地址一律判非法。
     * 不挡的话下面按公式会算出 5~23 的“合法”扇区号（1MB 芯片只到 11），
     * 越界扇区被交给 HAL 去擦。 */
    if(offset >= SVCRT_FLASH_TOTAL_SIZE)
    {
        return 0xFFFFFFFFu;
    }

    if(offset < (4u * SVCRT_FLASH_SMALL_SECTOR))"""

TOTAL_OLD = """#define SVCRT_FLASH_BIG_SECTOR    (128u * 1024u)    /* 扇区 5~11/23   */"""
TOTAL_NEW = """#define SVCRT_FLASH_BIG_SECTOR    (128u * 1024u)    /* 扇区 5~11/23   */
#define SVCRT_FLASH_TOTAL_SIZE    ((uint32)CHIP_FLASH_SIZE)  /* 片内 Flash 总容量 */"""

# ------------------------------------------------------- 9. 示例调用点 0 -> -1
DEMO_OLD = 'svcrt_mutex_lock(mtx, 0);'
DEMO_NEW = 'svcrt_mutex_lock(mtx, -1);    /* -1 = 永久等待（0 现在是“不等待”） */'

MAIN_OLD = 'svcrt_mtx_lock_internal(g_led_mutex, 0);'
MAIN_NEW = 'svcrt_mtx_lock_internal(g_led_mutex, -1);   /* -1 = 永久等待 */'

# ------------------------------------------------------------- 10. 手册示例
UM_OLD = '    svcrt_event_wait(evt, 0);'
UM_NEW = '    svcrt_event_wait(evt, -1);   /* -1 = 永久等待（0 表示不等待） */'

SDK1_OLD = '        svcrt_event_wait(evt, 0);    // 等待数据就绪'
SDK1_NEW = '        svcrt_event_wait(evt, -1);   // -1 = 永久等待数据就绪'

SDK2_OLD = '        svcrt_event_wait(evt, 0);'
SDK2_NEW = '        svcrt_event_wait(evt, -1);   // -1 = 永久等待'

API_OLD = '- `timeout`：超时时间（ms），负值表示永久等待'
API_NEW = '- `timeout`：超时时间（ms），0 表示不等待，负值表示永久等待'


def main():
    os.makedirs(BK, exist_ok=True)
    for rel in FILES:
        src = os.path.join(ROOT, rel.replace('/', os.sep))
        shutil.copy2(src, os.path.join(BK, rel.replace('/', '__')))
    print('备份完成 -> %s' % BK)

    d = Doc('kernelsrc/include/svcrt.h')
    d.sub(H_OLD, H_NEW, 'sem/mutex 超时语义文档统一', all=True)
    d.save()

    d = Doc('kernelsrc/include/svcrt_task.h')
    d.sub(BIC_OLD, BIC_NEW, 'block_in_critical 补充归一化约定说明')
    d.save()

    d = Doc('kernelsrc/src/svcrt_sync.c')
    d.sub(SEM_OLD, SEM_NEW, 'sem_wait: timeout==0 提前返回')
    d.sub(MTX_OLD, MTX_NEW, 'mtx_lock: timeout==0 提前返回')
    d.save()

    d = Doc('kernelsrc/src/svcrt_event.c')
    d.sub(EV_OLD, EV_NEW, 'event_wait: timeout==0 提前返回')
    d.save()

    d = Doc('kernelsrc/src/svcrt_task.c')
    d.sub(NAME_OLD, NAME_NEW, '名字校验长度参数化 + 新增回调区校验')
    d.sub(EVNAME_OLD, EVNAME_NEW, '事件创建名字校验给到 16 字节')
    d.sub(NAME_CALL_OLD, NAME_CALL_NEW, '其余名字校验统一 8 字节', all=True)
    d.sub(CB_OLD, CB_NEW, '定时器回调限定到调用者自身固件区')
    d.save()

    d = Doc('config/svcrt_partition.h')
    d.sub(RAW_OLD, RAW_NEW, '裸镜像开关默认关闭 + 可被板级覆盖')
    d.save()

    d = Doc('board/stm32f427/svcrt_board_config.h')
    d.sub(BOARD_OLD, BOARD_NEW, '开发板显式打开裸镜像旁路')
    d.save()

    d = Doc('board/stm32f427/drvflash.c')
    d.sub(TOTAL_OLD, TOTAL_NEW, '新增 Flash 总容量宏')
    d.sub(SECTOR_OLD, SECTOR_NEW, '扇区索引补上界检查')
    d.save()

    d = Doc('example/stm32f427/app_sdk/APP_DEMO/Src/app_demo.c')
    d.sub(DEMO_OLD, DEMO_NEW, 'app_demo: 互斥锁等待 0 -> -1', all=True)
    d.save()

    d = Doc('example/stm32f427/kernel/SVCRTOS_TEST/Core/Src/main.c')
    d.sub(MAIN_OLD, MAIN_NEW, 'main.c: 互斥锁等待 0 -> -1', all=True)
    d.save()

    d = Doc('kernelsrc/user_manual.md')
    d.sub(UM_OLD, UM_NEW, 'user_manual 事件等待示例')
    d.save()

    d = Doc('kernelsrc/sdk/sdk_user_manual.md')
    d.sub(SDK1_OLD, SDK1_NEW, 'sdk 手册示例一')
    d.sub(SDK2_OLD, SDK2_NEW, 'sdk 手册示例二')
    d.save()

    d = Doc('docs/api/SVCrtOS_API参考.md')
    d.sub(API_OLD, API_NEW, 'API 参考超时说明统一', all=True)
    d.save()

    print('OK')


if __name__ == '__main__':
    main()
