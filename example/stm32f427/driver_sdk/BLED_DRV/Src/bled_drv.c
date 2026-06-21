/**
* @brief SVCrtOS Driver SDK 示例 - 蓝灯(PC2)用户态驱动
* @details 演示外部用户态驱动直接操作硬件：通过裸寄存器驱动 STM32F427 的
*          PC2（RGB 灯珠蓝色）。驱动注册设备名 "BLED"，外部 App 通过
*          svcrt_dev_open("BLED", 0) 即可使用。
*
*          编译为独立 .bin 固件，烧录到 ROM 分区，DrvMain 中通过 SVC 0x14
*          向内核注册驱动。
*
*          硬件说明：PC2 低电平点亮（共阳 RGB）。本工程 MPU=0，
*          用户态可直接访问外设寄存器；移植到 MPU 隔离环境时，
*          需通过内核态驱动或授予外设区域访问权限。
*
* 读写约定（与板级 LED 驱动一致）：
*   write len>0  -> 点亮
*   write len==0 -> 熄灭
*   write len<0  -> 翻转
*/

#include "svcrt_driver_sdk.h"

/* ---- STM32F427 裸寄存器地址（不依赖芯片头文件，保持 SDK 零依赖） ---- */
#define RCC_AHB1ENR     (*(volatile uint32 *)0x40023830u)
#define GPIOC_MODER     (*(volatile uint32 *)0x40020800u)
#define GPIOC_OTYPER    (*(volatile uint32 *)0x40020804u)
#define GPIOC_PUPDR     (*(volatile uint32 *)0x4002080Cu)
#define GPIOC_BSRR      (*(volatile uint32 *)0x40020818u)

#define GPIOC_EN_BIT    (1u << 2)        /* RCC AHB1ENR: GPIOC 时钟 */
#define BLUE_PIN        2                /* PC2 */

/* 自定义控制码 */
#define BLED_CTRL_TOGGLE   (0x0100)

typedef struct {
    svcrt_dev_hdr_t hdr;
    uint8   state;          /* 0=灭 1=亮 */
    uint8   opened;
} bled_dev_obj_t;

static bled_dev_obj_t bled_obj;

static void bled_set(uint8 on)
{
    if(on)
        GPIOC_BSRR = (1u << (16 + BLUE_PIN));   /* 复位 PC2 → 低电平 → 蓝灯亮 */
    else
        GPIOC_BSRR = (1u << BLUE_PIN);          /* 置位 PC2 → 高电平 → 蓝灯灭 */
}

static svcrt_dev_hdr_t *bled_drv_open(uint32 dev_id, uint32 param)
{
    (void)dev_id; (void)param;

    /* 使能 GPIOC 时钟 */
    RCC_AHB1ENR |= GPIOC_EN_BIT;

    /* PC2 配为通用推挽输出、无上下拉 */
    GPIOC_MODER  &= ~(0x3u << (BLUE_PIN * 2));
    GPIOC_MODER  |=  (0x1u << (BLUE_PIN * 2));   /* 01: 输出 */
    GPIOC_OTYPER &= ~(0x1u << BLUE_PIN);         /* 推挽 */
    GPIOC_PUPDR  &= ~(0x3u << (BLUE_PIN * 2));   /* 无上下拉 */

    bled_obj.state          = 0;
    bled_obj.opened         = 1;
    bled_obj.hdr.block_size = 1;
    bled_set(0);                                  /* 初始熄灭 */

    return (svcrt_dev_hdr_t *)&bled_obj;
}

static int32 bled_drv_close(svcrt_dev_hdr_t *obj)
{
    bled_dev_obj_t *p = (bled_dev_obj_t *)obj;
    bled_set(0);
    p->opened = 0;
    return SVCRT_DRV_OK;
}

static int32 bled_drv_read(svcrt_dev_hdr_t *obj, uint8 *pdata, int32 len)
{
    bled_dev_obj_t *p = (bled_dev_obj_t *)obj;
    if(pdata == 0 || len < 1)
        return SVCRT_DRV_INVALID_PARAM;
    if(!p->opened)
        return SVCRT_DRV_ERROR;
    pdata[0] = p->state;
    return 1;
}

static int32 bled_drv_write(svcrt_dev_hdr_t *obj, uint8 *pdata, int32 len)
{
    bled_dev_obj_t *p = (bled_dev_obj_t *)obj;
    (void)pdata;
    if(!p->opened)
        return SVCRT_DRV_ERROR;

    if(len > 0)
        p->state = 1;
    else if(len == 0)
        p->state = 0;
    else
        p->state = !p->state;

    bled_set(p->state);
    return (len < 0) ? 1 : len;
}

static int32 bled_drv_ctrl(svcrt_dev_hdr_t *obj, uint32 code, uint32 value)
{
    bled_dev_obj_t *p = (bled_dev_obj_t *)obj;
    (void)value;
    switch(code)
    {
    case BLED_CTRL_TOGGLE:
        p->state = !p->state;
        bled_set(p->state);
        return SVCRT_DRV_OK;
    case SVCRT_DEV_CTRL_GET_STATUS:
        return (int32)p->state;
    case SVCRT_DEV_CTRL_RESET:
        p->state = 0;
        bled_set(0);
        return SVCRT_DRV_OK;
    default:
        return SVCRT_DRV_ERROR;
    }
}

static svcrt_dev_drv_t bled_drv = {
    bled_drv_open,
    bled_drv_close,
    bled_drv_read,
    bled_drv_write,
    bled_drv_ctrl
};

void DrvMain(void)
{
    /* 向内核注册蓝灯设备，注册后外部 App 可 open("BLED") 使用。
     * 启动汇编经 __main 完成 .data 拷贝/.bss 清零，bled_drv 函数指针表已就绪。 */
    svcrt_drv_register("BLED", &bled_drv, 0);

    while(1)
    {
        svcrt_task_wait(1000);
    }
}
