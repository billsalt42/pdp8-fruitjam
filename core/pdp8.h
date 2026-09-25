/*
 * pdp8.h - portable PDP-8/E emulator core
 *
 * Emulates a 32K-word PDP-8/E with:
 *   KE8E  extended arithmetic element (mode A and B)
 *   KM8E  memory extension / time-share (user mode traps)
 *   KL8E  console terminal (devices 03/04)
 *   DK8-E line-frequency clock (device 13)
 *   LE8   line printer (device 66)
 *   RK8E  RK05 cartridge disk controller, 4 drives (device 74)
 *   KL8E  x4 terminal multiplexer lines (devices 40-47), as used by TSS/8
 *   RF08  fixed-head disk (devices 60-62, 64), as used by TSS/8
 *   PC8E  high-speed paper tape reader (01) and punch (02)
 *
 * CPU semantics follow the SIMH PDP-8 simulator by Robert M Supnik
 * (MIT-style licence), which is the reference used to validate this core.
 *
 * The host supplies the functions declared under "host interface".
 */
#ifndef PDP8_H
#define PDP8_H

#include <stdint.h>
#include <stdbool.h>

#define PDP8_MEMSIZE   32768u          /* words */
#define RK_NUMDR       4
#define RK_SECTORS     (203u * 2u * 16u) /* 6496 sectors of 256 words */
#define RK_WORDS_PER_SECTOR 256u

/* ---- host interface (implemented by the platform) ---- */
/* Disk: sector = 256 twelve-bit words stored as 16-bit little-endian. */
bool     host_disk_present(int unit);
bool     host_disk_readonly(int unit);
bool     host_disk_read(int unit, uint32_t sector, uint16_t *words);        /* 256 words */
bool     host_disk_write(int unit, uint32_t sector, const uint16_t *words); /* 256 words */
void     host_tty_out(uint8_t ch);          /* 7-bit character from the PDP-8 */
void     host_lpt_out(uint8_t ch);          /* line printer character */
uint32_t host_millis(void);                 /* monotonic milliseconds */
void     host_ttx_out(int line, uint8_t ch);   /* multiplexer line 0-3 output */
/* RF08: backing store in 256-word blocks.  host_rf_words() == 0 -> no RF08. */
uint32_t host_rf_words(void);
bool     host_rf_read(uint32_t block, uint16_t *words);
bool     host_rf_write(uint32_t block, const uint16_t *words);
int      host_ptr_getc(void);                  /* next paper-tape byte, or -1 if none */
void     host_ptp_putc(uint8_t ch);            /* paper-tape punch output */

/* ---- emulator state (exposed for front-panel / debug use) ---- */
typedef struct {
    uint32_t pc;        /* 12-bit */
    uint32_t lac;       /* 13 bits: link + AC */
    uint32_t mq, sc;
    uint32_t ifld, ib, df;   /* field << 12 */
    uint32_t sf;             /* save field */
    uint32_t uf, ub;         /* user mode flag / buffer */
    uint32_t emode, gtf;     /* EAE mode B, greater-than flag */
    uint32_t sr;             /* switch register */
    bool     ion, ion_delay, cif_pending;
    bool     halted;
    uint32_t halt_pc;
    uint64_t icount;         /* instructions executed */
    uint32_t ir;             /* last instruction */
    uint32_t ma, md;         /* last memory address (15-bit) and data word */
} pdp8_cpu_t;

/* Front-panel lamp sampling.  While enabled, the CPU samples its registers
 * every few dozen instructions and counts how often each lamp is lit, so a
 * display can show averaged ("incandescent") brightness.  Index 0 is the
 * most significant bit, as DEC numbers them. */
typedef struct {
    uint32_t samples;
    uint32_t ma[15];         /* EMA 0-2 + memory address 0-11 */
    uint32_t ac[13];         /* link + AC 0-11 */
    uint32_t mq[12], md[12], ir[12], st[12];
    uint32_t ion;
} pdp8_lamps_t;

extern pdp8_cpu_t pdp8;
extern uint16_t   pdp8_mem[PDP8_MEMSIZE];

void     pdp8_reset(void);                       /* power-on reset of CPU + devices */
void     pdp8_boot_rk(int unit);                 /* load RK8E bootstrap and start */
uint32_t pdp8_run(uint32_t max_instructions);    /* returns instructions executed */
void     pdp8_key(uint8_t ch);                   /* queue a keystroke (8-bit, KSR mark bit added by caller) */
int      pdp8_key_backlog(void);
void     pdp8_clock_tick(void);                  /* call at 60 Hz (or clock rate) */
void     pdp8_set_tto_delay(uint32_t instructions);
void     pdp8_ttx_key(int line, uint8_t ch);     /* keystroke for multiplexer line 0-3 */
int      pdp8_ttx_backlog(int line);
void     pdp8_rf_flush(void);                    /* write back cached RF08 blocks */
void     pdp8_rf_invalidate(void);               /* flush + drop the RF08 cache */
int      pdp8_load_bin(const uint8_t *tape, uint32_t len); /* BIN tape -> memory; <0 on error */
void     pdp8_start(uint32_t addr15);            /* set IF/IB and PC, clear halt */
void     pdp8_caf(void);                         /* CLEAR key: as the CAF instruction */
uint32_t pdp8_status_word(void);                 /* 8/E STATUS display word */
void     pdp8_lamps_enable(bool on);
void     pdp8_lamps_take(pdp8_lamps_t *out);     /* copy the counts and clear them */

#endif
