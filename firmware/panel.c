/*
 * panel.c - PDP-8/E style front panel on its own virtual screen.
 *
 * Lamps: EMA + MEMORY ADDRESS (15), a 12-bit register display chosen with a
 * selector (STATUS, AC with link, MD, MQ, IR), RUN and ION.  While the panel
 * is visible the CPU samples its registers every few dozen instructions and
 * each lamp's brightness follows how often it was lit, with a little decay
 * for incandescent-style persistence.
 *
 * Switches: the 12-bit switch register (read by OSR) and the 8/E console
 * keys ADDR LOAD, EXTD ADDR LOAD, CLEAR, CONT, EXAM, HALT, SING STEP, DEP.
 *
 * Hardware-independent: it only draws into a term_t and calls the core.
 */
#include <stdio.h>
#include <string.h>
#include "panel.h"
#include "../core/pdp8.h"

/* glyphs added to the font by tools/bdf2c.py */
#define G_LAMP(level, half) (0x80 + 2 * (level) + (half))
#define G_SW_UP   0x88                      /* +0 TL, +1 TR, +2 BL, +3 BR */
#define G_SW_DOWN 0x8c
#define G_POINTER 0x90
#define G_HLINE   0x91
#define G_VLINE   0x92
#define G_TL      0x93
#define G_TR      0x94
#define G_BL      0x95
#define G_BR      0x96

enum { SEL_STATUS, SEL_AC, SEL_MD, SEL_MQ, SEL_IR, NSEL };
static const char *sel_name[NSEL] = { "STATUS", "AC", "MD", "MQ", "IR" };
static const char *sel_bits[NSEL][12] = {
    { "L", "GT", "IB", "NI", "IO", "UF", "I0", "I1", "I2", "D0", "D1", "D2" },
};

static term_t scr;
static int sel = SEL_AC;
static bool visible;
static float b_addr[15], b_data[13], b_run, b_ion;
static uint32_t cpma;                        /* console address register */
static uint32_t disp_ma, disp_md;            /* what the lamps show while halted */
static bool was_halted;
static uint32_t last_draw_ms, rate_ms;
static uint64_t rate_icount;
static double ips;
static char sysname[24] = "";
static bool octal_entry;
static int octal_digits;
static uint32_t octal_value;
bool panel_halted;                           /* HALT was pressed on the panel */

term_t *panel_screen(void) { return &scr; }
void panel_set_system(const char *name) { snprintf(sysname, sizeof sysname, "%s", name); }

/* ------------------------------------------------------------ drawing */
static void put(int x, int y, uint16_t c)
{
    if (x >= 0 && x < TEXT_COLS && y >= 0 && y < TEXT_ROWS) scr.buf[y][x] = c;
}

static void text(int x, int y, const char *s, uint16_t attr)
{
    while (*s) put(x++, y, (uint16_t)((uint8_t)*s++ | attr));
}

