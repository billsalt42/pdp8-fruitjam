/*
 * sdcard.c - minimal SPI-mode SD/SDHC/SDXC driver + FatFs glue for the
 * Adafruit Fruit Jam microSD slot (SPI0: SCK GPIO34, MOSI GPIO35,
 * MISO GPIO36, CS GPIO39).
 */
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"
#include "fatfs/ff.h"
#include "fatfs/diskio.h"

#define SD_SPI   spi0
#define PIN_SCK  34
#define PIN_MOSI 35
#define PIN_MISO 36
#define PIN_CS   39

static bool sd_ok, sd_hc;

static inline void cs_low(void)  { gpio_put(PIN_CS, 0); }
static inline void cs_high(void) { gpio_put(PIN_CS, 1); }

static uint8_t xfer(uint8_t b)
{
    uint8_t r;
    spi_write_read_blocking(SD_SPI, &b, &r, 1);
    return r;
}

static bool wait_ready(uint32_t ms)
{
    absolute_time_t t = make_timeout_time_ms(ms);
    do {
        if (xfer(0xff) == 0xff) return true;
    } while (!time_reached(t));
    return false;
}

static void deselect(void) { cs_high(); xfer(0xff); }

static bool select_card(void)
{
    cs_low();
    xfer(0xff);
    if (wait_ready(500)) return true;
    deselect();
    return false;
}

static uint8_t send_cmd(uint8_t cmd, uint32_t arg)
{
    if (cmd & 0x80) {                   /* ACMD: CMD55 first */
        cmd &= 0x7f;
        uint8_t r = send_cmd(55, 0);
        if (r > 1) return r;
    }
    if (cmd != 12) {
        deselect();
        if (!select_card()) return 0xff;
    }
    uint8_t crc = 0x01;
    if (cmd == 0) crc = 0x95;
    if (cmd == 8) crc = 0x87;
    uint8_t buf[6] = { (uint8_t)(0x40 | cmd), (uint8_t)(arg >> 24), (uint8_t)(arg >> 16),
                       (uint8_t)(arg >> 8), (uint8_t)arg, crc };
    spi_write_blocking(SD_SPI, buf, 6);
    if (cmd == 12) xfer(0xff);
    uint8_t r;
    int n = 10;
    do { r = xfer(0xff); } while ((r & 0x80) && --n);
    return r;
}

static bool sd_init(void)
{
    spi_init(SD_SPI, 400 * 1000);
    gpio_set_function(PIN_SCK, GPIO_FUNC_SPI);
    gpio_set_function(PIN_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(PIN_MISO, GPIO_FUNC_SPI);
    gpio_pull_up(PIN_MISO);
    gpio_init(PIN_CS);
    gpio_set_dir(PIN_CS, GPIO_OUT);
    cs_high();

    for (int i = 0; i < 10; i++) xfer(0xff);   /* >= 74 clocks with CS high */

    sd_ok = false;
    sd_hc = false;
    uint8_t r = 0xff;
    for (int tries = 0; tries < 10 && r != 1; tries++) r = send_cmd(0, 0);
    if (r != 1) { deselect(); return false; }

    absolute_time_t t = make_timeout_time_ms(1500);
    if (send_cmd(8, 0x1aa) == 1) {             /* SD v2 */
        uint8_t ocr[4];
        for (int i = 0; i < 4; i++) ocr[i] = xfer(0xff);
        if (ocr[2] != 0x01 || ocr[3] != 0xaa) { deselect(); return false; }
        while (send_cmd(0x80 | 41, 1u << 30) != 0)
            if (time_reached(t)) { deselect(); return false; }
        if (send_cmd(58, 0) != 0) { deselect(); return false; }
        for (int i = 0; i < 4; i++) ocr[i] = xfer(0xff);
        sd_hc = (ocr[0] & 0x40) != 0;
    } else {                                   /* SD v1 / MMC */
        uint8_t cmd = (send_cmd(0x80 | 41, 0) <= 1) ? (0x80 | 41) : 1;
        while (send_cmd(cmd, 0) != 0)
            if (time_reached(t)) { deselect(); return false; }
        if (send_cmd(16, 512) != 0) { deselect(); return false; }
    }
    deselect();
    spi_set_baudrate(SD_SPI, 20 * 1000 * 1000);
    sd_ok = true;
    return true;
}

static bool rx_block(uint8_t *buf)
{
    absolute_time_t t = make_timeout_time_ms(200);
    uint8_t tok;
    do { tok = xfer(0xff); } while (tok == 0xff && !time_reached(t));
    if (tok != 0xfe) return false;
    spi_read_blocking(SD_SPI, 0xff, buf, 512);
    xfer(0xff); xfer(0xff);                    /* CRC */
    return true;
}

static bool tx_block(const uint8_t *buf, uint8_t token)
{
    if (!wait_ready(500)) return false;
    xfer(token);
    spi_write_blocking(SD_SPI, buf, 512);
    xfer(0xff); xfer(0xff);                    /* dummy CRC */
    uint8_t resp = xfer(0xff);
    return (resp & 0x1f) == 0x05;
}

/* ---------------- FatFs diskio ---------------- */
DSTATUS disk_status(BYTE pdrv) { return (pdrv == 0 && sd_ok) ? 0 : STA_NOINIT; }

DSTATUS disk_initialize(BYTE pdrv)
{
    if (pdrv) return STA_NOINIT;
    return sd_init() ? 0 : STA_NOINIT;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
    if (pdrv || !sd_ok) return RES_NOTRDY;
    for (UINT i = 0; i < count; i++) {
        uint32_t a = sd_hc ? (uint32_t)(sector + i) : (uint32_t)(sector + i) * 512u;
        bool ok = send_cmd(17, a) == 0 && rx_block(buff + 512 * i);
        deselect();
        if (!ok) return RES_ERROR;
    }
    return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    if (pdrv || !sd_ok) return RES_NOTRDY;
    for (UINT i = 0; i < count; i++) {
        uint32_t a = sd_hc ? (uint32_t)(sector + i) : (uint32_t)(sector + i) * 512u;
        bool ok = send_cmd(24, a) == 0 && tx_block(buff + 512 * i, 0xfe);
        if (ok) ok = wait_ready(500);
        deselect();
        if (!ok) return RES_ERROR;
    }
    return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    if (pdrv || !sd_ok) return RES_NOTRDY;
    switch (cmd) {
    case CTRL_SYNC:
        select_card();
        deselect();
        return RES_OK;
    case GET_BLOCK_SIZE:
        *(DWORD *)buff = 1;
        return RES_OK;
    default:
        return RES_PARERR;
    }
}
