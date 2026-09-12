# -*- coding: utf-8 -*-
"""
修复 3：load_buffer 写 Flash 失败未回置槽位状态
修复 4：SysTick 优先级被 SysTick_Config() 覆盖回最低
修复 5：dev_unregister 前移数组导致已发放句柄指向别的设备

问题3：svcrt_loader_load_buffer() 擦除失败会回置 EMPTY，写入失败却直接 return，
       槽位会永远停在 INSTALLING（该函数当前无调用者，但语义必须一致）。
问题4：svcrt_port_start_timer() 用 SysTick_Config()，该函数会把 SysTick 优先级
       写成最低值，覆盖移植层 irq_init 设定的“SysTick 最高、PendSV 最低”策略，
       结果 tick 可能被其它中断长期压制，任务节拍/超时全部失真。
问题5：svcrt_dev_unregister() 把后续元素整体前移，而设备句柄就是槽位下标
       （i | SVCRT_DEV_HANDLE_FLAG），前移会让已发放的句柄指向另一个设备。
       改为墓碑式清空（空槽位以 dev_name[0]==0 标记），注册时复用空槽位。
"""
import os
import shutil

ROOT = r'D:\工作\git_project\svcrtos_new'
BK = r'C:\Users\14905\OneDrive\Documents\lingxi-claw\20260911-13-47-19-445\backup_fix2'


class Doc(object):
    def __init__(self, rel, enc):
        self.rel = rel
        self.enc = enc
        self.p = os.path.join(ROOT, rel.replace('/', os.sep))
        self.raw = open(self.p, 'rb').read()
        self.nl = b'\r\n' if b'\r\n' in self.raw else b'\n'

    def t(self, s):
        s = s.replace('\n', '\r\n') if self.nl == b'\r\n' else s
        return s.encode(self.enc)

    def sub(self, old, new, label, count=1):
        ob, nb = self.t(old), self.t(new)
        n = self.raw.count(ob)
        assert n == count, '[%s] %s: 期望 %d 次，实际 %d 次' % (self.rel, label, count, n)
        self.raw = self.raw.replace(ob, nb)
        print('  ok:', label)

    def save(self):
        open(self.p, 'wb').write(self.raw)


FILES = ['kernelsrc/src/svcrt_loader.c',
         'kernelsrc/port/arm/cortex-m4/svcrt_port.c',
         'kernelsrc/port/arm/cortex-m3/svcrt_port.c',
         'kernelsrc/src/svcrt_dev.c']


def do_backup():
    os.makedirs(BK, exist_ok=True)
    for rel in FILES:
        shutil.copy2(os.path.join(ROOT, rel.replace('/', os.sep)),
                     os.path.join(BK, rel.replace('/', '__')))


LD_OLD = """    if(svcrt_port_flash_write(slot_base, image, total) != 0)
    {
        return SVCRT_LOADER_ERR_FLASH;
    }"""

LD_NEW = """    if(svcrt_port_flash_write(slot_base, image, total) != 0)
    {
        /* 与擦除失败同样处理：写入失败必须回置 EMPTY，
         * 否则槽位会永远停在 INSTALLING，既不能启动也不会被回收。 */
        svcrt_ptable_set_slot(slot, SVCRT_APP_SLOT_EMPTY, 0u, 0u);
        return SVCRT_LOADER_ERR_FLASH;
    }"""

PORT_OLD = """void svcrt_port_start_timer(uint32 tick_period_us)
{
    uint32 ticks = (uint32)(((unsigned long long)SystemCoreClock * tick_period_us) / 1000000u);
    SysTick_Config(ticks);
}"""

PORT_NEW = """void svcrt_port_start_timer(uint32 tick_period_us)
{
    uint32 ticks = (uint32)(((unsigned long long)SystemCoreClock * tick_period_us) / 1000000u);

    if(ticks == 0u)
    {
        ticks = 1u;
    }

    /* 不能使用 SysTick_Config()：它会把 SysTick 优先级写成最低值，
     * 覆盖本移植层 svcrt_port_irq_init() 设定的“SysTick 最高、PendSV 最低”
     * 策略。优先级一旦被压低，tick 就可能被其它中断长时间压制，
     * 任务节拍与所有超时全部失真。这里只配置计数与使能，
     * 优先级统一由 svcrt_port_irq_init() 设定。 */
    SysTick->LOAD = ticks - 1u;
    SysTick->VAL  = 0u;
    SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk |
                    SysTick_CTRL_TICKINT_Msk   |
                    SysTick_CTRL_ENABLE_Msk;
}"""

