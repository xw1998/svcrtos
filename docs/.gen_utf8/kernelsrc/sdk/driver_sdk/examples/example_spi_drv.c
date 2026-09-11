/**
* @brief SVCrtOS Driver SDK - ???SPI????
* @details ??о??????? SPI ????????????????
*          ???????????write ?????read ?????ctrl ???????/??/????
*          ???????????????????λ??????????о?????
*
* ??????????????? 0x0100 ???
*   SPI_CTRL_SET_SPEED  0x0100  ???? SCK ?????/???
*   SPI_CTRL_SET_CPOL_CPHA 0x0101  ????????????λ??mode 0~3??
*   SPI_CTRL_SET_CS     0x0102  ????????value: 0=??????Ч, 1=????????
*/

#include "svcrt_driver_sdk.h"

#define SPI_CTRL_SET_SPEED       (0x0100)
#define SPI_CTRL_SET_CPOL_CPHA   (0x0101)
#define SPI_CTRL_SET_CS          (0x0102)

#define SPI_DRV_MAX_INST   2

typedef struct {
    svcrt_dev_hdr_t hdr;
    uint32  spi_id;
    uint32  speed;          /* ?????????? */
    uint8   mode;           /* SPI ?? 0~3 */
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
    p->speed          = param ? param : 1000000;   /* ??? 1MHz */
    p->mode           = 0;
    p->hdr.block_size = 1;
    p->opened         = 1;

    /* TODO(???): ????? SPI ????
     * - ???? SCK/MOSI/MISO/CS GPIO
     * - ????????????????? p->speed???? p->mode
     * - ??? SPI */

    return (svcrt_dev_hdr_t *)p;
}

static int32 spi_drv_close(svcrt_dev_hdr_t *obj)
{
    spi_dev_obj_t *p = (spi_dev_obj_t *)obj;
    p->opened = 0;
    /* TODO(???): ??? SPI ???? */
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
        /* TODO(???): ???? dummy ????????????
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
        /* TODO(???): д?????????????????
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
        /* TODO(???): ???????? SPI ????? */
        return SVCRT_DRV_OK;
    case SPI_CTRL_SET_CPOL_CPHA:
        p->mode = (uint8)(value & 0x3);
        /* TODO(???): ???????? CPOL/CPHA */
        return SVCRT_DRV_OK;
    case SPI_CTRL_SET_CS:
        /* TODO(???): value=0 ????????value=1 ????? */
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