static void textf(int x, int y, uint16_t attr, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
#include <stdarg.h>
static void textf(int x, int y, uint16_t attr, const char *fmt, ...)
{
    char b[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    text(x, y, b, attr);
}

static int lamp_x(int i) { return 8 + i * 3 + (i / 3) * 2; }   /* lamps 0-14, grouped in threes */

static void lamp(int x, int y, float b)
{
    int lv = b < 0.06f ? 0 : b < 0.30f ? 1 : b < 0.65f ? 2 : 3;
    uint16_t attr = lv == 3 ? ATTR_BOLD : 0;
    put(x, y, (uint16_t)(G_LAMP(lv, 0) | attr));
    put(x + 1, y, (uint16_t)(G_LAMP(lv, 1) | attr));
}

static void sw(int x, int y, bool up)
{
    uint16_t g = up ? G_SW_UP : G_SW_DOWN, attr = up ? ATTR_BOLD : 0;
    put(x, y, (uint16_t)(g | attr));     put(x + 1, y, (uint16_t)((g + 1) | attr));
    put(x, y + 1, (uint16_t)((g + 2) | attr)); put(x + 1, y + 1, (uint16_t)((g + 3) | attr));
}

static void frame(int x0, int y0, int x1, int y1)
{
    for (int x = x0 + 1; x < x1; x++) { put(x, y0, G_HLINE); put(x, y1, G_HLINE); }
    for (int y = y0 + 1; y < y1; y++) { put(x0, y, G_VLINE); put(x1, y, G_VLINE); }
    put(x0, y0, G_TL); put(x1, y0, G_TR); put(x0, y1, G_BL); put(x1, y1, G_BR);
}

static void key_label(int x, int y, char k, const char *name)
{
    char b[2] = { k, 0 };
    text(x, y, b, ATTR_REVERSE);
    text(x + 2, y, name, 0);
}

static const char sw_keys[12] = { '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=' };

static void draw_static(void)
{
    term_clear(&scr);
    scr.cursor_visible = false;
    frame(1, 2, 78, 17);
    text(3, 1, "PDP-8/E", ATTR_BOLD);
    text(11, 1, "front panel", 0);

    text(lamp_x(0) + 1, 3, "EMA", 0);
    text(lamp_x(3) + 12, 3, "MEMORY ADDRESS", 0);
    for (int i = 0; i < 15; i++) {
        int bit = i < 3 ? i : i - 3;
        textf(lamp_x(i) + (bit < 10 ? 1 : 0), 5, 0, "%d", bit);
    }
    for (int i = 0; i < 12; i++) {
        textf(lamp_x(i + 3) + (i < 10 ? 1 : 0), 10, 0, "%d", i);
        char k[2] = { sw_keys[i], 0 };
        text(lamp_x(i + 3), 15, k, ATTR_REVERSE);
    }
    text(lamp_x(3) + 11, 12, "SWITCH REGISTER", 0);

    text(65, 3, "RUN", 0);
    text(70, 3, "ION", 0);
    text(64, 8, "DISPLAY", 0);
    text(64, 9, "(Up/Down)", 0);
    for (int s = 0; s < NSEL; s++) text(66, 10 + s, sel_name[s], 0);

    int x = 3, y = 18;
    key_label(x, y, 'L', "ADDR LOAD");   x += 13;
    key_label(x, y, 'X', "EXTD LOAD");   x += 13;
    key_label(x, y, 'C', "CLEAR");       x += 9;
    key_label(x, y, 'G', "CONT");        x += 8;
    key_label(x, y, 'E', "EXAM");        x += 8;
    key_label(x, y, 'H', "HALT");        x += 8;
    key_label(x, y, 'S', "SING STEP");   x += 13;
    key_label(x, y, 'D', "DEP");

    text(3, 24, "1 2 3 ... 0 - =  toggle switches 0-11     O + 4 octal digits  set switches", 0);
    text(3, 25, "Z  zero the switches                     Up/Down  choose the register", 0);
    text(3, 26, "EXAM/DEP use the console address set by ADDR LOAD (field: EXTD LOAD).", 0);
    text(3, 27, "Programs read the switches with OSR.   F11 or Esc: back to the terminals", 0);
}

void panel_init(void)
{
    term_init(&scr, -2);
    draw_static();
}

void panel_show(bool v)
{
    visible = v;
    pdp8_lamps_enable(v);
    if (v) {
        pdp8_lamps_t discard;
        pdp8_lamps_take(&discard);                 /* start a fresh average */
        last_draw_ms = 0;
    }
}

/* ------------------------------------------------------------ dynamic part */
static uint32_t reg_value(int s, bool halted)
{
    switch (s) {
    case SEL_STATUS: return pdp8_status_word();
    case SEL_AC:     return pdp8.lac & 017777;              /* link in bit 12 */
    case SEL_MD:     return halted ? disp_md : pdp8.md;
    case SEL_MQ:     return pdp8.mq;
    default:         return pdp8.ir;
    }
}

static void fade(float *b, float duty) { *b = *b * 0.5f + duty * 0.5f; }

void panel_update(uint32_t now)
{
    bool halted = pdp8.halted;
    if (halted && !was_halted) {                   /* just stopped: latch what to show */
        cpma = pdp8.pc;
        disp_ma = pdp8.ma;
        disp_md = pdp8.md;
    }
    was_halted = halted;
    if (!halted) panel_halted = false;

    if (now - rate_ms >= 1000) {
        ips = (double)(pdp8.icount - rate_icount) * 1000.0 / (double)(now - rate_ms ? now - rate_ms : 1);
        rate_ms = now;
        rate_icount = pdp8.icount;
    }
    if (!visible || now - last_draw_ms < 25) return;
    last_draw_ms = now;

    pdp8_lamps_t l;
    pdp8_lamps_take(&l);
    float inv = l.samples ? 1.0f / (float)l.samples : 0.0f;
    bool sampled = !halted && l.samples > 0;

    /* address lamps */
    uint32_t ma = halted ? disp_ma : pdp8.ma;
    for (int i = 0; i < 15; i++) {
        float d = sampled ? l.ma[i] * inv : (float)((ma >> (14 - i)) & 1);
        fade(&b_addr[i], d);
        lamp(lamp_x(i), 4, b_addr[i]);
    }
    /* register display (with the link lamp in front when showing AC) */
    uint32_t v = reg_value(sel, halted);
    const uint32_t *cnt = sel == SEL_STATUS ? l.st : sel == SEL_AC ? l.ac + 1 :
                          sel == SEL_MD ? l.md : sel == SEL_MQ ? l.mq : l.ir;
    for (int i = 0; i < 12; i++) {
        float d = sampled ? cnt[i] * inv : (float)((v >> (11 - i)) & 1);
        fade(&b_data[i + 1], d);
        lamp(lamp_x(i + 3), 9, b_data[i + 1]);
    }
    if (sel == SEL_AC) {
        fade(&b_data[0], sampled ? l.ac[0] * inv : (float)((v >> 12) & 1));
        lamp(lamp_x(2), 9, b_data[0]);
        text(lamp_x(2), 10, " L", 0);
    } else {
        text(lamp_x(2), 9, "  ", 0);
        text(lamp_x(2), 10, "  ", 0);
    }
    text(lamp_x(3), 8, "                                          ", 0);
    if (sel == SEL_STATUS) {
        for (int i = 0; i < 12; i++) text(lamp_x(i + 3), 8, sel_bits[SEL_STATUS][i], 0);
    } else {
        textf(lamp_x(3) + 14, 8, 0, "%-6s", sel_name[sel]);
    }

    fade(&b_run, halted ? 0.0f : 1.0f);
    lamp(65, 4, b_run);
    fade(&b_ion, sampled ? l.ion * inv : (float)pdp8.ion);
    lamp(70, 4, b_ion);
    for (int s = 0; s < NSEL; s++) put(64, 10 + s, s == sel ? (G_POINTER | ATTR_BOLD) : ' ');

    for (int i = 0; i < 12; i++) sw(lamp_x(i + 3), 13, (pdp8.sr >> (11 - i)) & 1);

    /* readouts */
    textf(3, 20, 0, "PC %o:%04o  AC %o:%04o  MQ %04o  MD %04o  IR %04o  IF %o DF %o  SR %04o   ",
          (unsigned)(pdp8.ifld >> 12), (unsigned)pdp8.pc, (unsigned)((pdp8.lac >> 12) & 1),
          (unsigned)(pdp8.lac & 07777), (unsigned)pdp8.mq, (unsigned)(halted ? disp_md : pdp8.md),
          (unsigned)pdp8.ir, (unsigned)(pdp8.ifld >> 12), (unsigned)(pdp8.df >> 12), (unsigned)pdp8.sr);
    if (halted)
        textf(3, 21, ATTR_BOLD, "%-10s HALTED at %05o   console address %o:%04o                        ",
              sysname, (unsigned)(pdp8.ifld | pdp8.pc), (unsigned)(pdp8.ifld >> 12), (unsigned)cpma);
    else
        textf(3, 21, 0, "%-10s running, %.1f million instructions/s (about %.0fx a real PDP-8/E)      ",
              sysname, ips / 1e6, ips / 400000.0);
    if (octal_entry)
        textf(3, 22, ATTR_REVERSE, " SWITCHES = %0*o ", octal_digits ? octal_digits : 1, (unsigned)octal_value);
    else
        text(3, 22, "                    ", 0);
    textf(62, 1, 0, "%16s", halted ? "HALT" : "RUN");
}

/* ------------------------------------------------------------ keys */
static void examine_show(uint32_t addr)
{
    disp_ma = addr;
    disp_md = pdp8_mem[addr];
}

bool panel_key(int k)
{
    if (k == 27) return false;
    if (k == PANEL_KEY_UP)   { sel = (sel + NSEL - 1) % NSEL; return true; }
    if (k == PANEL_KEY_DOWN) { sel = (sel + 1) % NSEL; return true; }
    if (k < 0 || k > 0x7f) return true;
    if (k >= 'a' && k <= 'z') k -= 32;

    if (octal_entry) {
        if (k >= '0' && k <= '7') {
            octal_value = ((octal_value << 3) | (uint32_t)(k - '0')) & 07777;
            if (++octal_digits == 4) { pdp8.sr = octal_value; octal_entry = false; }
            return true;
        }
        if (k == '\r' && octal_digits) pdp8.sr = octal_value;
        octal_entry = false;
        if (k == '\r' || k == 0177) return true;
    }

    for (int i = 0; i < 12; i++)
        if (k == sw_keys[i]) { pdp8.sr ^= 04000u >> i; return true; }

    bool halted = pdp8.halted;
    switch (k) {
    case 'O': octal_entry = true; octal_digits = 0; octal_value = 0; break;
    case 'Z': pdp8.sr = 0; break;
    case 'H':                                       /* HALT */
        if (!halted) {
            pdp8.halted = true;
            pdp8.halt_pc = pdp8.ifld | pdp8.pc;
            panel_halted = true;
        }
        break;
    case 'G':                                       /* CONT */
        if (halted) { pdp8.halted = false; panel_halted = false; }
        break;
    case 'S':                                       /* SING STEP */
        if (halted) {
            pdp8.halted = false;
            pdp8_run(1);
            pdp8.halted = true;
            pdp8.halt_pc = pdp8.ifld | pdp8.pc;
            panel_halted = true;
            cpma = pdp8.pc;
            disp_ma = pdp8.ma;
            disp_md = pdp8.md;
        }
        break;
    case 'L':                                       /* ADDR LOAD */
        if (halted) { cpma = pdp8.sr; pdp8.pc = pdp8.sr; disp_ma = pdp8.ifld | cpma; }
        break;
    case 'X':                                       /* EXTD ADDR LOAD: IF = SR6-8, DF = SR9-11 */
        if (halted) {
            pdp8.ifld = pdp8.ib = (pdp8.sr & 070) << 9;
            pdp8.df = (pdp8.sr & 07) << 12;
            disp_ma = pdp8.ifld | cpma;
        }
        break;
    case 'C':                                       /* CLEAR */
        if (halted) pdp8_caf();
        break;
    case 'E':                                       /* EXAM */
        if (halted) { examine_show(pdp8.ifld | cpma); cpma = (cpma + 1) & 07777; }
        break;
    case 'D':                                       /* DEP */
        if (halted) {
            pdp8_mem[pdp8.ifld | cpma] = (uint16_t)pdp8.sr;
            examine_show(pdp8.ifld | cpma);
            cpma = (cpma + 1) & 07777;
        }
        break;
    default: break;
    }
    return true;
}
