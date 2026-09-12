/* SVCrtOS Driver SDK - I2C example driver.
 *
 * Skeleton driver for an I2C master. Bus transactions are triggered through
 * ctrl(): the caller first selects the 7-bit slave address and the target
 * register address, then issues read()/write(). The register-level transfer
 * itself is left as TODO so this file can be copied as a template; every TODO
 * marks the exact place where the chip-specific code belongs.
 *
 * Control codes:
 *   I2C_CTRL_SET_ADDR    0x0100  set the 7-bit slave address
 *   I2C_CTRL_SET_REG     0x0101  set the register address used by read()/write()
 *   I2C_CTRL_SET_SPEED   0x0102  set the bus speed (100k, typically 400k)
 */

#include "svcrt_driver_sdk.h"

#define I2C_CTRL_SET_ADDR    (0x0100)
#define I2C_CTRL_SET_REG     (0x0101)
#define I2C_CTRL_SET_SPEED   (0x0102)

#define I2C_DRV_MAX_INST   2

typedef struct {
    svcrt_dev_hdr_t hdr;
    uint32  i2c_id;
    uint32  speed;          /* bus speed in Hz */
    uint8   slave_addr;     /* 7-bit slave address */
    uint8   reg_addr;       /* register address used by read()/write() */
    uint8   opened;
} i2c_dev_obj_t;

static i2c_dev_obj_t i2c_objs[I2C_DRV_MAX_INST];

static svcrt_dev_hdr_t *i2c_drv_open(uint32 dev_id, uint32 param)
{
    i2c_dev_obj_t *p;
    if(dev_id >= I2C_DRV_MAX_INST)
        return 0;

    p = &i2c_objs[dev_id];
    p->i2c_id         = dev_id;
    p->speed          = param ? param : 100000;   /* default 100 kHz */
    p->slave_addr     = 0;
    p->reg_addr       = 0;
    p->hdr.block_size = 1;
    p->opened         = 1;

    /* TODO(chip): bring up the I2C controller:
     * - configure the SCL/SDA pins and their alternate function
     * - derive the clock divider from p->speed
     * - enable the peripheral */

    return (svcrt_dev_hdr_t *)p;
}

static int32 i2c_drv_close(svcrt_dev_hdr_t *obj)
{
    i2c_dev_obj_t *p = (i2c_dev_obj_t *)obj;
    p->opened = 0;
    /* TODO(chip): release the I2C controller */
    return SVCRT_DRV_OK;
}

static int32 i2c_drv_read(svcrt_dev_hdr_t *obj, uint8 *pdata, int32 len)
{
    i2c_dev_obj_t *p = (i2c_dev_obj_t *)obj;

    if(pdata == 0 || len <= 0)
        return SVCRT_DRV_INVALID_PARAM;
    if(!p->opened)
        return SVCRT_DRV_ERROR;

    /* TODO(chip): full register read sequence:
     * - START + (slave_addr << 1 | W) + reg_addr
     * - repeated START + (slave_addr << 1 | R)
     * - read len bytes into pdata, NACK after the last byte
     * - STOP */
    (void)pdata;
    return len;
}

static int32 i2c_drv_write(svcrt_dev_hdr_t *obj, uint8 *pdata, int32 len)
{
    i2c_dev_obj_t *p = (i2c_dev_obj_t *)obj;

    if(pdata == 0 || len <= 0)
        return SVCRT_DRV_INVALID_PARAM;
    if(!p->opened)
        return SVCRT_DRV_ERROR;

    /* TODO(chip): full register write sequence:
     * - START + (slave_addr << 1 | W) + reg_addr
     * - write len bytes from pdata
     * - STOP */
    (void)pdata;
    return len;
}

static int32 i2c_drv_ctrl(svcrt_dev_hdr_t *obj, uint32 code, uint32 value)
{
    i2c_dev_obj_t *p = (i2c_dev_obj_t *)obj;
    switch(code)
    {
    case I2C_CTRL_SET_ADDR:
        p->slave_addr = (uint8)(value & 0x7F);
        return SVCRT_DRV_OK;
    case I2C_CTRL_SET_REG:
        p->reg_addr = (uint8)(value & 0xFF);
        return SVCRT_DRV_OK;
    case I2C_CTRL_SET_SPEED:
        p->speed = value;
        /* TODO(chip): reprogram the I2C clock divider for the new speed */
        return SVCRT_DRV_OK;
    case SVCRT_DEV_CTRL_GET_STATUS:
        return (int32)p->slave_addr;
    default:
        return SVCRT_DRV_ERROR;
    }
}

static svcrt_dev_drv_t i2c_drv = {
    i2c_drv_open,
    i2c_drv_close,
    i2c_drv_read,
    i2c_drv_write,
    i2c_drv_ctrl
};

int32 i2c_drv_install(void)
{
    return svcrt_drv_register("I2C1", &i2c_drv, 0);
}
