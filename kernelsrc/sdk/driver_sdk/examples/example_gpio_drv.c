/* SVCrtOS Driver SDK - GPIO example driver.
 *
 * Skeleton driver for a single GPIO pin. open() selects direction, write()
 * drives the pin, read() samples it and ctrl() changes direction, pull mode or
 * toggles the pin. Every TODO marks the place where the chip-specific code
 * belongs.
 *
 * Control codes:
 *   GPIO_CTRL_SET_DIR    0x0100  set direction, value: 0=input, 1=output
 *   GPIO_CTRL_SET_PULL   0x0101  set pull mode, value: 0=none, 1=up, 2=down
 *   GPIO_CTRL_TOGGLE     0x0102  toggle the output level
 */

#include "svcrt_driver_sdk.h"

#define GPIO_CTRL_SET_DIR    (0x0100)
#define GPIO_CTRL_SET_PULL   (0x0101)
#define GPIO_CTRL_TOGGLE     (0x0102)

#define GPIO_DRV_MAX_INST   8

typedef struct {
    svcrt_dev_hdr_t hdr;
    uint32  pin_id;         /* pin identifier: port index + pin index */
    uint8   dir;            /* 0=input, 1=output */
    uint8   level;          /* last known output level */
    uint8   opened;
} gpio_dev_obj_t;

static gpio_dev_obj_t gpio_objs[GPIO_DRV_MAX_INST];

static svcrt_dev_hdr_t *gpio_drv_open(uint32 dev_id, uint32 param)
{
    gpio_dev_obj_t *p;
    if(dev_id >= GPIO_DRV_MAX_INST)
        return 0;

    p = &gpio_objs[dev_id];
    p->pin_id         = dev_id;
    p->dir            = (uint8)(param & 0x1);   /* param bit0: direction */
    p->level          = 0;
    p->hdr.block_size = 1;
    p->opened         = 1;

    /* TODO(chip): bring up the pin:
     * - enable the port clock
     * - apply p->dir and switch the pin to input or output mode */

    return (svcrt_dev_hdr_t *)p;
}

static int32 gpio_drv_close(svcrt_dev_hdr_t *obj)
{
    gpio_dev_obj_t *p = (gpio_dev_obj_t *)obj;
    p->opened = 0;
    /* TODO(chip): put the pin back to its reset state */
    return SVCRT_DRV_OK;
}

static int32 gpio_drv_read(svcrt_dev_hdr_t *obj, uint8 *pdata, int32 len)
{
    gpio_dev_obj_t *p = (gpio_dev_obj_t *)obj;

    if(pdata == 0 || len < 1)
        return SVCRT_DRV_INVALID_PARAM;
    if(!p->opened)
        return SVCRT_DRV_ERROR;

    /* TODO(chip): sample the input level:
     * pdata[0] = (GPIOx->IDR >> pin) & 0x1; */
    pdata[0] = p->level;
    return 1;
}

static int32 gpio_drv_write(svcrt_dev_hdr_t *obj, uint8 *pdata, int32 len)
{
    gpio_dev_obj_t *p = (gpio_dev_obj_t *)obj;

    if(pdata == 0 || len < 1)
        return SVCRT_DRV_INVALID_PARAM;
    if(!p->opened || p->dir != 1)
        return SVCRT_DRV_ERROR;

    p->level = pdata[0] ? 1 : 0;
    /* TODO(chip): drive the output:
     * if(p->level) GPIOx->BSRR = pin; else GPIOx->BSRR = pin << 16; */
    return 1;
}

static int32 gpio_drv_ctrl(svcrt_dev_hdr_t *obj, uint32 code, uint32 value)
{
    gpio_dev_obj_t *p = (gpio_dev_obj_t *)obj;
    switch(code)
    {
    case GPIO_CTRL_SET_DIR:
        p->dir = (uint8)(value & 0x1);
        /* TODO(chip): reprogram the pin direction */
        return SVCRT_DRV_OK;
    case GPIO_CTRL_SET_PULL:
        /* TODO(chip): apply the pull mode encoded in value (0/1/2) */
        (void)value;
        return SVCRT_DRV_OK;
    case GPIO_CTRL_TOGGLE:
        p->level = !p->level;
        /* TODO(chip): toggle the pin output */
        return SVCRT_DRV_OK;
    case SVCRT_DEV_CTRL_GET_STATUS:
        return (int32)p->level;
    default:
        return SVCRT_DRV_ERROR;
    }
}

static svcrt_dev_drv_t gpio_drv = {
    gpio_drv_open,
    gpio_drv_close,
    gpio_drv_read,
    gpio_drv_write,
    gpio_drv_ctrl
};

int32 gpio_drv_install(void)
{
    return svcrt_drv_register("GPIO", &gpio_drv, 0);
}
