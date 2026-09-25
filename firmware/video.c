/*
 * video.c - 640x480@60 DVI text display (80x30, 8x16 font) using the RP2350
 * HSTX peripheral on the Adafruit Fruit Jam.  Runs entirely on core 1:
 * a DMA ping-pong feeds HSTX with command lists and scanlines, and each
 * scanline is rendered from the text buffer just before it is needed.
 *
 * Based on the pico-examples "dvi_out_hstx_encoder" example
 * (Copyright (c) 2024 Raspberry Pi (Trading) Ltd., BSD-3-Clause) and the
 * clock scheme used by the Pimoroni/Adafruit DVHSTX driver.
 */
#include <string.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/clocks.h"
#include "hardware/pll.h"
#include "hardware/vreg.h"
#include "hardware/structs/bus_ctrl.h"
#include "hardware/structs/hstx_ctrl.h"
#include "hardware/structs/hstx_fifo.h"
#include "hardware/structs/qmi.h"
#include "hardware/structs/ioqspi.h"
#include "hardware/sync.h"
#include "video.h"
#include "term.h"

extern const uint8_t font8x16[256][16];

static term_t *volatile shown;
static term_t blank_term;
void video_show(struct term *t) { shown = t; }
struct term *video_shown(void) { return shown; }

/* ---------------------------------------------------------------- clocks */
/* clk_sys = 240 MHz from PLL_USB (480 MHz VCO output), which is an exact
 * multiple of 12 MHz for PIO-USB; PLL_SYS is then free to generate the exact
 * 126 MHz HSTX clock (2 bits/clock -> 252 Mbit/s TMDS = 25.2 MHz pixels). */
void __no_inline_not_in_flash_func(video_clock_init)(void)
{
    uint32_t irq = save_and_disable_interrupts();

    /* slow the flash interface down before speeding things up */
    hw_write_masked(&qmi_hw->m[0].timing, 6, QMI_M0_TIMING_CLKDIV_BITS);
    vreg_set_voltage(VREG_VOLTAGE_1_15);
    volatile uint32_t *xip = (volatile uint32_t *)0x14000000;
    (void)*xip;

    hw_clear_bits(&clocks_hw->clk[clk_sys].ctrl, CLOCKS_CLK_SYS_CTRL_SRC_BITS);
    while (clocks_hw->clk[clk_sys].selected != 0x1) tight_loop_contents();
    hw_write_masked(&clocks_hw->clk[clk_ref].ctrl, CLOCKS_CLK_REF_CTRL_SRC_VALUE_XOSC_CLKSRC,
                    CLOCKS_CLK_REF_CTRL_SRC_BITS);
    while (clocks_hw->clk[clk_ref].selected != 0x4) tight_loop_contents();

    clock_stop(clk_usb);
    clock_stop(clk_adc);
    clock_stop(clk_peri);
    clock_stop(clk_hstx);

    pll_init(pll_usb, 1, 1440 * MHZ, 3, 1);                 /* 480 MHz */
    const uint32_t f = 480 * MHZ;
    clock_configure(clk_sys, CLOCKS_CLK_SYS_CTRL_SRC_VALUE_CLKSRC_CLK_SYS_AUX,
                    CLOCKS_CLK_SYS_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB, f, f / 2);
    clock_configure(clk_peri, 0, CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB, f, f / 4);
    clock_configure(clk_usb, 0, CLOCKS_CLK_USB_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB, f, 48 * MHZ);
    clock_configure(clk_adc, 0, CLOCKS_CLK_ADC_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB, f, 48 * MHZ);

    /* PLL_SYS: 1512 MHz / 6 / 2 = 126 MHz for HSTX */
    pll_init(pll_sys, 1, 1512 * MHZ, 6, 2);
    clock_configure(clk_hstx, 0, CLOCKS_CLK_HSTX_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS,
                    126 * MHZ, 126 * MHZ);

    /* flash: clkdiv 4 (60 MHz) with one cycle of read delay */
    qmi_hw->m[0].timing = 0x40000104;
    (void)*xip;

    restore_interrupts(irq);
}

