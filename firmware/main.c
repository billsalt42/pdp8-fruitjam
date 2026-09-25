/*
 * PDP-8/E emulator for the Adafruit Fruit Jam (RP2350B) - runs OS/8, DEC TSS/8.24
 * and UWM TSS/8.25.
 *
 *   core 0: PDP-8/E CPU + devices, USB host (keyboard) and device (serial
 *           ports), microSD disk images, system menu
 *   core 1: 640x480 DVI text display (80x30)
 *
 * OS/8 boots from RK05 cartridge images; TSS/8 loads its INIT tape and runs
 * from an RF08 fixed-head disk image.  Under TSS/8 the console (K00) and four
 * KL8E multiplexer lines (K01-K04) each get a virtual screen (Alt-F1..F5) and
 * a USB serial port.  All media are SIMH-format files on the microSD card.
 */
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <strings.h>
#include <stdlib.h>
#include <stdarg.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "tusb.h"
#include "pio_usb.h"
#include "fatfs/ff.h"
#include "video.h"
#include "term.h"
#include "keyboard.h"
#include "panel.h"
#include "../core/pdp8.h"

#define VERSION "1.3"

#define PIN_USB_HOST_DP   1
#define PIN_USB_HOST_5V   11
#define PIN_LED           29
#define PIN_BUTTON1       0

#define RK_IMAGE_BYTES (RK_SECTORS * 512u)     /* 3,325,952 */
#define NTERM 5                                /* console + 4 multiplexer lines */

enum { SYS_OS8 = 0, SYS_TSS8 = 1, SYS_UWM = 2, NSYS = 3 };
static const char *sys_name[] = { "OS/8", "TSS/8", "UWM TSS/8" };
#define IS_TSS(s) ((s) == SYS_TSS8 || (s) == SYS_UWM)

/* ---------------- settings ---------------- */
typedef struct {
    char rk[RK_NUMDR][64];
    char tss8_rf[64], tss8_bin[64];
    char uwm_rf[64], uwm_bin[64];
    int  system;
    int  color;
    bool upper;
    bool real_speed;
    int  autoboot;
} settings_t;

static settings_t cfg = {
    .rk = { "os8.rk05", "", "", "" },
    .tss8_rf = "tss8_rf.dsk", .tss8_bin = "tss8_init.bin",
    .uwm_rf = "uwm_rf.dsk", .uwm_bin = "uwm_init.bin",
    .system = SYS_OS8, .color = 0, .upper = true, .real_speed = false, .autoboot = 3,
};

static const struct { const char *name; uint8_t fg, bold, bg; } colors[] = {
    { "green", 0x1c, 0x5f, 0x00 },
    { "amber", 0xf0, 0xfd, 0x00 },
    { "white", 0xb6, 0xff, 0x00 },
    { "blue",  0x5b, 0xbf, 0x01 },
};
#define NCOLORS (int)(sizeof colors / sizeof colors[0])

/* ---------------- terminals ---------------- */
static term_t terms[NTERM];         /* 0 = console, 1..4 = TSS/8 lines K01..K04 */
static term_t menu_term;
static term_t *ui = &terms[0];      /* where firmware messages go */
static int active;                  /* terminal shown on the monitor */
static int running = -1;            /* system currently running */

static void cdc_out(int itf, const char *s, size_t n)
{
    if (!tud_cdc_n_connected(itf)) return;
    if (tud_cdc_n_write_available(itf) < n) { tud_task(); tud_cdc_n_write_flush(itf); }
    if (tud_cdc_n_write_available(itf) >= n) tud_cdc_n_write(itf, s, (uint32_t)n);
}

static void line_out(int n, uint8_t c)          /* PDP-8 output to terminal n */
{
    term_putc(&terms[n], c);
    char ch = (char)c;
    cdc_out(n, &ch, 1);
}

static void ui_char(uint8_t c)
{
    term_putc(ui, c);
    char ch = (char)c;
    cdc_out(0, &ch, 1);
}

static void out_str(const char *s)
{
    while (*s) {
        if (*s == '\n') ui_char('\r');
        ui_char((uint8_t)*s++);
    }
    tud_cdc_n_write_flush(0);
}

static void out_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void out_printf(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    out_str(buf);
}

/* keystroke from the keyboard or a serial port, for terminal n */
static void send_key(int n, int c)
{
    if (n == 0) pdp8_key((uint8_t)(c | 0200));                 /* KL8E console: KSR mark bit */
    else if (IS_TSS(running)) pdp8_ttx_key(n - 1, (uint8_t)(c & 0177));
}

static void answerback(term_t *t, const char *s)
{
    while (*s) send_key(t->id, (uint8_t)*s++);
}

static bool panel_on;
extern bool panel_halted;

