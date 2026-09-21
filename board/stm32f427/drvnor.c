/**
* @file drvnor.c
* @brief STM32F427 onboard SPI NOR flash (W25Q128, 16 MiB) - board level
* @details See drvnor.h for the schematic wiring. SPI2 + PE15 chip select.
*
*          Why this file initialises the SPI pins itself instead of relying on
*          the CubeMX generated HAL_SPI_MspInit(): board/ is a standalone layer
*          and must not depend on a file that lives in one example project. The
*          MspInit callback still runs (HAL_SPI_Init calls it) and programs the
*          same pins with the same settings, so doing it here is idempotent.
*
* @author xw
* @date 2026.09.21
*/

#include "stm32f4xx.h"
#include "stm32f4xx_hal.h"
#include "drvnor.h"

/* ============================================================
 * Chip select
 * ============================================================ */
#define NOR_CS_ASSERT()    (GPIOE->BSRR = ((uint32)GPIO_PIN_15 << 16u))
#define NOR_CS_RELEASE()   (GPIOE->BSRR =  (uint32)GPIO_PIN_15)

/* ============================================================
 * Command set (only what this driver needs)
 * ============================================================ */
#define NOR_CMD_WRITE_ENABLE   (0x06u)
#define NOR_CMD_READ_STATUS1   (0x05u)
#define NOR_CMD_READ_DATA      (0x03u)
#define NOR_CMD_PAGE_PROGRAM   (0x02u)
#define NOR_CMD_SECTOR_ERASE   (0x20u)   /* 4 KiB */
#define NOR_CMD_BLOCK_ERASE    (0xD8u)   /* 64 KiB */
#define NOR_CMD_JEDEC_ID       (0x9Fu)
#define NOR_CMD_RELEASE_PD     (0xABu)

#define NOR_STATUS_WIP         (0x01u)   /* write in progress */
#define NOR_STATUS_WEL         (0x02u)   /* write enable latch */

/* ============================================================
 * Timing / sizing
 * ============================================================ */
#define NOR_PAGE_SIZE          (256u)
#define NOR_SECTOR_SIZE        SVCRT_NOR_SECTOR_BYTES

#define NOR_TMO_TX_MS          (100u)    /* one SPI transfer */
#define NOR_TMO_READY_MS       (2000u)   /* sector/block erase, page program */
#define NOR_TMO_ID_MS          (20u)

/* SCK = APB1(24 MHz) / (1 << prescaler). /4 = 6 MHz: comfortable for
 * first bring-up, still ~600 KiB/s on reads. Raise to /2 (12 MHz) later
 * if load time matters - the part supports far more, the wiring does not. */
#define NOR_BAUD_PRESCALER     (SPI_BAUDRATEPRESCALER_4)

static SPI_HandleTypeDef hnor_spi;
static uint8  nor_ready   = 0u;
static uint32 nor_jedec   = 0u;
static uint32 nor_capacity = 0u;

/* ============================================================
 * Low level helpers
 * ============================================================ */

/** @brief Send len bytes with CS held low for the whole call */
static int32 nor_tx(const uint8 *buf, uint32 len)
{
    if(HAL_SPI_Transmit(&hnor_spi, (uint8 *)buf, (uint16)len, NOR_TMO_TX_MS) != HAL_OK)
    {
        return -1;
    }
    return 0;
}

/** @brief Receive len bytes (clocks out 0xFF) with CS held low for the whole call */
static int32 nor_rx(uint8 *buf, uint32 len)
{
    if(HAL_SPI_Receive(&hnor_spi, buf, (uint16)len, NOR_TMO_TX_MS) != HAL_OK)
    {
        return -1;
    }
    return 0;
}

/** @brief Build the 4-byte "cmd + 24-bit address" header */
static void nor_cmd_addr(uint8 *hdr, uint8 cmd, uint32 addr)
{
    hdr[0] = cmd;
    hdr[1] = (uint8)(addr >> 16);
    hdr[2] = (uint8)(addr >> 8);
    hdr[3] = (uint8)(addr);
}

static int32 nor_read_status1(uint8 *p_status)
{
    uint8 cmd = NOR_CMD_READ_STATUS1;
    int32 rc;

    NOR_CS_ASSERT();
    rc = nor_tx(&cmd, 1u);
    if(rc == 0)
    {
        rc = nor_rx(p_status, 1u);
    }
    NOR_CS_RELEASE();

    return rc;
}

