/* SVCrtOS Driver SDK - SPI example driver.
 *
 * Skeleton driver for an SPI master. write() shifts data out, read() clocks in
 * dummy bytes, ctrl() sets the clock speed, the CPOL/CPHA mode and the chip
 * select line. Every TODO marks the place where the chip-specific code belongs.
 *
 * Control codes:
 *   SPI_CTRL_SET_SPEED      0x0100  set the SCK frequency
 *   SPI_CTRL_SET_CPOL_CPHA  0x0101  set CPOL/CPHA, mode 0..3
 *   SPI_CTRL_SET_CS         0x0102  drive CS, value: 0=assert, 1=deassert
 */

#include "svcrt_driver_sdk.h"

#define SPI_CTRL_SET_SPEED       (0x0100)
#define SPI_CTRL_SET_CPOL_CPHA   (0x0101)
#define SPI_CTRL_SET_CS          (0x0102)

#define SPI_DRV_MAX_INST   2

typedef struct {
    svcrt_dev_hdr_t hdr;
    uint32  spi_id;
    uint32  speed;          /* SCK frequency in Hz */
    uint8   mode;           /* SPI mode 0..3 */
    uint8   opened;
} spi_dev_obj_t;

static spi_dev_obj_t spi_objs[SPI_DRV_MAX_INST];

static svcrt_dev_hdr_t *spi_drv_open(uint32 dev_id, uint32 param)
{
    spi_dev_obj_t *p;
    if(dev_id >= SPI_DRV_MAX_INST)
        return 0;

    p = &spi_objs[dev_id];
    p->spi_id         = dev_id;
    p->speed          = param ? param : 1000000;   /* default 1 MHz */
    p->mode           = 0;
    p->hdr.block_size = 1;
    p->opened         = 1;

    /* TODO(chip): bring up the SPI controller:
     * - configure the SCK/MOSI/MISO/CS pins
     * - derive the prescaler from p->speed and p->mode
     * - enable the peripheral */

    return (svcrt_dev_hdr_t *)p;
}

static int32 spi_drv_close(svcrt_dev_hdr_t *obj)
{
    spi_dev_obj_t *p = (spi_dev_obj_t *)obj;
    p->opened = 0;
    /* TODO(chip): release the SPI controller */
    return SVCRT_DRV_OK;
}

static int32 spi_drv_read(svcrt_dev_hdr_t *obj, uint8 *pdata, int32 len)
{
    spi_dev_obj_t *p = (spi_dev_obj_t *)obj;
    int32 cnt;

    if(pdata == 0 || len <= 0)
        return SVCRT_DRV_INVALID_PARAM;
    if(!p->opened)
        return SVCRT_DRV_ERROR;

    for(cnt = 0; cnt < len; cnt++)
    {
        /* TODO(chip): clock in one byte by sending a dummy word:
         * SPIx->DR = 0xFF; while(!(SPIx->SR & RXNE)); pdata[cnt] = SPIx->DR; */
        pdata[cnt] = 0;
    }
    return len;
}

static int32 spi_drv_write(svcrt_dev_hdr_t *obj, uint8 *pdata, int32 len)
{
    spi_dev_obj_t *p = (spi_dev_obj_t *)obj;
    int32 cnt;

    if(pdata == 0 || len <= 0)
        return SVCRT_DRV_INVALID_PARAM;
    if(!p->opened)
        return SVCRT_DRV_ERROR;

    for(cnt = 0; cnt < len; cnt++)
    {
        /* TODO(chip): shift out one byte and wait for the TX buffer to drain:
         * while(!(SPIx->SR & TXE)); SPIx->DR = pdata[cnt]; */
        (void)pdata;
    }
    return len;
}

static int32 spi_drv_ctrl(svcrt_dev_hdr_t *obj, uint32 code, uint32 value)
{
    spi_dev_obj_t *p = (spi_dev_obj_t *)obj;
    switch(code)
    {
    case SPI_CTRL_SET_SPEED:
        p->speed = value;
        /* TODO(chip): reprogram the SPI prescaler for the new speed */
        return SVCRT_DRV_OK;
    case SPI_CTRL_SET_CPOL_CPHA:
        p->mode = (uint8)(value & 0x3);
        /* TODO(chip): apply the new CPOL/CPHA configuration */
        return SVCRT_DRV_OK;
    case SPI_CTRL_SET_CS:
        /* TODO(chip): value=0 asserts CS, value=1 deasserts it */
        (void)value;
        return SVCRT_DRV_OK;
    case SVCRT_DEV_CTRL_GET_STATUS:
        return (int32)p->speed;
    default:
        return SVCRT_DRV_ERROR;
    }
}

static svcrt_dev_drv_t spi_drv = {
    spi_drv_open,
    spi_drv_close,
    spi_drv_read,
    spi_drv_write,
    spi_drv_ctrl
};

int32 spi_drv_install(void)
{
    return svcrt_drv_register("SPI1", &spi_drv, 0);
}