static void show_terminal(int n)
{
    active = n;
    if (!panel_on) video_show(&terms[n]);
}

static void set_panel(bool on)
{
    panel_on = on;
    panel_show(on);
    video_show(on ? panel_screen() : &terms[active]);
}

/* ---------------- SD card / disk images ---------------- */
static FATFS fs;
static bool  sd_mounted;
static FIL   rk_fil[RK_NUMDR];
static bool  rk_open[RK_NUMDR], rk_ro[RK_NUMDR], rk_dirty[RK_NUMDR];
static DWORD rk_clmt[RK_NUMDR][64];
static FIL   rf_fil;
static bool  rf_open, rf_dirty;
static uint32_t rf_capacity;
static DWORD rf_clmt[64];
static uint32_t last_write_ms, last_rf_flush_ms;
static FIL   lpt_fil;
static bool  lpt_open, lpt_dirty;
static FIL   ptr_fil, ptp_fil;          /* paper tape reader / punch */
static bool  ptr_open, ptp_open, ptp_dirty;

static void service(void);

static bool sd_mount(void)
{
    if (sd_mounted) return true;
    sd_mounted = (f_mount(&fs, "", 1) == FR_OK);
    return sd_mounted;
}

/* Pad an open file with zeros up to 'size' bytes (whole 512-byte sectors). */
static void pad_file(FIL *f, const char *name, FSIZE_t size)
{
    FSIZE_t sz = f_size(f);
    if (sz >= size) return;
    static uint8_t buf[512];
    out_printf("  Extending %s to %lu bytes", name, (unsigned long)size);
    f_lseek(f, sz & ~(FSIZE_t)511);
    memset(buf, 0, sizeof buf);
    if (sz & 511) {
        UINT br, bw;
        f_read(f, buf, sz & 511, &br);
        f_lseek(f, sz & ~(FSIZE_t)511);
        f_write(f, buf, 512, &bw);
        memset(buf, 0, sizeof buf);
    }
    uint32_t n = 0;
    while (f_tell(f) < size) {
        UINT bw;
        if (f_write(f, buf, 512, &bw) != FR_OK || bw != 512) break;
        if ((++n & 255) == 0) { ui_char('.'); service(); }
    }
    f_sync(f);
    out_str(" done\n");
}

static void fast_seek(FIL *f, DWORD *tbl, UINT n)
{
    tbl[0] = n;
    f->cltbl = tbl;
    if (f_lseek(f, CREATE_LINKMAP) != FR_OK) f->cltbl = NULL;
}

static void disk_close(int u)
{
    if (rk_open[u]) { f_close(&rk_fil[u]); rk_open[u] = false; }
}

static bool disk_open(int u, const char *name)
{
    disk_close(u);
    if (!name[0]) return false;
    FIL *f = &rk_fil[u];
    rk_ro[u] = false;
    if (f_open(f, name, FA_READ | FA_WRITE) != FR_OK) {
        if (f_open(f, name, FA_READ) != FR_OK) return false;
        rk_ro[u] = true;
    }
    if (!rk_ro[u]) pad_file(f, name, RK_IMAGE_BYTES);
    fast_seek(f, rk_clmt[u], sizeof rk_clmt[u] / sizeof(DWORD));
    rk_open[u] = true;
    rk_dirty[u] = false;
    return true;
}

bool host_disk_present(int u) { return u >= 0 && u < RK_NUMDR && rk_open[u]; }
bool host_disk_readonly(int u) { return rk_ro[u]; }

static bool file_read512(FIL *f, uint32_t sector, void *buf)
{
    UINT br = 0;
    if (f_lseek(f, (FSIZE_t)sector * 512u) != FR_OK) return false;
    if (f_read(f, buf, 512, &br) != FR_OK) return false;
    if (br < 512) memset((uint8_t *)buf + br, 0, 512 - br);
    return true;
}

static bool file_write512(FIL *f, uint32_t sector, const void *buf)
{
    UINT bw = 0;
    if (f_lseek(f, (FSIZE_t)sector * 512u) != FR_OK) return false;
    if (f_write(f, buf, 512, &bw) != FR_OK || bw != 512) return false;
    last_write_ms = to_ms_since_boot(get_absolute_time());
    gpio_put(PIN_LED, 1);
    return true;
}

bool host_disk_read(int u, uint32_t sector, uint16_t *w) { return file_read512(&rk_fil[u], sector, w); }

bool host_disk_write(int u, uint32_t sector, const uint16_t *w)
{
    if (!file_write512(&rk_fil[u], sector, w)) return false;
    rk_dirty[u] = true;
    return true;
}

