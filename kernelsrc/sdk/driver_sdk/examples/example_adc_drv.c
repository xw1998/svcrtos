/* SVCrtOS Driver SDK - ADC example driver.
 *
 * Skeleton driver for a single-channel ADC. read() returns little-endian
 * 16-bit samples, 2 bytes per sample, so a 12/16-bit conversion fits without
 * padding. ctrl() selects the channel, the sample time and the start trigger.
 * Every TODO marks the place where the chip-specific code belongs.
 *
 * Control codes:
 *   ADC_CTRL_SET_CHANNEL    0x0100  select the input channel
 *   ADC_CTRL_SET_SAMPLETIME 0x0101  set the sampling time
 *   ADC_CTRL_START          0x0102  start one conversion, return the result
 */

#include "svcrt_driver_sdk.h"

#define ADC_CTRL_SET_CHANNEL     (0x0100)
#define ADC_CTRL_SET_SAMPLETIME  (0x0101)
#define ADC_CTRL_START           (0x0102)

#define ADC_DRV_MAX_INST   1

typedef struct {
    svcrt_dev_hdr_t hdr;
    uint32  adc_id;
    uint8   channel;        /* currently selected channel */
    uint8   opened;
    uint16  last_value;     /* last converted value */
} adc_dev_obj_t;

static adc_dev_obj_t adc_objs[ADC_DRV_MAX_INST];

static svcrt_dev_hdr_t *adc_drv_open(uint32 dev_id, uint32 param)
{
    adc_dev_obj_t *p;
    if(dev_id >= ADC_DRV_MAX_INST)
        return 0;

    p = &adc_objs[dev_id];
    p->adc_id         = dev_id;
    p->channel        = (uint8)param;
    p->last_value     = 0;
    p->hdr.block_size = 2;       /* one sample is 2 bytes (12/16-bit result) */
    p->opened         = 1;

    /* TODO(chip): bring up the ADC:
     * - enable the ADC clock and its reference/regulator
     * - configure the pin of p->channel as analog input
     * - set the sample time for the first conversion */

    return (svcrt_dev_hdr_t *)p;
}

static int32 adc_drv_close(svcrt_dev_hdr_t *obj)
{
    adc_dev_obj_t *p = (adc_dev_obj_t *)obj;
    p->opened = 0;
    /* TODO(chip): release the ADC */
    return SVCRT_DRV_OK;
}

static int32 adc_drv_read(svcrt_dev_hdr_t *obj, uint8 *pdata, int32 len)
{
    adc_dev_obj_t *p = (adc_dev_obj_t *)obj;
    int32 cnt = 0;

    if(pdata == 0 || len < 2)
        return SVCRT_DRV_INVALID_PARAM;
    if(!p->opened)
        return SVCRT_DRV_ERROR;

    /* one sample is 2 bytes, little-endian; a shorter tail is not produced */
    while(cnt + 2 <= len)
    {
        /* TODO(chip): trigger one conversion and wait for it to finish:
         * ADCx->CR2 |= SWSTART; while(!(ADCx->SR & EOC));
         * p->last_value = (uint16)ADCx->DR; */
        pdata[cnt]     = (uint8)(p->last_value & 0xFF);
        pdata[cnt + 1] = (uint8)((p->last_value >> 8) & 0xFF);
        cnt += 2;
    }
    return cnt;
}

static int32 adc_drv_write(svcrt_dev_hdr_t *obj, uint8 *pdata, int32 len)
{
    /* an ADC is a read-only device */
    (void)obj; (void)pdata; (void)len;
    return SVCRT_DRV_ERROR;
}

static int32 adc_drv_ctrl(svcrt_dev_hdr_t *obj, uint32 code, uint32 value)
{
    adc_dev_obj_t *p = (adc_dev_obj_t *)obj;
    switch(code)
    {
    case ADC_CTRL_SET_CHANNEL:
        p->channel = (uint8)value;
        /* TODO(chip): reconfigure the analog input pin for the new channel */
        return SVCRT_DRV_OK;
    case ADC_CTRL_SET_SAMPLETIME:
        /* TODO(chip): apply the sampling time encoded in value */
        (void)value;
        return SVCRT_DRV_OK;
    case ADC_CTRL_START:
        /* TODO(chip): trigger one conversion and store it in p->last_value */
        return (int32)p->last_value;
    case SVCRT_DEV_CTRL_GET_STATUS:
        return (int32)p->channel;
    default:
        return SVCRT_DRV_ERROR;
    }
}

static svcrt_dev_drv_t adc_drv = {
    adc_drv_open,
    adc_drv_close,
    adc_drv_read,
    adc_drv_write,
    adc_drv_ctrl
};

int32 adc_drv_install(void)
{
    return svcrt_drv_register("ADC1", &adc_drv, 0);
}