static int32 nor_write_enable(void)
{
    uint8 cmd  = NOR_CMD_WRITE_ENABLE;
    uint8 st   = 0u;
    int32 rc;

    NOR_CS_ASSERT();
    rc = nor_tx(&cmd, 1u);
    NOR_CS_RELEASE();

    if(rc != 0)
    {
        return -1;
    }

    /* Confirm the latch actually set - a silently failed WEL turns the
     * following program/erase into a no-op that still "returns success". */
    rc = nor_read_status1(&st);
    if((rc != 0) || ((st & NOR_STATUS_WEL) == 0u))
    {
        return -1;
    }

    return 0;
}

int32 svcrt_nor_wait_ready(uint32 timeout_ms)
{
    uint32 t0 = HAL_GetTick();
    uint8  st = 0u;
    int32  rc;

    for(;;)
    {
        rc = nor_read_status1(&st);

        if(rc != 0)
        {
            return -1;
        }

        if((st & NOR_STATUS_WIP) == 0u)
        {
            return 0;
        }

        if((HAL_GetTick() - t0) > timeout_ms)
        {
            return -1;
        }
    }
}

/* ============================================================
 * Bring-up
 * ============================================================ */
int32 svcrt_nor_init(void)
{
    GPIO_InitTypeDef gpio = {0};
    uint8 cmd[4];
    uint8 id[3] = {0u, 0u, 0u};
    uint8 st = 0u;

    if(nor_ready != 0u)
    {
        return 0;
    }

    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOE_CLK_ENABLE();

    /* CS first: drive it high before the SPI peripheral can clock anything,
     * otherwise the chip may latch a partial command during init. */
    HAL_GPIO_WritePin(GPIOE, GPIO_PIN_15, GPIO_PIN_SET);
    gpio.Pin   = GPIO_PIN_15;
    gpio.Mode  = GPIO_MODE_OUTPUT_PP;
    gpio.Pull  = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    HAL_GPIO_Init(GPIOE, &gpio);

    /* SPI2 pins (PB10 SCK, PB14 MISO, PB15 MOSI, AF5) */
    gpio.Pin       = GPIO_PIN_10 | GPIO_PIN_14 | GPIO_PIN_15;
    gpio.Mode      = GPIO_MODE_AF_PP;
    gpio.Pull      = GPIO_NOPULL;
    gpio.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate = GPIO_AF5_SPI2;
    HAL_GPIO_Init(GPIOB, &gpio);

    hnor_spi.Instance               = SPI2;
    hnor_spi.Init.Mode              = SPI_MODE_MASTER;
    hnor_spi.Init.Direction         = SPI_DIRECTION_2LINES;
    hnor_spi.Init.DataSize          = SPI_DATASIZE_8BIT;
    hnor_spi.Init.CLKPolarity       = SPI_POLARITY_LOW;   /* mode 0 */
    hnor_spi.Init.CLKPhase          = SPI_PHASE_1EDGE;
    hnor_spi.Init.NSS               = SPI_NSS_SOFT;
    hnor_spi.Init.BaudRatePrescaler = NOR_BAUD_PRESCALER;
    hnor_spi.Init.FirstBit          = SPI_FIRSTBIT_MSB;
    hnor_spi.Init.TIMode            = SPI_TIMODE_DISABLE;
    hnor_spi.Init.CRCCalculation    = SPI_CRCCALCULATION_DISABLE;
    hnor_spi.Init.CRCPolynomial     = 10;

    if(HAL_SPI_Init(&hnor_spi) != HAL_OK)
    {
        return -1;
    }

    /* A part that was left in deep power-down ignores everything but 0xAB. */
    cmd[0] = NOR_CMD_RELEASE_PD;
    NOR_CS_ASSERT();
    nor_tx(cmd, 1u);
    NOR_CS_RELEASE();

    /* JEDEC id */
    cmd[0] = NOR_CMD_JEDEC_ID;
    NOR_CS_ASSERT();
    if((nor_tx(cmd, 1u) != 0) || (nor_rx(id, 3u) != 0))
    {
        NOR_CS_RELEASE();
        return -1;
    }
    NOR_CS_RELEASE();

    nor_jedec = ((uint32)id[0] << 16) | ((uint32)id[1] << 8) | (uint32)id[2];

    /* Capacity from the JEDEC "memory type + capacity" byte pair.
     * 0x18 = 128 Mbit = 16 MiB on W25Q128. Any other capacity is still a
     * usable NOR, so accept the vendor byte and derive the size instead of
     * hard-coding one part number. */
    if(id[0] == 0u)
    {
        return -1;
    }

    nor_capacity = (1u << (id[2] & 0x1Fu));

    if(nor_capacity > (64u * 1024u * 1024u))
    {
        nor_capacity = 64u * 1024u * 1024u;
    }

    /* Leave the chip idle and write-disabled before anyone uses it. */
    if((nor_read_status1(&st) != 0) || (svcrt_nor_wait_ready(NOR_TMO_READY_MS) != 0))
    {
        return -1;
    }

    nor_ready = 1u;
    return 0;
}