/* ---------------------------------------------------------------- DVI timing */
#define TMDS_CTRL_00 0x354u
#define TMDS_CTRL_01 0x0abu
#define TMDS_CTRL_10 0x154u
#define TMDS_CTRL_11 0x2abu
#define SYNC_V0_H0 (TMDS_CTRL_00 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V0_H1 (TMDS_CTRL_01 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V1_H0 (TMDS_CTRL_10 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V1_H1 (TMDS_CTRL_11 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))

#define H_FRONT 16
#define H_SYNC  96
#define H_BACK  48
#define H_ACTIVE 640
#define V_FRONT 10
#define V_SYNC  2
#define V_BACK  33
#define V_ACTIVE 480
#define V_TOTAL (V_FRONT + V_SYNC + V_BACK + V_ACTIVE)
#define V_BLANK (V_FRONT + V_SYNC + V_BACK)

#define HSTX_CMD_RAW_REPEAT  (0x1u << 12)
#define HSTX_CMD_TMDS        (0x2u << 12)
#define HSTX_CMD_NOP         (0xfu << 12)

static uint32_t vblank_line_vsync_off[] = {
    HSTX_CMD_RAW_REPEAT | H_FRONT, SYNC_V1_H1,
    HSTX_CMD_RAW_REPEAT | H_SYNC,  SYNC_V1_H0,
    HSTX_CMD_RAW_REPEAT | (H_BACK + H_ACTIVE), SYNC_V1_H1,
    HSTX_CMD_NOP
};
static uint32_t vblank_line_vsync_on[] = {
    HSTX_CMD_RAW_REPEAT | H_FRONT, SYNC_V0_H1,
    HSTX_CMD_RAW_REPEAT | H_SYNC,  SYNC_V0_H0,
    HSTX_CMD_RAW_REPEAT | (H_BACK + H_ACTIVE), SYNC_V0_H1,
    HSTX_CMD_NOP
};
static uint32_t vactive_line[] = {
    HSTX_CMD_RAW_REPEAT | H_FRONT, SYNC_V1_H1, HSTX_CMD_NOP,
    HSTX_CMD_RAW_REPEAT | H_SYNC,  SYNC_V1_H0, HSTX_CMD_NOP,
    HSTX_CMD_RAW_REPEAT | H_BACK,  SYNC_V1_H1,
    HSTX_CMD_TMDS | H_ACTIVE
};

/* ---------------------------------------------------------------- renderer */
static uint32_t linebuf[2][H_ACTIVE / 4];
/* [bank][normal/bold][glyph byte] -> 8 pixels; double-buffered so a colour
 * change never stalls the scanline IRQ */
static uint32_t lut_mem[2][2][256][2];
static volatile int lut_sel;
static volatile uint8_t col_fg = 0x1c, col_bold = 0x5f, col_bg = 0x00;
static volatile bool lut_dirty = true;
static volatile uint32_t frame_count;
static volatile uint32_t flash_until;

static void build_lut(void)
{
    int bank = lut_sel ^ 1;
    uint32_t (*lut)[256][2] = lut_mem[bank];
    uint8_t fgs[2] = { col_fg, col_bold };
    for (int b = 0; b < 2; b++)
        for (int v = 0; v < 256; v++) {
            uint8_t px[8];
            for (int i = 0; i < 8; i++) px[i] = (v & (0x80 >> i)) ? fgs[b] : col_bg;
            lut[b][v][0] = px[0] | (px[1] << 8) | (px[2] << 16) | ((uint32_t)px[3] << 24);
            lut[b][v][1] = px[4] | (px[5] << 8) | (px[6] << 16) | ((uint32_t)px[7] << 24);
        }
    lut_sel = bank;
}