DEV_REG_OLD = """int32 svcrt_dev_register(const char *name, svcrt_dev_drv_t *drv, uint32 dev_num)
{
    int32 i;
    if(name == 0 || drv == 0)
        return -2;

    if(svcrt_dev_count >= SVCRT_DEV_MAX_NUM)
        return -1;

    for(i = 0; i < svcrt_dev_count; i++)
    {
        int32 j;
        uint8 match = 1;
        for(j = 0; j < 8; j++)
        {
            if(svcrt_dev_list[i].dev_name[j] != name[j])
            {
                match = 0;
                break;
            }
            if(name[j] == 0)
                break;
        }
        if(match)
            return -2;
    }

    for(i = 0; i < 7; i++)
    {
        svcrt_dev_list[svcrt_dev_count].dev_name[i] = name[i];
        if(name[i] == 0)
            break;
    }
    svcrt_dev_list[svcrt_dev_count].dev_name[7] = 0;
    svcrt_dev_list[svcrt_dev_count].drv = drv;
    svcrt_dev_list[svcrt_dev_count].dev_num = dev_num;
    svcrt_dev_count++;
    return 0;
}"""

DEV_REG_NEW = """int32 svcrt_dev_register(const char *name, svcrt_dev_drv_t *drv, uint32 dev_num)
{
    int32 i;
    int32 slot = -1;

    if(name == 0 || drv == 0)
        return -2;

    /* 空槽位以 dev_name[0]==0 标记。注销设备时只清空槽位、不移动数组元素：
     * 设备句柄就是槽位下标（i | SVCRT_DEV_HANDLE_FLAG），一旦移动，
     * 已发放的句柄就会指向另一个设备。 */
    for(i = 0; i < SVCRT_DEV_MAX_NUM; i++)
    {
        int32 j;
        uint8 match = 1;

        if(svcrt_dev_list[i].dev_name[0] == 0)
        {
            if(slot < 0 && svcrt_dev_handles[i] == 0)
            {
                slot = i;
            }
            continue;
        }

        for(j = 0; j < 8; j++)
        {
            if(svcrt_dev_list[i].dev_name[j] != name[j])
            {
                match = 0;
                break;
            }
            if(name[j] == 0)
                break;
        }
        if(match)
            return -2;
    }

    if(slot < 0)
        return -1;

    for(i = 0; i < 7; i++)
    {
        svcrt_dev_list[slot].dev_name[i] = name[i];
        if(name[i] == 0)
            break;
    }
    svcrt_dev_list[slot].dev_name[7] = 0;
    svcrt_dev_list[slot].drv = drv;
    svcrt_dev_list[slot].dev_num = dev_num;
    svcrt_dev_count++;
    return 0;
}"""

DEV_UNREG_OLD = """int32 svcrt_dev_unregister(const char *name)
{
    int32 i, j;
    if(name == 0)
        return -1;

    for(i = 0; i < svcrt_dev_count; i++)
    {
        uint8 match = 1;
        for(j = 0; j < 8; j++)
        {
            if(svcrt_dev_list[i].dev_name[j] != name[j])
            {
                match = 0;
                break;
            }
            if(name[j] == 0)
                break;
        }
        if(match)
        {
            for(j = i; j < svcrt_dev_count - 1; j++)
            {
                svcrt_dev_list[j] = svcrt_dev_list[j + 1];
                svcrt_dev_handles[j] = svcrt_dev_handles[j + 1];
            }
            svcrt_dev_list[svcrt_dev_count - 1].dev_name[0] = 0;
            svcrt_dev_list[svcrt_dev_count - 1].drv = 0;
            svcrt_dev_list[svcrt_dev_count - 1].dev_num = 0;
            svcrt_dev_handles[svcrt_dev_count - 1] = 0;
            svcrt_dev_count--;
            return 0;
        }
    }
    return -1;
}"""