/* RF08 backing store for TSS/8 */
uint32_t host_rf_words(void) { return rf_open ? rf_capacity : 0; }
bool host_rf_read(uint32_t blk, uint16_t *w) { return rf_open && file_read512(&rf_fil, blk, w); }
bool host_rf_write(uint32_t blk, const uint16_t *w)
{
    if (!rf_open || !file_write512(&rf_fil, blk, w)) return false;
    rf_dirty = true;
    return true;
}

static void rf_close(void)
{
    if (rf_open) {
        pdp8_rf_flush();
        f_close(&rf_fil);
        rf_open = false;
    }
}

static bool rf_attach(const char *name)
{
    rf_close();
    if (f_open(&rf_fil, name, FA_READ | FA_WRITE) != FR_OK) return false;
    uint32_t words = (uint32_t)(f_size(&rf_fil) / 2);
    uint32_t plat = (words + 262143) / 262144;       /* 256K words per RS08 platter */
    if (plat < 1) plat = 1;
    if (plat > 4) plat = 4;
    rf_capacity = plat * 262144u;
    pad_file(&rf_fil, name, (FSIZE_t)rf_capacity * 2);
    fast_seek(&rf_fil, rf_clmt, sizeof rf_clmt / sizeof(DWORD));
    rf_open = true;
    rf_dirty = false;
    return true;
}

static void disks_sync(void)
{
    for (int u = 0; u < RK_NUMDR; u++)
        if (rk_open[u] && rk_dirty[u]) { f_sync(&rk_fil[u]); rk_dirty[u] = false; }
    if (rf_open && rf_dirty) { f_sync(&rf_fil); rf_dirty = false; }
    if (lpt_open && lpt_dirty) { f_sync(&lpt_fil); lpt_dirty = false; }
    if (ptp_open && ptp_dirty) { f_sync(&ptp_fil); ptp_dirty = false; }
    gpio_put(PIN_LED, 0);
}

static void flush_everything(void)
{
    pdp8_rf_flush();
    disks_sync();
}

void host_tty_out(uint8_t c) { line_out(0, c & 0177); }
void host_ttx_out(int line, uint8_t c) { if (line >= 0 && line < NTERM - 1) line_out(line + 1, c & 0177); }

void host_lpt_out(uint8_t c)
{
    if (!sd_mounted) return;
    if (!lpt_open) {
        if (f_open(&lpt_fil, "printer.txt", FA_OPEN_APPEND | FA_WRITE) != FR_OK) return;
        lpt_open = true;
    }
    c &= 0177;
    if (c == 0 || c == 0177) return;
    UINT bw;
    f_write(&lpt_fil, &c, 1, &bw);
    lpt_dirty = true;
    last_write_ms = to_ms_since_boot(get_absolute_time());
}

/* PC8E paper tape: the reader reads READER.BIN from the card (from the start
 * again after each boot); the punch appends to PUNCH.BIN. */

static void ptr_rewind(void)
{
    if (ptr_open) { f_close(&ptr_fil); ptr_open = false; }
}

int host_ptr_getc(void)
{
    if (!sd_mounted) return -1;
    if (!ptr_open) {
        if (f_open(&ptr_fil, "reader.bin", FA_READ) != FR_OK) return -1;
        ptr_open = true;
    }
    uint8_t c;
    UINT br = 0;
    if (f_read(&ptr_fil, &c, 1, &br) != FR_OK || br != 1) return -1;
    return c;
}

void host_ptp_putc(uint8_t c)
{
    if (!sd_mounted) return;
    if (!ptp_open) {
        if (f_open(&ptp_fil, "punch.bin", FA_OPEN_APPEND | FA_WRITE) != FR_OK) return;
        ptp_open = true;
    }
    UINT bw;
    f_write(&ptp_fil, &c, 1, &bw);
    ptp_dirty = true;
    last_write_ms = to_ms_since_boot(get_absolute_time());
}

uint32_t host_millis(void) { return to_ms_since_boot(get_absolute_time()); }

/* ---------------- settings file ---------------- */
static void trim(char *s)
{
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    char *b = s;
    while (*b && isspace((unsigned char)*b)) b++;
    if (b != s) memmove(s, b, strlen(b) + 1);
}

static void copy_str(char *dst, const char *src, size_t n)
{
    strncpy(dst, src, n - 1);
    dst[n - 1] = 0;
}