static void __not_in_flash_func(render_line)(uint y, uint32_t *dst)
{
    uint row = y >> 4, gy = y & 15;
    term_t *t = shown;
    volatile uint16_t *cells = t->buf[row];
    bool flash = (int32_t)(flash_until - frame_count) > 0;
    uint8_t inv_all = flash ? 0xff : 0x00;
    int cur_col = -1;
    uint32_t (*lut)[256][2] = lut_mem[lut_sel];
    if (t->cursor_visible && (int)row == t->cy && gy >= 13 && gy <= 14 && (frame_count & 32))
        cur_col = t->cx;
    for (int x = 0; x < TEXT_COLS; x++) {
        uint16_t c = cells[x];
        uint8_t g = font8x16[c & 0xff][gy];
        if ((c & ATTR_UNDERLINE) && gy == 14) g = 0xff;
        if (c & ATTR_REVERSE) g = ~g;
        if (x == cur_col) g = ~g;
        g ^= inv_all;
        const uint32_t *p = lut[(c >> 9) & 1][g];
        dst[0] = p[0];
        dst[1] = p[1];
        dst += 2;
    }
}

/* ---------------------------------------------------------------- DMA/IRQ */
#define DMACH_PING 0
#define DMACH_PONG 1
static bool dma_pong;
static uint v_scanline = 2;
static bool vactive_cmdlist_posted;

static void __scratch_x("") dma_irq_handler(void)
{
    uint ch_num = dma_pong ? DMACH_PONG : DMACH_PING;
    dma_channel_hw_t *ch = &dma_hw->ch[ch_num];
    dma_hw->intr = 1u << ch_num;
    dma_pong = !dma_pong;

    if (v_scanline >= V_FRONT && v_scanline < V_FRONT + V_SYNC) {
        ch->read_addr = (uintptr_t)vblank_line_vsync_on;
        ch->transfer_count = count_of(vblank_line_vsync_on);
    } else if (v_scanline < V_BLANK) {
        ch->read_addr = (uintptr_t)vblank_line_vsync_off;
        ch->transfer_count = count_of(vblank_line_vsync_off);
        if (v_scanline == V_BLANK - 1)          /* prepare first visible line */
            render_line(0, linebuf[0]);
    } else if (!vactive_cmdlist_posted) {
        ch->read_addr = (uintptr_t)vactive_line;
        ch->transfer_count = count_of(vactive_line);
        vactive_cmdlist_posted = true;
    } else {
        uint y = v_scanline - V_BLANK;
        ch->read_addr = (uintptr_t)linebuf[y & 1];
        ch->transfer_count = H_ACTIVE / 4;
        vactive_cmdlist_posted = false;
        if (y + 1 < V_ACTIVE) render_line(y + 1, linebuf[(y + 1) & 1]);
        else frame_count++;
    }
    if (!vactive_cmdlist_posted) v_scanline = (v_scanline + 1) % V_TOTAL;
}