DEV_UNREG_NEW = """int32 svcrt_dev_unregister(const char *name)
{
    int32 i, j;
    if(name == 0)
        return -1;

    for(i = 0; i < SVCRT_DEV_MAX_NUM; i++)
    {
        uint8 match = 1;

        if(svcrt_dev_list[i].dev_name[0] == 0)
        {
            continue;                       /* 空槽位 */
        }

        for(j = 0; j < 8; j++)
        {
            if(svcrt_dev_list[i].dev_name[j] != name[j])
            {
                match = 0;
                break;
            }
            if(name[j] == 0)
                break;
        }
        if(match)
        {
            /* 墓碑式清空：保持槽位下标不变，已发放的句柄仍指向原槽位，
             * 此时该槽位 drv==0 / handles==0，后续读写会安全地返回 -1，
             * 而不是（像整体前移那样）指向另一个设备。 */
            svcrt_dev_list[i].dev_name[0] = 0;
            svcrt_dev_list[i].drv         = 0;
            svcrt_dev_list[i].dev_num     = 0;
            svcrt_dev_handles[i]          = 0;
            if(svcrt_dev_count > 0)
            {
                svcrt_dev_count--;
            }
            return 0;
        }
    }
    return -1;
}"""


def patch_dev():
    d = Doc('kernelsrc/src/svcrt_dev.c', 'gbk')
    d.sub(DEV_REG_OLD, DEV_REG_NEW, 'register 复用空槽位')
    d.sub(DEV_UNREG_OLD, DEV_UNREG_NEW, 'unregister 墓碑式清空')
    d.sub('    for(i = 0; i < svcrt_dev_count; i++)\n    {\n        int32 j;\n        uint8 match = 1;\n        for(j = 0; j < 8; j++)\n        {\n            if(svcrt_dev_list[i].dev_name[j] != name[j])\n            {\n                match = 0;\n                break;\n            }\n            if(name[j] == 0)\n                break;\n        }\n        if(match && svcrt_dev_list[i].drv != 0 && svcrt_dev_list[i].drv->drv_open != 0)',
          '    /* 槽位可能不连续（注销留下的空槽位），必须遍历整个表 */\n    for(i = 0; i < SVCRT_DEV_MAX_NUM; i++)\n    {\n        int32 j;\n        uint8 match = 1;\n        if(svcrt_dev_list[i].dev_name[0] == 0)\n        {\n            continue;\n        }\n        for(j = 0; j < 8; j++)\n        {\n            if(svcrt_dev_list[i].dev_name[j] != name[j])\n            {\n                match = 0;\n                break;\n            }\n            if(name[j] == 0)\n                break;\n        }\n        if(match && svcrt_dev_list[i].drv != 0 && svcrt_dev_list[i].drv->drv_open != 0)',
          'open 遍历整表并跳过空槽位')
    d.sub('    if(idx >= svcrt_dev_count)\n        return -1;',
          '    /* 句柄是槽位下标：合法范围是整个设备表，而不是当前在用数量 */\n'
          '    if(idx < 0 || idx >= SVCRT_DEV_MAX_NUM)\n        return -1;',
          'close/read/write/ctrl 边界检查', count=4)
    d.save()


def main():
    do_backup()
    print('--- svcrt_loader.c ---')
    d = Doc('kernelsrc/src/svcrt_loader.c', 'utf-8')
    d.sub(LD_OLD, LD_NEW, 'load_buffer 写失败回置 EMPTY')
    d.save()

    for arch in ('cortex-m4', 'cortex-m3'):
        print('--- port/%s ---' % arch)
        d = Doc('kernelsrc/port/arm/%s/svcrt_port.c' % arch, 'gbk')
        d.sub(PORT_OLD, PORT_NEW, 'start_timer 不再覆盖 SysTick 优先级')
        d.save()

    print('--- svcrt_dev.c ---')
    patch_dev()
    print('OK')


if __name__ == '__main__':
    main()