static void load_settings(void)
{
    FIL f;
    if (f_open(&f, "pdp8.cfg", FA_READ) != FR_OK) return;
    char line[128];
    while (f_gets(line, sizeof line, &f)) {
        char *h = strchr(line, '#');
        if (h) *h = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = line, *v = eq + 1;
        trim(k); trim(v);
        for (char *p = k; *p; p++) *p = (char)tolower((unsigned char)*p);
        if (!strncmp(k, "rk", 2) && k[2] >= '0' && k[2] <= '3' && !k[3]) {
            copy_str(cfg.rk[k[2] - '0'], v, sizeof cfg.rk[0]);
        } else if (!strcmp(k, "system")) {
            cfg.system = (!strcasecmp(v, "tss8") || !strcasecmp(v, "tss/8")) ? SYS_TSS8 :
                         (!strcasecmp(v, "uwm") || !strcasecmp(v, "uwm-tss8")) ? SYS_UWM : SYS_OS8;
        } else if (!strcmp(k, "tss8_rf")) {
            copy_str(cfg.tss8_rf, v, sizeof cfg.tss8_rf);
        } else if (!strcmp(k, "tss8_bin")) {
            copy_str(cfg.tss8_bin, v, sizeof cfg.tss8_bin);
        } else if (!strcmp(k, "uwm_rf")) {
            copy_str(cfg.uwm_rf, v, sizeof cfg.uwm_rf);
        } else if (!strcmp(k, "uwm_bin")) {
            copy_str(cfg.uwm_bin, v, sizeof cfg.uwm_bin);
        } else if (!strcmp(k, "color") || !strcmp(k, "colour")) {
            for (int i = 0; i < NCOLORS; i++) if (!strcasecmp(v, colors[i].name)) cfg.color = i;
        } else if (!strcmp(k, "uppercase")) {
            cfg.upper = !(strcasecmp(v, "off") == 0 || strcasecmp(v, "no") == 0 || !strcmp(v, "0"));
        } else if (!strcmp(k, "speed")) {
            cfg.real_speed = (strcasecmp(v, "real") == 0 || strcasecmp(v, "slow") == 0);
        } else if (!strcmp(k, "autoboot")) {
            cfg.autoboot = atoi(v);
        }
    }
    f_close(&f);
}