uint8 svcrt_nor_present(void)
{
    return nor_ready;
}

uint32 svcrt_nor_jedec_id(void)
{
    return nor_jedec;
}

uint32 svcrt_nor_capacity(void)
{
    return nor_capacity;
}

uint32 svcrt_nor_sector_size(void)
{
    return NOR_SECTOR_SIZE;
}

/* ============================================================
 * Read / write / erase
 * ============================================================ */
int32 svcrt_nor_read(uint32 addr, uint8 *buf, uint32 len)
{
    uint8 hdr[4];
    int32 rc = 0;

    if((nor_ready == 0u) || (buf == 0) || (len == 0u))
    {
        return -1;
    }

    if((addr + len) > nor_capacity)
    {
        return -1;
    }

    nor_cmd_addr(hdr, NOR_CMD_READ_DATA, addr);

    NOR_CS_ASSERT();
    if(nor_tx(hdr, 4u) != 0)
    {
        rc = -1;
    }
    else if(nor_rx(buf, len) != 0)
    {
        rc = -1;
    }
    NOR_CS_RELEASE();

    return rc;
}

int32 svcrt_nor_write(uint32 addr, const uint8 *data, uint32 len)
{
    uint8 hdr[4];
    uint32 left = len;
    uint32 off  = addr;
    const uint8 *p = data;

    if((nor_ready == 0u) || (data == 0) || (len == 0u))
    {
        return -1;
    }

    if((addr + len) > nor_capacity)
    {
        return -1;
    }

    while(left > 0u)
    {
        /* Never cross a page boundary: the chip wraps within the page, which
         * would silently overwrite the start of the same page. */
        uint32 page_left = NOR_PAGE_SIZE - (off & (NOR_PAGE_SIZE - 1u));
        uint32 chunk     = (left < page_left) ? left : page_left;

        if(nor_write_enable() != 0)
        {
            return -1;
        }

        nor_cmd_addr(hdr, NOR_CMD_PAGE_PROGRAM, off);

        NOR_CS_ASSERT();
        if((nor_tx(hdr, 4u) != 0) || (nor_tx(p, chunk) != 0))
        {
            NOR_CS_RELEASE();
            return -1;
        }
        NOR_CS_RELEASE();

        if(svcrt_nor_wait_ready(NOR_TMO_READY_MS) != 0)
        {
            return -1;
        }

        off  += chunk;
        p    += chunk;
        left -= chunk;
    }

    return 0;
}

int32 svcrt_nor_erase_sector(uint32 addr)
{
    uint8 hdr[4];

    if(nor_ready == 0u)
    {
        return -1;
    }

    if((addr + NOR_SECTOR_SIZE) > nor_capacity)
    {
        return -1;
    }

    if(nor_write_enable() != 0)
    {
        return -1;
    }

    nor_cmd_addr(hdr, NOR_CMD_SECTOR_ERASE, addr & ~(NOR_SECTOR_SIZE - 1u));

    NOR_CS_ASSERT();
    if(nor_tx(hdr, 4u) != 0)
    {
        NOR_CS_RELEASE();
        return -1;
    }
    NOR_CS_RELEASE();

    return svcrt_nor_wait_ready(NOR_TMO_READY_MS);
}

int32 svcrt_nor_erase(uint32 addr, uint32 size)
{
    uint32 first = addr & ~(NOR_SECTOR_SIZE - 1u);
    uint32 last  = (addr + size + NOR_SECTOR_SIZE - 1u) & ~(NOR_SECTOR_SIZE - 1u);
    uint32 a;

    if((nor_ready == 0u) || (size == 0u))
    {
        return -1;
    }

    if(last > nor_capacity)
    {
        return -1;
    }

    for(a = first; a < last; a += NOR_SECTOR_SIZE)
    {
        if(svcrt_nor_erase_sector(a) != 0)
        {
            return -1;
        }
    }

    return 0;
}