static void core1_main(void)
{
    /* RGB332: lane 2 (red) = bits 7:5, lane 1 (green) = 4:2, lane 0 (blue) = 1:0 */
    hstx_ctrl_hw->expand_tmds =
        2  << HSTX_CTRL_EXPAND_TMDS_L2_NBITS_LSB | 0  << HSTX_CTRL_EXPAND_TMDS_L2_ROT_LSB |
        2  << HSTX_CTRL_EXPAND_TMDS_L1_NBITS_LSB | 29 << HSTX_CTRL_EXPAND_TMDS_L1_ROT_LSB |
        1  << HSTX_CTRL_EXPAND_TMDS_L0_NBITS_LSB | 26 << HSTX_CTRL_EXPAND_TMDS_L0_ROT_LSB;
    hstx_ctrl_hw->expand_shift =
        4 << HSTX_CTRL_EXPAND_SHIFT_ENC_N_SHIFTS_LSB | 8 << HSTX_CTRL_EXPAND_SHIFT_ENC_SHIFT_LSB |
        1 << HSTX_CTRL_EXPAND_SHIFT_RAW_N_SHIFTS_LSB | 0 << HSTX_CTRL_EXPAND_SHIFT_RAW_SHIFT_LSB;
    hstx_ctrl_hw->csr = 0;
    hstx_ctrl_hw->csr = HSTX_CTRL_CSR_EXPAND_EN_BITS | 5u << HSTX_CTRL_CSR_CLKDIV_LSB |
                        5u << HSTX_CTRL_CSR_N_SHIFTS_LSB | 2u << HSTX_CTRL_CSR_SHIFT_LSB |
                        HSTX_CTRL_CSR_EN_BITS;

    /* Fruit Jam: GPIO12 CK-, 13 CK+, 14 D0-, 15 D0+, 16 D1-, 17 D1+, 18 D2-, 19 D2+
     * HSTX output bit n drives GPIO 12+n. */
    hstx_ctrl_hw->bit[0] = HSTX_CTRL_BIT0_CLK_BITS | HSTX_CTRL_BIT0_INV_BITS;
    hstx_ctrl_hw->bit[1] = HSTX_CTRL_BIT0_CLK_BITS;
    for (uint lane = 0; lane < 3; ++lane) {
        int bit = 2 + 2 * lane;
        uint32_t sel = (lane * 10) << HSTX_CTRL_BIT0_SEL_P_LSB |
                       (lane * 10 + 1) << HSTX_CTRL_BIT0_SEL_N_LSB;
        hstx_ctrl_hw->bit[bit]     = sel | HSTX_CTRL_BIT0_INV_BITS;   /* N pin */
        hstx_ctrl_hw->bit[bit + 1] = sel;                             /* P pin */
    }
    for (int i = 12; i <= 19; ++i) gpio_set_function(i, 0);  /* GPIO_FUNC_HSTX */

    build_lut();

    dma_channel_config c = dma_channel_get_default_config(DMACH_PING);
    channel_config_set_chain_to(&c, DMACH_PONG);
    channel_config_set_dreq(&c, DREQ_HSTX);
    dma_channel_configure(DMACH_PING, &c, &hstx_fifo_hw->fifo, vblank_line_vsync_off,
                          count_of(vblank_line_vsync_off), false);
    c = dma_channel_get_default_config(DMACH_PONG);
    channel_config_set_chain_to(&c, DMACH_PING);
    channel_config_set_dreq(&c, DREQ_HSTX);
    dma_channel_configure(DMACH_PONG, &c, &hstx_fifo_hw->fifo, vblank_line_vsync_off,
                          count_of(vblank_line_vsync_off), false);

    dma_hw->ints0 = (1u << DMACH_PING) | (1u << DMACH_PONG);
    dma_hw->inte0 = (1u << DMACH_PING) | (1u << DMACH_PONG);
    irq_set_exclusive_handler(DMA_IRQ_0, dma_irq_handler);
    irq_set_priority(DMA_IRQ_0, 0);
    irq_set_enabled(DMA_IRQ_0, true);
    bus_ctrl_hw->priority = BUSCTRL_BUS_PRIORITY_DMA_W_BITS | BUSCTRL_BUS_PRIORITY_DMA_R_BITS;
    dma_channel_start(DMACH_PING);

    for (;;) {
        __wfi();
        if (lut_dirty) {
            lut_dirty = false;
            build_lut();
        }
    }
}

void video_start_core1(void)
{
    dma_channel_claim(DMACH_PING);
    dma_channel_claim(DMACH_PONG);
    term_init(&blank_term, -1);
    if (!shown) shown = &blank_term;
    multicore_launch_core1(core1_main);
}

void video_set_colors(uint8_t fg, uint8_t bold, uint8_t bg)
{
    col_fg = fg; col_bold = bold; col_bg = bg;
    lut_dirty = true;
}

void video_flash(void) { flash_until = frame_count + 4; }