static bool save_settings(void)
{
    FIL f;
    if (f_open(&f, "pdp8.cfg", FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) return false;
    f_printf(&f, "# PDP-8/E emulator settings (Adafruit Fruit Jam)\n");
    f_printf(&f, "system=%s\n", cfg.system == SYS_TSS8 ? "tss8" : cfg.system == SYS_UWM ? "uwm" : "os8");
    for (int u = 0; u < RK_NUMDR; u++) f_printf(&f, "rk%d=%s\n", u, cfg.rk[u]);
    f_printf(&f, "tss8_rf=%s\n", cfg.tss8_rf);
    f_printf(&f, "tss8_bin=%s\n", cfg.tss8_bin);
    f_printf(&f, "uwm_rf=%s\n", cfg.uwm_rf);
    f_printf(&f, "uwm_bin=%s\n", cfg.uwm_bin);
    f_printf(&f, "color=%s\n", colors[cfg.color].name);
    f_printf(&f, "uppercase=%s\n", cfg.upper ? "on" : "off");
    f_printf(&f, "speed=%s\n", cfg.real_speed ? "real" : "full");
    f_printf(&f, "autoboot=%d\n", cfg.autoboot);
    f_close(&f);
    return true;
}

/* ---------------- USB ---------------- */
static void usb_init(void)
{
    gpio_init(PIN_USB_HOST_5V);
    gpio_set_dir(PIN_USB_HOST_5V, GPIO_OUT);
    gpio_put(PIN_USB_HOST_5V, 1);

    pio_usb_configuration_t pio_cfg = PIO_USB_DEFAULT_CONFIG;
    pio_cfg.pin_dp = PIN_USB_HOST_DP;
    pio_cfg.tx_ch = 2;                  /* DMA 0/1 belong to the video */
    tuh_configure(BOARD_TUH_RHPORT, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &pio_cfg);
    tuh_init(BOARD_TUH_RHPORT);
    tud_init(BOARD_TUD_RHPORT);
}

/* Poll USB, keyboard repeat, and lazy disk flush. */
static void service(void)
{
    tuh_task();
    tud_task();
    kbd_task();
    for (int i = 0; i < CFG_TUD_CDC; i++) tud_cdc_n_write_flush(i);
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (rf_open && (uint32_t)(now - last_rf_flush_ms) > 1000) {
        last_rf_flush_ms = now;
        pdp8_rf_flush();                /* write back the RF08 block cache */
    }
    if ((uint32_t)(now - last_write_ms) > 500) disks_sync();
}

/* Read one character from serial port n (-1 if none), with the usual fix-ups. */
static int cdc_get(int n)
{
    if (!tud_cdc_n_available(n)) return -1;
    int c = tud_cdc_n_read_char(n);
    if (n == 0 && c == 034) return KEY_MENU;        /* Ctrl-\ opens the menu from serial */
    if (c == '\n') return -2;                       /* terminals sending CR LF */
    if (c == 010) c = 0177;                         /* backspace -> RUBOUT */
    if (cfg.upper && c >= 'a' && c <= 'z') c -= 32;
    return c;
}

/* Input for the firmware's own screens: keyboard or serial port 0. */
static int input_get(void)
{
    int k = kbd_get();
    if (k >= 0) return k;
    k = cdc_get(0);
    return k == -2 ? -1 : k;
}

static int wait_key(void)
{
    int k;
    while ((k = input_get()) < 0) service();
    return k;
}

/* ---------------- disk image picker ---------------- */
#define MAX_FILES 40
static char files[MAX_FILES][64];
static int nfiles;

static void scan_images(void)
{
    DIR d;
    FILINFO fi;
    nfiles = 0;
    if (f_opendir(&d, "/") != FR_OK) return;
    while (nfiles < MAX_FILES && f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
        if (fi.fattrib & (AM_DIR | AM_HID | AM_SYS)) continue;
        const char *dot = strrchr(fi.fname, '.');
        if (dot && (!strcasecmp(dot, ".rk05") || !strcasecmp(dot, ".rk"))) {
            if (strlen(fi.fname) >= sizeof files[0]) continue;
            memcpy(files[nfiles], fi.fname, strlen(fi.fname) + 1);
            nfiles++;
        }
    }
    f_closedir(&d);
    for (int i = 0; i < nfiles; i++)
        for (int j = i + 1; j < nfiles; j++)
            if (strcasecmp(files[i], files[j]) > 0) {
                char t[64];
                strcpy(t, files[i]); strcpy(files[i], files[j]); strcpy(files[j], t);
            }
}

static bool pick_image(int unit)
{
    scan_images();
    out_printf("\nSelect image for RK%d:\n", unit);
    out_str("   0  (none - unload drive)\n");
    for (int i = 0; i < nfiles; i++)
        out_printf("  %2d  %s%s\n", i + 1, files[i],
                   !strcasecmp(files[i], cfg.rk[unit]) ? "   <- current" : "");
    out_str("Number (Enter = cancel): ");
    char buf[4]; int n = 0;
    for (;;) {
        int k = wait_key();
        if (k == '\r') break;
        if (k == 27) return false;
        if (k == 0177 && n > 0) { n--; out_str("\b \b"); continue; }
        if (k >= '0' && k <= '9' && n < 2) { buf[n++] = (char)k; ui_char((uint8_t)k); }
    }
    out_str("\n");
    if (n == 0) return false;
    buf[n] = 0;
    int sel = atoi(buf);
    if (sel == 0) { cfg.rk[unit][0] = 0; disk_close(unit); return true; }
    if (sel < 1 || sel > nfiles) return false;
    for (int u = 0; u < RK_NUMDR; u++)
        if (u != unit && !strcasecmp(cfg.rk[u], files[sel - 1])) {
            out_printf("That image is already mounted on RK%d.\n", u);
            sleep_ms(1200);
            return false;
        }
    strcpy(cfg.rk[unit], files[sel - 1]);
    if (!disk_open(unit, cfg.rk[unit])) {
        out_printf("Cannot open %s\n", cfg.rk[unit]);
        sleep_ms(1200);
    }
    return true;
}

static void mount_all(void)
{
    for (int u = 0; u < RK_NUMDR; u++) {
        if (!cfg.rk[u][0]) { disk_close(u); continue; }
        if (!disk_open(u, cfg.rk[u])) {
            if (u == 0) {
                scan_images();
                if (nfiles > 0 && disk_open(0, files[0])) {
                    strcpy(cfg.rk[0], files[0]);
                    continue;
                }
            }
            out_printf("  RK%d: cannot open %s\n", u, cfg.rk[u]);
        }
    }
}

static bool file_exists(const char *name)
{
    FILINFO fi;
    return name[0] && f_stat(name, &fi) == FR_OK;
}

static void show_mounts(void)
{
    for (int u = 0; u < RK_NUMDR; u++)
        out_printf("  RK%d: %s%s\n", u, rk_open[u] ? cfg.rk[u] : "(empty)",
                   rk_open[u] && rk_ro[u] ? "  [read-only]" : "");
    out_printf("  TSS/8: %s + %s%s\n", cfg.tss8_bin, cfg.tss8_rf,
               (file_exists(cfg.tss8_bin) && file_exists(cfg.tss8_rf)) ? "" : "  [not found]");
    out_printf("  UWM TSS/8: %s + %s%s\n", cfg.uwm_bin, cfg.uwm_rf,
               (file_exists(cfg.uwm_bin) && file_exists(cfg.uwm_rf)) ? "" : "  [not found]");
}

/* ---------------- booting ---------------- */
static void apply_colors(void)
{
    video_set_colors(colors[cfg.color].fg, colors[cfg.color].bold, colors[cfg.color].bg);
}

static void reset_terminals(void)
{
    for (int i = 0; i < NTERM; i++) {
        term_init(&terms[i], i);
        terms[i].answerback = answerback;
    }
}

static bool boot_os8(void)
{
    if (!rk_open[0]) return false;
    ptr_rewind();
    rf_close();
    reset_terminals();
    show_terminal(0);
    running = SYS_OS8;
    panel_set_system(sys_name[running]);
    pdp8_boot_rk(0);
    return true;
}

static bool boot_tss8(int sys)
{
    const char *bin = sys == SYS_UWM ? cfg.uwm_bin : cfg.tss8_bin;
    const char *rfn = sys == SYS_UWM ? cfg.uwm_rf : cfg.tss8_rf;
    ui = &terms[0];
    FIL f;
    if (f_open(&f, bin, FA_READ) != FR_OK) {
        out_printf("\nCannot open %s\n", bin);
        return false;
    }
    UINT n = 0;
    FSIZE_t sz = f_size(&f);
    uint8_t *tape = malloc(sz ? (size_t)sz : 1);
    if (!tape) { f_close(&f); return false; }
    f_read(&f, tape, (UINT)sz, &n);
    f_close(&f);
    if (!rf_attach(rfn)) {
        free(tape);
        out_printf("\nCannot open %s\n", rfn);
        return false;
    }
    pdp8_reset();                       /* also drops any stale RF08 cache */
    int w = pdp8_load_bin(tape, n);
    free(tape);
    if (w < 0) {
        out_printf("\n%s is not a valid BIN tape (%d)\n", bin, w);
        rf_close();
        return false;
    }
    reset_terminals();
    for (int i = 1; i < NTERM; i++) {
        term_set_attr(&terms[i], ATTR_REVERSE);
        char hdr[96];
        if (sys == SYS_UWM)
            snprintf(hdr, sizeof hdr,
                     " UWM TSS/8 terminal K%02d  (Alt-F%d)  - press Enter, then Ctrl-B LOGIN 2 LXHE ", i, i + 1);
        else
            snprintf(hdr, sizeof hdr,
                     " TSS/8 terminal K%02d   (Alt-F%d)   - press Enter, then LOGIN 2 LXHE      ", i, i + 1);
        term_puts(&terms[i], hdr);
        term_set_attr(&terms[i], 0);
        term_puts(&terms[i], "\r\n\n");
    }
    show_terminal(0);
    running = sys;
    panel_set_system(sys_name[running]);
    ptr_rewind();
    pdp8_start(024200);
    return true;
}

static bool boot_system(int sys)
{
    return IS_TSS(sys) ? boot_tss8(sys) : boot_os8();
}

/* ---------------- system menu ---------------- */
#define MENU_CONTINUE (-1)          /* otherwise the menu returns the system to boot */

static int system_menu(void)
{
    flush_everything();
    term_init(&menu_term, -1);
    term_t *saved_ui = ui;
    ui = &menu_term;
    video_show(&menu_term);
    int result = MENU_CONTINUE;
    for (;;) {
        term_clear(&menu_term);
        if (tud_cdc_n_connected(0)) cdc_out(0, "\033[2J\033[H", 7);
        term_set_attr(ui, ATTR_REVERSE);
        out_str(" PDP-8/E for Adafruit Fruit Jam  v" VERSION "  -  System Menu                            \n");
        term_set_attr(ui, 0);
        out_printf("\n%s: %s at %05o   AC=%04o L=%o MQ=%04o   %llu instructions\n\n",
                   running >= 0 ? sys_name[running] : "CPU",
                   pdp8.halted ? "HALTED" : "running",
                   (unsigned)(pdp8.halted ? pdp8.halt_pc : (pdp8.ifld | pdp8.pc)),
                   (unsigned)(pdp8.lac & 07777), (unsigned)((pdp8.lac >> 12) & 1), (unsigned)pdp8.mq,
                   (unsigned long long)pdp8.icount);
        show_mounts();
        out_str("\n");
        out_str("  C  Continue\n");
        out_str("  O  Boot OS/8 from RK0 (reset)\n");
        out_str("  T  Boot TSS/8 (DEC TSS/8.24, reset)\n");
        out_str("  U  Boot UWM TSS/8 (TSS/8.25 from the University of Wisconsin-Milwaukee)\n");
        if (running == SYS_OS8) out_str("  R  Restart OS/8 monitor (jump to 07600)\n");
        out_str("  0-3  Change disk image on RK0-RK3\n");
        out_printf("  D  Default system at power-up: %s\n", sys_name[cfg.system]);
        out_printf("  K  Keyboard upper-case: %s\n", cfg.upper ? "ON" : "off");
        out_printf("  S  Speed: %s\n", cfg.real_speed ? "real PDP-8/E (~400K instr/s)" : "full speed");
        out_printf("  P  Phosphor colour: %s\n", colors[cfg.color].name);
        out_str("  W  Write these settings to PDP8.CFG\n");
        out_str("\nChoice: ");
        int k = wait_key();
        if (k >= 'a' && k <= 'z') k -= 32;
        if ((k == 'C' || k == 27 || k == KEY_MENU || k == '\r') && running >= 0) break;
        if (k == 'O' || k == 'B') {
            if (!rk_open[0]) { out_str("\nRK0 is empty - choose an image first.\n"); sleep_ms(1200); continue; }
            result = SYS_OS8; break;
        }
        if (k == 'T' || k == 'U') {
            int sys = k == 'T' ? SYS_TSS8 : SYS_UWM;
            const char *bin = sys == SYS_UWM ? cfg.uwm_bin : cfg.tss8_bin;
            const char *rfn = sys == SYS_UWM ? cfg.uwm_rf : cfg.tss8_rf;
            if (!file_exists(bin) || !file_exists(rfn)) {
                out_printf("\n%s needs %s and %s on the card.\n", sys_name[sys], bin, rfn);
                sleep_ms(1500);
                continue;
            }
            result = sys; break;
        }
        if (k == 'R' && running == SYS_OS8) {
            pdp8.ifld = pdp8.ib = pdp8.df = 0;
            pdp8.pc = 07600;
            pdp8.ion = false;
            pdp8.cif_pending = false;
            pdp8.uf = pdp8.ub = 0;
            pdp8.halted = false;
            break;
        }
        if (k >= '0' && k <= '3') { if (sd_mounted) pick_image(k - '0'); continue; }
        if (k == 'D') { cfg.system = (cfg.system + 1) % NSYS; continue; }
        if (k == 'K') { cfg.upper = !cfg.upper; kbd_set_caps(cfg.upper); continue; }
        if (k == 'S') { cfg.real_speed = !cfg.real_speed; continue; }
        if (k == 'P') { cfg.color = (cfg.color + 1) % NCOLORS; apply_colors(); continue; }
        if (k == 'W') {
            out_str(save_settings() ? "\nSaved.\n" : "\nCould not write PDP8.CFG\n");
            sleep_ms(800);
            continue;
        }
    }
    ui = saved_ui;
    show_terminal(active);
    if (tud_cdc_n_connected(0)) cdc_out(0, "\r\n", 2);
    return result;
}

/* ---------------- boot screen ---------------- */
static void banner(void)
{
    term_clear(ui);
    term_set_attr(ui, ATTR_BOLD);
    out_str("PDP-8/E emulator for the Adafruit Fruit Jam  v" VERSION "\n");
    term_set_attr(ui, 0);
    out_str("32K words, KE8E EAE, KL8E console + 4-line mux, RK8E, RF08, DK8-E clock, LE8, PC8E\n\n");
}

/* returns the system to boot, or -1 for the menu */
static int boot_countdown(void)
{
    int sys = cfg.system;
    if (cfg.autoboot < 0) {
        out_printf("\nEnter = boot %s,  O = OS/8,  T = TSS/8,  U = UWM TSS/8,  F12 (Ctrl-\\ on serial) = menu\n",
                   sys_name[sys]);
    } else {
        out_printf("\nBooting %s in %d s.  O = OS/8,  T = TSS/8,  U = UWM TSS/8,  F12 (Ctrl-\\ on serial) = menu",
                   sys_name[sys], cfg.autoboot);
    }
    absolute_time_t t = make_timeout_time_ms(cfg.autoboot < 0 ? 0x7fffffff : cfg.autoboot * 1000);
    while (!time_reached(t)) {
        service();
        int k = input_get();
        if (k == KEY_MENU) return -1;
        if (k == '\r' || k == ' ') break;
        if (k == 'O' || k == 'o') { sys = SYS_OS8; break; }
        if (k == 'T' || k == 't') { sys = SYS_TSS8; break; }
        if (k == 'U' || k == 'u') { sys = SYS_UWM; break; }
    }
    out_str("\n\n");
    return sys;
}

/* ---------------- main ---------------- */
int main(void)
{
    video_clock_init();

    gpio_init(PIN_LED);
    gpio_set_dir(PIN_LED, GPIO_OUT);
    gpio_init(PIN_BUTTON1);
    gpio_set_dir(PIN_BUTTON1, GPIO_IN);
    gpio_pull_up(PIN_BUTTON1);

    reset_terminals();
    panel_init();
    show_terminal(0);
    video_start_core1();
    apply_colors();
    usb_init();
    banner();

    out_str("Mounting microSD card... ");
    while (!sd_mount()) {
        out_str("\nNo readable microSD card. Insert a FAT32/exFAT card holding OS8.RK05; retrying...");
        for (int i = 0; i < 200; i++) { service(); sleep_ms(10); }
        out_str("\n");
    }
    out_str("ok\n");
    load_settings();
    kbd_set_caps(cfg.upper);
    apply_colors();
    mount_all();
    show_mounts();

    int want = boot_countdown();
    for (;;) {
        if (want >= 0 && boot_system(want)) break;
        if (want >= 0) sleep_ms(1500);
        int m = system_menu();
        want = m;
    }

    absolute_time_t t0 = get_absolute_time();
    uint64_t base_icount = pdp8.icount;
    bool button_was = true;
    bool halt_announced = false;

    for (;;) {
        int menu = -1;

        /* keyboard -> front panel, or the terminal on screen */
        int k;
        while ((k = kbd_get()) >= 0) {
            if (k == KEY_MENU) { menu = 1; break; }
            if (k == KEY_BREAK) continue;
            if (k == KEY_PANEL) { set_panel(!panel_on); continue; }
            if (panel_on && k >= KEY_VT0 && k < KEY_VT0 + NTERM) {   /* Alt-Fn leaves the panel */
                set_panel(false);
                if (IS_TSS(running)) show_terminal(k - KEY_VT0);
                continue;
            }
            if (panel_on) {
                if (k == 27) {                       /* VT52 cursor keys arrive as ESC + letter */
                    int n = kbd_get();
                    if (n == 'A') k = PANEL_KEY_UP;
                    else if (n == 'B') k = PANEL_KEY_DOWN;
                    else if (n >= 0) k = n;
                }
                if (!panel_key(k)) set_panel(false);
                continue;
            }
            if (k >= KEY_VT0 && k < KEY_VT0 + NTERM) {
                if (IS_TSS(running)) show_terminal(k - KEY_VT0);
                continue;
            }
            if (pdp8.halted && !panel_halted) {      /* any key continues after a HLT */
                pdp8.halted = false;
                continue;
            }
            send_key(active, k);
        }
        /* serial port n -> terminal n */
        for (int n = 0; n < NTERM && menu < 0; n++) {
            if (n > 0 && !IS_TSS(running)) {             /* drain unused ports */
                while (tud_cdc_n_available(n)) tud_cdc_n_read_char(n);
                continue;
            }
            int backlog = n == 0 ? pdp8_key_backlog() : pdp8_ttx_backlog(n - 1);
            while (backlog++ < 32) {
                int c = cdc_get(n);
                if (c == -1) break;
                if (c == -2) continue;
                if (c == KEY_MENU) { menu = 1; break; }
                if (n == 0 && pdp8.halted && !panel_halted) { pdp8.halted = false; continue; }
                send_key(n, c);
            }
        }
        /* BUTTON1 also opens the menu */
        bool b = gpio_get(PIN_BUTTON1);
        if (!b && button_was) menu = 1;
        button_was = b;

        if (menu >= 0) {
            bool was_panel = panel_on;
            set_panel(false);
            int m = system_menu();
            while (m != MENU_CONTINUE && !boot_system(m)) {
                sleep_ms(1500);
                m = system_menu();
            }
            if (m == MENU_CONTINUE && was_panel) set_panel(true);
            halt_announced = false;
            t0 = get_absolute_time();
            base_icount = pdp8.icount;
            continue;
        }

        /* run the CPU for ~1 ms worth of work */
        if (pdp8.halted) {
            if (!halt_announced && !panel_halted) {
                flush_everything();
                if (!panel_on) show_terminal(0);
                ui = &terms[0];
                out_printf("\r\n[HALT at %05o - F11 front panel, F12 menu, any other key continues]\r\n",
                           (unsigned)pdp8.halt_pc);
            }
            halt_announced = true;
            t0 = get_absolute_time();
            base_icount = pdp8.icount;
        } else {
            halt_announced = false;
            if (cfg.real_speed) {
                uint64_t us = absolute_time_diff_us(t0, get_absolute_time());
                uint64_t target = base_icount + us * 2 / 5;   /* 400K instructions/s */
                if (pdp8.icount < target) {
                    uint64_t n = target - pdp8.icount;
                    if (n > 4000) n = 4000;
                    pdp8_run((uint32_t)n);
                }
                if (pdp8.icount + 100000 < target) {
                    t0 = get_absolute_time();
                    base_icount = pdp8.icount;
                }
            } else {
                pdp8_run(20000);
            }
        }
        panel_update(to_ms_since_boot(get_absolute_time()));
        service();
    }
}
