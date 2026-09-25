/*
 * pdp8.c - portable PDP-8/E emulator core (see pdp8.h)
 *
 * Instruction semantics follow SIMH's pdp8_cpu.c, pdp8_tt.c, pdp8_rk.c,
 * pdp8_clk.c and pdp8_lp.c (Copyright (c) 1993-2013 Robert M Supnik,
 * MIT-style licence).  This is an independent, compact re-implementation
 * intended for microcontrollers.
 */
#include "pdp8.h"
#include <string.h>

pdp8_cpu_t pdp8;
uint16_t   pdp8_mem[PDP8_MEMSIZE];
#define M pdp8_mem

/* ---------------- interrupt bookkeeping ---------------- */
/* devices with an enable mask (dev_done & int_enable) */
#define INT_LPT   (1u << 0)
#define INT_PTP   (1u << 1)
#define INT_PTR   (1u << 2)
#define INT_TTO   (1u << 3)
#define INT_TTI   (1u << 4)
#define INT_CLK   (1u << 5)
#define INT_TTO1  (1u << 6)          /* KL8E multiplexer, output summary */
#define INT_TTI1  (1u << 10)         /* KL8E multiplexer, input summary */
#define INT_DEV_ENABLE ((1u << 14) - 1)
/* direct requests */
#define INT_RK    (1u << 15)
#define INT_RF    (1u << 16)
#define INT_UF    (1u << 23)
#define INT_INIT_ENABLE (INT_TTI | INT_TTO | INT_LPT | INT_PTR | INT_PTP)

static uint32_t int_req, dev_done, int_enable;

static inline void int_update(void)
{
    int_req = (int_req & ~INT_DEV_ENABLE) | (dev_done & int_enable);
}

/* ---------------- simple event scheduler ---------------- */
enum { EV_TTO, EV_LPT, EV_RK0, EV_RK1, EV_RK2, EV_RK3, EV_POLL,
       EV_TTOX0, EV_TTOX1, EV_TTOX2, EV_TTOX3, EV_RF, EV_PCELL, EV_PTR, EV_PTP, EV_COUNT };
static uint64_t ev_time[EV_COUNT];      /* 0 = inactive, else icount deadline */
static uint64_t ev_next = 1;

static void ev_recalc(void)
{
    uint64_t n = UINT64_MAX;
    for (int i = 0; i < EV_COUNT; i++)
        if (ev_time[i] && ev_time[i] < n) n = ev_time[i];
    ev_next = n;
}
static void ev_schedule(int ev, uint32_t delay)
{
    if (delay == 0) delay = 1;
    ev_time[ev] = pdp8.icount + delay;
    if (ev_time[ev] < ev_next) ev_next = ev_time[ev];
}
static bool ev_active(int ev) { return ev_time[ev] != 0; }

/* ---------------- KL8E console ---------------- */
#define KEYQ_SIZE 256
static uint8_t  keyq[KEYQ_SIZE];
static volatile uint32_t keyq_head, keyq_tail;
static uint32_t tti_buf, tto_buf;
static uint32_t tti_time;
static uint32_t tto_wait = 100;
#define POLL_INTERVAL 1000

void pdp8_key(uint8_t ch)
{
    uint32_t n = (keyq_head + 1) % KEYQ_SIZE;
    if (n == keyq_tail) return;              /* full: drop */
    keyq[keyq_head] = ch;
    keyq_head = n;
}
int pdp8_key_backlog(void) { return (int)((keyq_head + KEYQ_SIZE - keyq_tail) % KEYQ_SIZE); }
void pdp8_set_tto_delay(uint32_t n) { tto_wait = n ? n : 1; }

static void tti_poll(void)
{
    if (keyq_head == keyq_tail) return;
    /* like SIMH: don't overwrite an unread character unless it's stale (500 ms) */
    if ((dev_done & INT_TTI) && (uint32_t)(host_millis() - tti_time) < 500) return;
    tti_buf = keyq[keyq_tail];
    keyq_tail = (keyq_tail + 1) % KEYQ_SIZE;
    tti_time = host_millis();
    dev_done |= INT_TTI;
    int_update();
}

static uint32_t iot_tti(uint32_t ir, uint32_t ac, bool *skip)
{

    switch (ir & 7) {
    case 0: dev_done &= ~INT_TTI; int_req &= ~INT_TTI; return ac;           /* KCF */
    case 1: if (dev_done & INT_TTI) *skip = true; return ac;                 /* KSF */
    case 2: dev_done &= ~INT_TTI; int_req &= ~INT_TTI; return 0;             /* KCC */
    case 4: return ac | tti_buf;                                             /* KRS */
    case 5:                                                                  /* KIE */
        if (ac & 1) int_enable |= (INT_TTI | INT_TTO);
        else        int_enable &= ~(INT_TTI | INT_TTO);
        int_update();
        return ac;
    case 6:                                                                  /* KRB */
        dev_done &= ~INT_TTI; int_req &= ~INT_TTI;
        return tti_buf;
    default: return ac;
    }
}

static uint32_t iot_tto(uint32_t ir, uint32_t ac, bool *skip)
{
    switch (ir & 7) {
    case 0: dev_done |= INT_TTO; int_update(); return ac;                    /* TFL */
    case 1: if (dev_done & INT_TTO) *skip = true; return ac;                 /* TSF */
    case 2: dev_done &= ~INT_TTO; int_req &= ~INT_TTO; return ac;            /* TCF */
    case 5: if (int_req & (INT_TTI | INT_TTO)) *skip = true; return ac;      /* SPI */
    case 6: dev_done &= ~INT_TTO; int_req &= ~INT_TTO;                       /* TLS */
        /* fall through */
    case 4:                                                                  /* TPC */
        tto_buf = ac & 0377;
        ev_schedule(EV_TTO, tto_wait);
        return ac;
    default: return ac;
    }
}

static void tto_svc(void)
{
    host_tty_out((uint8_t)(tto_buf & 0177));
    dev_done |= INT_TTO;
    int_update();
}

/* ---------------- DK8-E clock ---------------- */
static uint32_t clk_last_ms;
static uint32_t iot_clk(uint32_t ir, uint32_t ac, bool *skip)
{
    switch (ir & 7) {
    case 1: int_enable |= INT_CLK; int_update(); return ac;                  /* CLEI */
    case 2: int_enable &= ~INT_CLK; int_req &= ~INT_CLK; return ac;          /* CLDI */
    case 3:                                                                  /* CLSC */
        if (dev_done & INT_CLK) {
            dev_done &= ~INT_CLK; int_req &= ~INT_CLK; *skip = true;
        }
        return ac;
    case 5:                                                                  /* CLLE */
        if (ac & 1) int_enable |= INT_CLK; else int_enable &= ~INT_CLK;
        int_update();
        return ac;
    case 6: dev_done &= ~INT_CLK; int_req &= ~INT_CLK; return ac;            /* CLCL */
    case 7: if (dev_done & INT_CLK) *skip = true; return ac;                 /* CLSK */
    default: return ac;
    }
}
void pdp8_clock_tick(void) { dev_done |= INT_CLK; int_update(); }

/* ---------------- LE8 line printer ---------------- */
static uint32_t lpt_buf;
static uint32_t iot_lpt(uint32_t ir, uint32_t ac, bool *skip)
{
    switch (ir & 7) {
    case 0: dev_done |= INT_LPT; int_update(); return ac;                    /* PSKF-ish: set flag */
    case 1: if (dev_done & INT_LPT) *skip = true; return ac;                 /* PSKF */
    case 2: dev_done &= ~INT_LPT; int_req &= ~INT_LPT; return ac;            /* PCLF */
    case 3: return ac;                                                       /* PSKE: never in error */
    case 6: dev_done &= ~INT_LPT; int_req &= ~INT_LPT;                       /* PCLF!PSTB */
        /* fall through */
    case 4:                                                                  /* PSTB */
        lpt_buf = ac & 0177;
        ev_schedule(EV_LPT, (lpt_buf == 015 || lpt_buf == 014 || lpt_buf == 012) ? 500 : 10);
        return ac;
    case 5: int_enable |= INT_LPT; int_update(); return ac;                  /* PSIE */
    case 7: int_enable &= ~INT_LPT; int_req &= ~INT_LPT; return ac;          /* PCIE */
    }
    return ac;
}
static void lpt_svc(void)
{
    host_lpt_out((uint8_t)lpt_buf);
    dev_done |= INT_LPT;
    int_update();
}

/* ---------------- RK8E disk ---------------- */
#define RKS_DONE 04000
#define RKS_HMOV 02000
#define RKS_NRDY 00200
#define RKS_BUSY 00100
#define RKS_WLK  00020
#define RKS_STAT 00002
#define RKS_ERR  (RKS_BUSY | 00040 | RKS_WLK | 00010 | 00004 | RKS_STAT | 00001)
#define RKC_READ  0
#define RKC_RALL  1
#define RKC_WLK   2
#define RKC_SEEK  3
#define RKC_WRITE 4
#define RKC_WALL  5
#define RKC_IE   00400
#define RKC_SKDN 00200
#define RKC_HALF 00100
#define RK_NUMCY 203
#define RK_SWAIT 10
#define RK_RWAIT 10
#define RK_MIN   50

static uint32_t rk_sta, rk_cmd, rk_da, rk_ma, rk_busy;
static uint32_t rk_cyl[RK_NUMDR], rk_func[RK_NUMDR];
static bool     rk_swlk[RK_NUMDR];
static uint16_t rk_secbuf[RK_WORDS_PER_SECTOR];

#define RK_DRIVE(c) (((c) >> 1) & 3)
#define RK_FUNC(c)  (((c) >> 9) & 7)
#define RK_MEX(c)   (((c) & 070) << 9)

static inline void rk_int_update(void)
{
    if ((rk_sta & (RKS_DONE | RKS_ERR)) && (rk_cmd & RKC_IE)) int_req |= INT_RK;
    else int_req &= ~INT_RK;
}

static void rk_go(uint32_t func, uint32_t cyl)
{
    if (func == RKC_RALL) func = RKC_READ;
    if (func == RKC_WALL) func = RKC_WRITE;
    int u = RK_DRIVE(rk_cmd);
    if (!host_disk_present(u)) { rk_sta |= RKS_DONE | RKS_NRDY | RKS_STAT; return; }
    if (ev_active(EV_RK0 + u) || cyl >= RK_NUMCY) { rk_sta |= RKS_DONE | RKS_STAT; return; }
    if (func == RKC_WRITE && (rk_swlk[u] || host_disk_readonly(u))) {
        rk_sta |= RKS_DONE | RKS_WLK; return;
    }
    if (func == RKC_WLK) { rk_swlk[u] = true; rk_sta |= RKS_DONE; return; }
    uint32_t d = (cyl > rk_cyl[u] ? cyl - rk_cyl[u] : rk_cyl[u] - cyl) * RK_SWAIT;
    if (func == RKC_SEEK) {
        ev_schedule(EV_RK0 + u, d > RK_MIN ? d : RK_MIN);
        rk_sta |= RKS_DONE;
    } else {
        ev_schedule(EV_RK0 + u, d + RK_RWAIT);
        rk_busy = 1;
    }
    rk_func[u] = func;
    rk_cyl[u] = cyl;
}

static void rk_svc(int u)
{
    if (rk_func[u] == RKC_SEEK) {
        if (u == (int)RK_DRIVE(rk_cmd) && (rk_cmd & RKC_SKDN)) {
            rk_sta |= RKS_DONE;
            rk_int_update();
        }
        return;
    }
    if (!host_disk_present(u)) {
        rk_sta |= RKS_DONE | RKS_NRDY | RKS_STAT; rk_busy = 0; rk_int_update(); return;
    }
    if (rk_func[u] == RKC_WRITE && (rk_swlk[u] || host_disk_readonly(u))) {
        rk_sta |= RKS_DONE | RKS_WLK; rk_busy = 0; rk_int_update(); return;
    }
    uint32_t field = RK_MEX(rk_cmd);
    uint32_t sector = ((rk_cmd & 1) << 12) | rk_da;
    uint32_t wc = (rk_cmd & RKC_HALF) ? 128 : 256;
    bool ok = true;

    if (rk_func[u] == RKC_READ) {
        ok = host_disk_read(u, sector, rk_secbuf);
        if (!ok) memset(rk_secbuf, 0, sizeof rk_secbuf);
        for (uint32_t i = 0; i < wc; i++)
            M[field | ((rk_ma + i) & 07777)] = rk_secbuf[i] & 07777;
    } else if (rk_func[u] == RKC_WRITE) {
        for (uint32_t i = 0; i < 256; i++)
            rk_secbuf[i] = (i < wc) ? M[field | ((rk_ma + i) & 07777)] : 0;
        ok = host_disk_write(u, sector, rk_secbuf);
    }
    rk_ma = (rk_ma + wc) & 07777;
    rk_sta |= RKS_DONE;
    if (!ok) rk_sta |= RKS_STAT;
    rk_busy = 0;
    rk_int_update();
}

static uint32_t iot_rk(uint32_t ir, uint32_t ac, bool *skip)
{
    switch (ir & 7) {
    case 1:                                                                  /* DSKP */
        if (rk_sta & (RKS_DONE | RKS_ERR)) *skip = true;
        return ac;
    case 2:                                                                  /* DCLR */
        rk_sta = 0;
        switch (ac & 3) {
        case 0: if (rk_busy) rk_sta |= RKS_BUSY; break;
        case 3: break;
        case 1:
            rk_cmd = rk_busy = 0; rk_ma = rk_da = 0;
            for (int i = 0; i < RK_NUMDR; i++) ev_time[EV_RK0 + i] = 0;
            ev_recalc();
            break;
        case 2:
            if (rk_busy) rk_sta |= RKS_BUSY; else rk_go(RKC_SEEK, 0);
            break;
        }
        break;
    case 3:                                                                  /* DLAG */
        if (rk_busy) rk_sta |= RKS_BUSY;
        else { rk_da = ac; rk_go(RK_FUNC(rk_cmd), ((rk_cmd & 1) << 7) | ((rk_da >> 5) & 0177)); }
        break;
    case 4:                                                                  /* DLCA */
        if (rk_busy) rk_sta |= RKS_BUSY; else rk_ma = ac;
        break;
    case 5: {                                                                /* DRST */
        int u = RK_DRIVE(rk_cmd);
        rk_sta &= ~(RKS_HMOV | RKS_NRDY);
        if (!host_disk_present(u)) rk_sta |= RKS_NRDY;
        if (ev_active(EV_RK0 + u)) rk_sta |= RKS_HMOV;
        return rk_sta;
    }
    case 6:                                                                  /* DLDC */
        if (rk_busy) rk_sta |= RKS_BUSY; else { rk_cmd = ac; rk_sta = 0; }
        break;
    case 0: return ac;
    case 7: break;                                                           /* DMAN */
    }
    rk_int_update();
    return 0;
}

static void rk_reset(void)
{
    rk_cmd = rk_ma = rk_da = rk_sta = rk_busy = 0;
    int_req &= ~INT_RK;
    for (int i = 0; i < RK_NUMDR; i++) {
        ev_time[EV_RK0 + i] = 0; rk_swlk[i] = false; rk_cyl[i] = rk_func[i] = 0;
    }
    ev_recalc();
}

/* ---------------- KL8E terminal multiplexer: 4 lines, devices 40-47 ---------------- */
#define TTX_LINES 4
static uint32_t ttix_done, ttox_done, ttx_enbl;
static uint8_t  ttix_buf[TTX_LINES], ttox_buf[TTX_LINES];
#define TTXQ_SIZE 64
static uint8_t  ttxq[TTX_LINES][TTXQ_SIZE];
static volatile uint32_t ttxq_head[TTX_LINES], ttxq_tail[TTX_LINES];

static void ttx_new_flags(uint32_t i, uint32_t o, uint32_t e)
{
    ttix_done = i; ttox_done = o; ttx_enbl = e;
    if (ttix_done & ttx_enbl) dev_done |= INT_TTI1; else dev_done &= ~INT_TTI1;
    if (ttox_done & ttx_enbl) dev_done |= INT_TTO1; else dev_done &= ~INT_TTO1;
    int_enable |= (INT_TTI1 | INT_TTO1);
    int_update();
}

void pdp8_ttx_key(int line, uint8_t ch)
{
    if (line < 0 || line >= TTX_LINES) return;
    uint32_t n = (ttxq_head[line] + 1) % TTXQ_SIZE;
    if (n == ttxq_tail[line]) return;
    ttxq[line][ttxq_head[line]] = ch;
    ttxq_head[line] = n;
}
int pdp8_ttx_backlog(int line)
{
    return (int)((ttxq_head[line] + TTXQ_SIZE - ttxq_tail[line]) % TTXQ_SIZE);
}

static void ttx_poll(void)
{
    for (int ln = 0; ln < TTX_LINES; ln++) {
        if ((ttix_done >> ln) & 1) continue;
        if (ttxq_head[ln] == ttxq_tail[ln]) continue;
        ttix_buf[ln] = ttxq[ln][ttxq_tail[ln]];
        ttxq_tail[ln] = (ttxq_tail[ln] + 1) % TTXQ_SIZE;
        ttx_new_flags(ttix_done | (1u << ln), ttox_done, ttx_enbl);
    }
}

static uint32_t iot_ttx(uint32_t ir, uint32_t ac, bool *skip)
{
    uint32_t dev = (ir >> 3) & 077;
    int ln = (int)((dev - 040) >> 1);
    uint32_t m = 1u << ln;
    if (!(dev & 1)) {                                   /* input side */
        switch (ir & 7) {
        case 0: ttx_new_flags(ttix_done & ~m, ttox_done, ttx_enbl); return ac;       /* KCF */
        case 1: if (ttix_done & m) *skip = true; return ac;                          /* KSF */
        case 2: ttx_new_flags(ttix_done & ~m, ttox_done, ttx_enbl); return 0;        /* KCC */
        case 4: return ac | ttix_buf[ln];                                            /* KRS */
        case 5:                                                                      /* KIE */
            ttx_new_flags(ttix_done, ttox_done, (ac & 1) ? (ttx_enbl | m) : (ttx_enbl & ~m));
            return ac;
        case 6: ttx_new_flags(ttix_done & ~m, ttox_done, ttx_enbl); return ttix_buf[ln]; /* KRB */
        default: return ac;
        }
    } else {                                            /* output side */
        switch (ir & 7) {
        case 0: ttx_new_flags(ttix_done, ttox_done | m, ttx_enbl); return ac;        /* TLF */
        case 1: if (ttox_done & m) *skip = true; return ac;                          /* TSF */
        case 2: ttx_new_flags(ttix_done, ttox_done & ~m, ttx_enbl); return ac;       /* TCF */
        case 5: if (((ttix_done | ttox_done) & m) && (ttx_enbl & m)) *skip = true;   /* SPI */
                return ac;
        case 6: ttx_new_flags(ttix_done, ttox_done & ~m, ttx_enbl);                  /* TLS */
            /* fall through */
        case 4:                                                                      /* TPC */
            ttox_buf[ln] = ac & 0377;
            ev_schedule(EV_TTOX0 + ln, tto_wait);
            return ac;
        default: return ac;
        }
    }
}

static void ttox_svc(int ln)
{
    host_ttx_out(ln, (uint8_t)(ttox_buf[ln] & 0177));
    ttx_new_flags(ttix_done, ttox_done | (1u << ln), ttx_enbl);
}

static void ttx_reset(void)
{
    for (int ln = 0; ln < TTX_LINES; ln++) {
        ttix_buf[ln] = ttox_buf[ln] = 0;
        ev_time[EV_TTOX0 + ln] = 0;
    }
    ttx_new_flags(0, 0, (1u << TTX_LINES) - 1);
}

/* ---------------- RF08 fixed-head disk (devices 60-62, 64) ----------------
 * Words are fetched through a small write-back cache of 256-word blocks;
 * the host supplies the backing store (a file on the SD card). */
#define RF_NUMWD   2048
#define RF_WC      07750
#define RF_MA      07751
#define RFS_PCA    04000
#define RFS_WLS    01000
#define RFS_EIE    00400
#define RFS_PIE    00200
#define RFS_CIE    00100
#define RFS_MEX    00070
#define RFS_DRL    00004
#define RFS_NXD    00002
#define RFS_PER    00001
#define RFS_ERR    (RFS_WLS | RFS_DRL | RFS_NXD | RFS_PER)
#define RF_TIME    10
#define RF_READ    2
#define RF_CACHE   32

static uint32_t rf_sta, rf_da, rf_done = 1, rf_func;
static struct { int32_t blk; bool dirty; uint32_t lru; uint16_t w[256]; } rf_cache[RF_CACHE];
static uint32_t rf_lru_clock;

static inline bool rf_present(void) { return host_rf_words() != 0; }
static inline uint32_t rf_pos(void) { return (uint32_t)((pdp8.icount / RF_TIME) % RF_NUMWD); }

static void rf_update_pcell(void)
{
    if (rf_pos() < 6) rf_sta |= RFS_PCA; else rf_sta &= ~RFS_PCA;
}
static void rf_int_update(void)
{
    if ((rf_done && (rf_sta & RFS_CIE)) || ((rf_sta & RFS_ERR) && (rf_sta & RFS_EIE)) ||
        ((rf_sta & RFS_PCA) && (rf_sta & RFS_PIE)))
        int_req |= INT_RF;
    else int_req &= ~INT_RF;
}

static uint16_t *rf_block(uint32_t blk)
{
    int victim = 0;
    for (int i = 0; i < RF_CACHE; i++) {
        if (rf_cache[i].blk == (int32_t)blk) { rf_cache[i].lru = ++rf_lru_clock; return rf_cache[i].w; }
        if (rf_cache[i].lru < rf_cache[victim].lru) victim = i;
    }
    if (rf_cache[victim].dirty) {
        host_rf_write(rf_cache[victim].blk, rf_cache[victim].w);
        rf_cache[victim].dirty = false;
    }
    if (!host_rf_read(blk, rf_cache[victim].w)) memset(rf_cache[victim].w, 0, 512);
    rf_cache[victim].blk = (int32_t)blk;
    rf_cache[victim].lru = ++rf_lru_clock;
    return rf_cache[victim].w;
}

void pdp8_rf_flush(void)
{
    for (int i = 0; i < RF_CACHE; i++)
        if (rf_cache[i].blk >= 0 && rf_cache[i].dirty) {
            host_rf_write(rf_cache[i].blk, rf_cache[i].w);
            rf_cache[i].dirty = false;
        }
}

void pdp8_rf_invalidate(void)
{
    pdp8_rf_flush();
    for (int i = 0; i < RF_CACHE; i++) { rf_cache[i].blk = -1; rf_cache[i].lru = 0; rf_cache[i].dirty = false; }
}

static uint32_t iot_rf(uint32_t ir, uint32_t ac, bool *skip)
{
    uint32_t dev = (ir >> 3) & 077, pulse = ir & 7;
    if (!rf_present()) return ac;                       /* controller absent */
    rf_update_pcell();
    switch (dev) {
    case 060:
        if (pulse & 1) {                                /* DCMA */
            rf_da &= ~07777u;
            rf_done = 0;
            rf_sta &= ~RFS_ERR;
            rf_int_update();
        }
        if (pulse & 6) {                                /* DMAR, DMAW */
            rf_da |= ac;
            rf_func = pulse & ~1u;
            int32_t t = (int32_t)(rf_da & (RF_NUMWD - 1)) - (int32_t)rf_pos();
            if (t < 0) t += RF_NUMWD;
            ev_schedule(EV_RF, (uint32_t)t * RF_TIME);
            ac = 0;
        }
        return ac;
    case 061:
        switch (pulse) {
        case 1: rf_sta &= 07007; int_req &= ~INT_RF; ev_time[EV_PCELL] = 0; return ac;  /* DCIM */
        case 2: if ((rf_da & (RF_NUMWD - 1)) == rf_pos()) *skip = true; return 0;         /* DSAC */
        case 5:                                                                          /* DIML */
            rf_sta = (rf_sta & 07007) | (ac & 0770);
            if (rf_sta & RFS_PIE) ev_schedule(EV_PCELL, (RF_NUMWD - rf_pos()) * RF_TIME);
            else ev_time[EV_PCELL] = 0;
            rf_int_update();
            return 0;
        case 6: return rf_sta;                                                           /* DIMA */
        }
        return ac;
    case 062:
        if (pulse & 1) { if (rf_sta & RFS_ERR) *skip = true; }                           /* DFSE */
        if (pulse & 2) {                                                                 /* DFSC */
            if (pulse & 4) ac &= ~07777u;
            else if (rf_done) *skip = true;
        }
        if (pulse & 4) ac |= rf_da & 07777;                                              /* DMAC */
        return ac;
    case 064:
        switch (pulse) {
        case 1: rf_da &= 07777; break;                                                   /* DCXA */
        case 3: rf_da &= 07777;                                                          /* DXAL */
            /* fall through */
        case 2: rf_da |= (ac & 0377) << 12; ac = 0; break;
        case 5: ac = 0;                                                                  /* DXAC */
            /* fall through */
        case 4: ac |= (rf_da >> 12) & 0377; break;
        default: break;
        }
        if (rf_da >= host_rf_words()) rf_sta |= RFS_NXD; else rf_sta &= ~RFS_NXD;
        rf_int_update();
        return ac;
    }
    return ac;
}

static void rf_svc(void)
{
    uint32_t mex = (rf_sta & RFS_MEX) << 9;
    uint32_t cap = host_rf_words();
    uint32_t wc;
    rf_update_pcell();
    do {
        if (rf_da >= cap) { rf_sta |= RFS_NXD; break; }
        wc = M[RF_WC] = (M[RF_WC] + 1) & 07777;
        M[RF_MA] = (M[RF_MA] + 1) & 07777;
        uint32_t pa = mex | M[RF_MA];
        uint16_t *b = rf_block(rf_da >> 8);
        if (rf_func == RF_READ) {
            M[pa] = b[rf_da & 0377] & 07777;
        } else {
            b[rf_da & 0377] = M[pa];
            for (int i = 0; i < RF_CACHE; i++)
                if (rf_cache[i].w == b) { rf_cache[i].dirty = true; break; }
        }
        rf_da = (rf_da + 1) & 03777777;
    } while (wc != 0);
    rf_done = 1;
    rf_int_update();
}

static void pcell_svc(void)
{
    rf_sta |= RFS_PCA;
    if (rf_sta & RFS_PIE) {
        ev_schedule(EV_PCELL, RF_NUMWD * RF_TIME);
        int_req |= INT_RF;
    }
}

static void rf_reset(void)
{
    /* Initialize (power-up / CAF) leaves the completion flag clear.  SIMH sets
     * it here, but the UWM TSS/8 monitor attributes any interrupt to the disc
     * while DFSC skips, so a stale flag after CAF crashes it at start-up. */
    rf_sta = rf_da = 0;
    rf_done = 0;
    int_req &= ~INT_RF;
    ev_time[EV_RF] = ev_time[EV_PCELL] = 0;
}

/* ---------------- BIN-format paper tape loader ---------------- */
int pdp8_load_bin(const uint8_t *tape, uint32_t len)
{
    uint32_t i = 0, field = 0, newf = 0, origin = 0, words = 0;
    int hi, lo;
    #define BIN_GETC(v) do { v = -1; \
        while (i < len) { int c_ = tape[i++]; \
            if (rubout) { if (c_ == 0377) rubout = 0; continue; } \
            if (c_ == 0377) { rubout = 1; continue; } \
            if (c_ > 0200) { newf = (uint32_t)(c_ & 070) << 9; continue; } \
            v = c_; break; } } while (0)
    int rubout = 0;
    do { BIN_GETC(hi); if (hi < 0) return -1; } while (hi == 0 || hi >= 0200);
    uint32_t csum = 0;
    for (;;) {
        BIN_GETC(lo); if (lo < 0) return -1;
        uint32_t wd = ((uint32_t)hi << 6) | (uint32_t)lo;
        int t = hi;
        BIN_GETC(hi); if (hi < 0) return -1;
        if (hi == 0200) {                                  /* trailer: wd is checksum */
            if ((csum - wd) & 07777) return -2;
            return (int)words;
        }
        csum += (uint32_t)t + (uint32_t)lo;
        if (wd > 07777) origin = wd & 07777;
        else {
            M[(field | origin) & (PDP8_MEMSIZE - 1)] = (uint16_t)wd;
            origin = (origin + 1) & 07777;
        }
        words++;
        field = newf;
    }
    #undef BIN_GETC
}

void pdp8_start(uint32_t addr)
{
    pdp8.ifld = pdp8.ib = addr & 070000;
    pdp8.df = 0;
    pdp8.pc = addr & 07777;
    pdp8.halted = false;
}

/* ---------------- PC8E paper tape reader (01) and punch (02) ---------------- */
static uint32_t ptr_buf, ptp_buf;
static uint32_t iot_ptr(uint32_t ir, uint32_t ac, bool *skip)
{
    switch (ir & 7) {
    case 0: int_enable |= INT_PTR | INT_PTP; int_update(); return ac;        /* RPE */
    case 1: if (dev_done & INT_PTR) *skip = true; return ac;                 /* RSF */
    case 6: ev_schedule(EV_PTR, 100);                                        /* RFC!RRB */
        /* fall through */
    case 2: dev_done &= ~INT_PTR; int_req &= ~INT_PTR; return ac | ptr_buf;  /* RRB */
    case 4: ev_schedule(EV_PTR, 100);                                        /* RFC */
        dev_done &= ~INT_PTR; int_req &= ~INT_PTR; return ac;
    default: return ac;
    }
}
static void ptr_svc(void)
{
    int c = host_ptr_getc();
    if (c < 0) return;                  /* no tape / end of tape: flag stays clear */
    ptr_buf = (uint32_t)c & 0377;
    dev_done |= INT_PTR;
    int_update();
}
static uint32_t iot_ptp(uint32_t ir, uint32_t ac, bool *skip)
{
    switch (ir & 7) {
    case 0: int_enable &= ~(INT_PTR | INT_PTP); int_update(); return ac;     /* PCE */
    case 1: if (dev_done & INT_PTP) *skip = true; return ac;                 /* PSF */
    case 2: dev_done &= ~INT_PTP; int_req &= ~INT_PTP; return ac;            /* PCF */
    case 6: dev_done &= ~INT_PTP; int_req &= ~INT_PTP;                       /* PLS */
        /* fall through */
    case 4: ptp_buf = ac & 0377; ev_schedule(EV_PTP, 100); return ac;        /* PPC */
    default: return ac;
    }
}
static void ptp_svc(void)
{
    host_ptp_putc((uint8_t)ptp_buf);
    dev_done |= INT_PTP;
    int_update();
}

/* ---------------- device reset (CAF / power-on) ---------------- */
static void devices_reset(void)
{
    tti_buf = 0; tto_buf = 0; lpt_buf = 0;
    dev_done = 0;
    int_req &= ~INT_DEV_ENABLE;
    int_enable = INT_INIT_ENABLE;              /* TTI/TTO/LPT enabled, clock disabled */
    ev_time[EV_TTO] = ev_time[EV_LPT] = ev_time[EV_PTR] = ev_time[EV_PTP] = 0;
    ptr_buf = ptp_buf = 0;
    rk_reset();
    ttx_reset();
    rf_reset();
    if (!ev_active(EV_POLL)) ev_schedule(EV_POLL, POLL_INTERVAL);
}

void pdp8_reset(void)
{
    memset(&pdp8, 0, sizeof pdp8);
    int_req = 0;
    memset(ev_time, 0, sizeof ev_time);
    ev_next = 1;
    devices_reset();
    pdp8_rf_invalidate();
    clk_last_ms = host_millis();
}

void pdp8_boot_rk(int unit)
{
    static const uint16_t boot[] = { 06007, 06744, 01032, 06746, 06743, 01032, 05031, 00000 };
    pdp8_reset();
    for (unsigned i = 0; i < sizeof boot / sizeof boot[0]; i++) M[023 + i] = boot[i];
    M[032] = (uint16_t)((unit & 3) << 1);
    pdp8.pc = 023;
}

/* ---------------- event service ---------------- */
static void service_events(void)
{
    uint64_t now = pdp8.icount;
    for (int i = 0; i < EV_COUNT; i++) {
        if (ev_time[i] && ev_time[i] <= now) {
            ev_time[i] = 0;
            switch (i) {
            case EV_TTO: tto_svc(); break;
            case EV_LPT: lpt_svc(); break;
            case EV_RK0: case EV_RK1: case EV_RK2: case EV_RK3: rk_svc(i - EV_RK0); break;
            case EV_TTOX0: case EV_TTOX1: case EV_TTOX2: case EV_TTOX3: ttox_svc(i - EV_TTOX0); break;
            case EV_RF: rf_svc(); break;
            case EV_PTR: ptr_svc(); break;
            case EV_PTP: ptp_svc(); break;
            case EV_PCELL: pcell_svc(); break;
            case EV_POLL: {
                tti_poll();
                ttx_poll();
                uint32_t ms = host_millis();
                if ((uint32_t)(ms - clk_last_ms) >= 17) {   /* ~60 Hz */
                    clk_last_ms = ms;
                    pdp8_clock_tick();
                }
                ev_time[EV_POLL] = now + POLL_INTERVAL;
                break;
            }
            }
        }
    }
    ev_recalc();
}

/* ---------------- EAE (group 3 OPR) ---------------- */
static inline uint32_t eae_ind_addr(void)
{
    /* mode B memory operand: word after instruction is a pointer (field DF) */
    uint32_t ma = pdp8.ifld | pdp8.pc;
    if ((ma & 07770) != 00010) return pdp8.df | M[ma];
    M[ma] = (M[ma] + 1) & 07777;
    return pdp8.df | M[ma];
}

static void opr_group3(uint32_t ir)
{
    uint32_t temp = pdp8.mq;
    int32_t  st;
    uint32_t ma;
    uint32_t *LAC = &pdp8.lac;

    if (ir & 0200) *LAC &= 010000;                       /* CLA */
    if (ir & 0020) { pdp8.mq = *LAC & 07777; *LAC &= 010000; }  /* MQL */
    if (ir & 0100) *LAC |= temp;                         /* MQA */

    if (ir == 07431) { pdp8.emode = 1; return; }         /* SWAB */
    if (ir == 07447) { pdp8.emode = 0; pdp8.gtf = 0; return; } /* SWBA */
    if (pdp8.emode == 0) pdp8.gtf = 0;

    switch ((ir >> 1) & 027) {
    case 020: *LAC |= pdp8.sc; break;                    /* SCA */
    case 000: break;
    case 021:                                            /* DAD (B) */
        if (pdp8.emode) {
            ma = eae_ind_addr();
            pdp8.mq = pdp8.mq + M[ma];
            ma = pdp8.df | ((ma + 1) & 07777);
            *LAC = (*LAC & 07777) + M[ma] + (pdp8.mq >> 12);
            pdp8.mq &= 07777;
            pdp8.pc = (pdp8.pc + 1) & 07777;
            break;
        }
        *LAC |= pdp8.sc;
        /* fall through */
    case 001:                                            /* ACS (B) / SCL (A) */
        if (pdp8.emode) { pdp8.sc = *LAC & 037; *LAC &= 010000; }
        else { pdp8.sc = (~M[pdp8.ifld | pdp8.pc]) & 037; pdp8.pc = (pdp8.pc + 1) & 07777; }
        break;
    case 022:                                            /* DST (B) */
        if (pdp8.emode) {
            ma = eae_ind_addr();
            M[ma] = pdp8.mq & 07777;
            ma = pdp8.df | ((ma + 1) & 07777);
            M[ma] = *LAC & 07777;
            pdp8.pc = (pdp8.pc + 1) & 07777;
            break;
        }
        *LAC |= pdp8.sc;
        /* fall through */
    case 002:                                            /* MUY */
        ma = pdp8.emode ? eae_ind_addr() : (pdp8.ifld | pdp8.pc);
        temp = pdp8.mq * M[ma] + (*LAC & 07777);
        *LAC = (temp >> 12) & 07777;
        pdp8.mq = temp & 07777;
        pdp8.pc = (pdp8.pc + 1) & 07777;
        pdp8.sc = 014;
        break;
    case 023:                                            /* SWBA (B) handled above */
        if (pdp8.emode) break;
        *LAC |= pdp8.sc;
        /* fall through */
    case 003:                                            /* DVI */
        ma = pdp8.emode ? eae_ind_addr() : (pdp8.ifld | pdp8.pc);
        if ((*LAC & 07777) >= M[ma]) {
            *LAC |= 010000;
            pdp8.mq = ((pdp8.mq << 1) + 1) & 07777;
            pdp8.sc = 0;
        } else {
            temp = ((*LAC & 07777) << 12) | pdp8.mq;
            pdp8.mq = temp / M[ma];
            *LAC = temp % M[ma];
            pdp8.sc = 015;
        }
        pdp8.pc = (pdp8.pc + 1) & 07777;
        break;
    case 024:                                            /* DPSZ (B) */
        if (pdp8.emode) {
            if (((*LAC | pdp8.mq) & 07777) == 0) pdp8.pc = (pdp8.pc + 1) & 07777;
            break;
        }
        *LAC |= pdp8.sc;
        /* fall through */
    case 004:                                            /* NMI */
        temp = (*LAC << 12) | pdp8.mq;
        for (pdp8.sc = 0; (temp & 017777777) != 0 &&
             (temp & 040000000) == ((temp << 1) & 040000000); pdp8.sc++)
            temp <<= 1;
        *LAC = (temp >> 12) & 017777;
        pdp8.mq = temp & 07777;
        if (pdp8.emode && (*LAC & 07777) == 04000 && pdp8.mq == 0) *LAC &= 010000;
        break;
    case 025:                                            /* DPIC (B) */
        if (pdp8.emode) {
            temp = (*LAC + 1) & 07777;
            *LAC = pdp8.mq + (temp == 0);
            pdp8.mq = temp;
            break;
        }
        *LAC |= pdp8.sc;
        /* fall through */
    case 005:                                            /* SHL */
        pdp8.sc = (M[pdp8.ifld | pdp8.pc] & 037) + (pdp8.emode ^ 1);
        if (pdp8.sc > 25) temp = 0;
        else temp = ((*LAC << 12) | pdp8.mq) << pdp8.sc;
        *LAC = (temp >> 12) & 017777;
        pdp8.mq = temp & 07777;
        pdp8.pc = (pdp8.pc + 1) & 07777;
        pdp8.sc = pdp8.emode ? 037 : 0;
        break;
    case 026:                                            /* DCM (B) */
        if (pdp8.emode) {
            temp = (-(int32_t)*LAC) & 07777;
            *LAC = (pdp8.mq ^ 07777) + (temp == 0);
            pdp8.mq = temp;
            break;
        }
        *LAC |= pdp8.sc;
        /* fall through */
    case 006:                                            /* ASR */
        pdp8.sc = (M[pdp8.ifld | pdp8.pc] & 037) + (pdp8.emode ^ 1);
        st = (int32_t)(((*LAC & 07777) << 12) | pdp8.mq);
        if (*LAC & 04000) st |= ~037777777;
        if (pdp8.emode && pdp8.sc != 0) pdp8.gtf = (st >> (pdp8.sc - 1)) & 1;
        if (pdp8.sc > 25) st = (*LAC & 04000) ? -1 : 0;
        else st = st >> pdp8.sc;
        *LAC = ((uint32_t)st >> 12) & 017777;
        pdp8.mq = (uint32_t)st & 07777;
        pdp8.pc = (pdp8.pc + 1) & 07777;
        pdp8.sc = pdp8.emode ? 037 : 0;
        break;
    case 027:                                            /* SAM (B) */
        if (pdp8.emode) {
            temp = *LAC & 07777;
            *LAC = pdp8.mq + (temp ^ 07777) + 1;
            pdp8.gtf = (temp <= pdp8.mq) ^ ((temp ^ pdp8.mq) >> 11);
            break;
        }
        *LAC |= pdp8.sc;
        /* fall through */
    case 007:                                            /* LSR */
        pdp8.sc = (M[pdp8.ifld | pdp8.pc] & 037) + (pdp8.emode ^ 1);
        temp = ((*LAC & 07777) << 12) | pdp8.mq;
        if (pdp8.emode && pdp8.sc != 0) pdp8.gtf = (temp >> (pdp8.sc - 1)) & 1;
        if (pdp8.sc > 24) temp = 0;
        else temp >>= pdp8.sc;
        *LAC = (temp >> 12) & 07777;
        pdp8.mq = temp & 07777;
        pdp8.pc = (pdp8.pc + 1) & 07777;
        pdp8.sc = pdp8.emode ? 037 : 0;
        break;
    }
}

/* ---------------- IOT dispatch ---------------- */
static void do_iot(uint32_t ir)
{
    if (pdp8.uf) { int_req |= INT_UF; return; }          /* user mode trap */

    uint32_t dev = (ir >> 3) & 077, pulse = ir & 7;
    uint32_t ac = pdp8.lac & 07777;
    bool skip = false;

    switch (dev) {
    case 000:
        switch (pulse) {
        case 0: if (pdp8.ion) skip = true; pdp8.ion = false; break;          /* SKON */
        case 1: pdp8.ion = true; pdp8.ion_delay = true; break;               /* ION */
        case 2: pdp8.ion = false; break;                                     /* IOF */
        case 3: if (int_req) skip = true; break;                             /* SRQ */
        case 4:                                                              /* GTF */
            pdp8.lac = (pdp8.lac & 010000) | ((pdp8.lac & 010000) >> 1) |
                       (pdp8.gtf << 10) | ((int_req != 0) << 9) |
                       ((pdp8.ion ? 1u : 0u) << 7) | pdp8.sf;
            break;
        case 5:                                                              /* RTF */
            pdp8.gtf = (pdp8.lac & 02000) >> 10;
            pdp8.ub = (pdp8.lac & 0100) >> 6;
            pdp8.ib = (pdp8.lac & 0070) << 9;
            pdp8.df = (pdp8.lac & 0007) << 12;
            pdp8.lac = ((pdp8.lac & 04000) << 1) | ac;
            pdp8.ion = true;
            pdp8.cif_pending = true;
            break;
        case 6: if (pdp8.gtf) skip = true; break;                            /* SGT */
        case 7:                                                              /* CAF */
            pdp8_caf();
            break;
        }
        break;

    case 020: case 021: case 022: case 023:
    case 024: case 025: case 026: case 027:              /* KM8E memory extension */
        switch (pulse) {
        case 1: pdp8.df = (ir & 0070) << 9; break;                           /* CDF */
        case 2: pdp8.ib = (ir & 0070) << 9; pdp8.cif_pending = true; break;  /* CIF */
        case 3: pdp8.df = pdp8.ib = (ir & 0070) << 9; pdp8.cif_pending = true; break;
        case 4:
            switch (dev & 7) {
            case 0: int_req &= ~INT_UF; break;                               /* CINT */
            case 1: pdp8.lac |= pdp8.df >> 9; break;                         /* RDF */
            case 2: pdp8.lac |= pdp8.ifld >> 9; break;                       /* RIF */
            case 3: pdp8.lac |= pdp8.sf; break;                              /* RIB */
            case 4:                                                          /* RMF */
                pdp8.ub = (pdp8.sf & 0100) >> 6;
                pdp8.ib = (pdp8.sf & 0070) << 9;
                pdp8.df = (pdp8.sf & 0007) << 12;
                pdp8.cif_pending = true;
                break;
            case 5: if (int_req & INT_UF) skip = true; break;                /* SINT */
            case 6: pdp8.ub = 0; pdp8.cif_pending = true; break;             /* CUF */
            case 7: pdp8.ub = 1; pdp8.cif_pending = true; break;             /* SUF */
            }
            break;
        }
        break;

    case 003: pdp8.lac = (pdp8.lac & 010000) | (iot_tti(ir, ac, &skip) & 07777); break;
    case 004: pdp8.lac = (pdp8.lac & 010000) | (iot_tto(ir, ac, &skip) & 07777); break;
    case 001: pdp8.lac = (pdp8.lac & 010000) | (iot_ptr(ir, ac, &skip) & 07777); break;
    case 002: pdp8.lac = (pdp8.lac & 010000) | (iot_ptp(ir, ac, &skip) & 07777); break;
    case 013: pdp8.lac = (pdp8.lac & 010000) | (iot_clk(ir, ac, &skip) & 07777); break;
    case 066: pdp8.lac = (pdp8.lac & 010000) | (iot_lpt(ir, ac, &skip) & 07777); break;
    case 074: pdp8.lac = (pdp8.lac & 010000) | (iot_rk(ir, ac, &skip) & 07777); break;
    case 040: case 041: case 042: case 043: case 044: case 045: case 046: case 047:
        pdp8.lac = (pdp8.lac & 010000) | (iot_ttx(ir, ac, &skip) & 07777); break;
    case 060: case 061: case 062: case 064:
        pdp8.lac = (pdp8.lac & 010000) | (iot_rf(ir, ac, &skip) & 07777); break;
    default: break;                                       /* no such device: no-op */
    }
    if (skip) pdp8.pc = (pdp8.pc + 1) & 07777;
}

/* ---------------- front panel ---------------- */
void pdp8_caf(void)
{
    pdp8.gtf = 0; pdp8.emode = 0;
    pdp8.ion = false; pdp8.ion_delay = false;
    int_req = 0;
    pdp8.lac = 0;
    devices_reset();
}

uint32_t pdp8_status_word(void)
{
    /* PDP-8/E STATUS: L, GT, INT BUS, NO INT, ION, USER, IF0-2, DF0-2 */
    return ((pdp8.lac >> 1) & 04000) | (pdp8.gtf << 10) | ((int_req != 0) << 9) |
           ((pdp8.cif_pending || pdp8.ion_delay) << 8) | ((pdp8.ion ? 1u : 0u) << 7) |
           (pdp8.uf << 6) | (pdp8.ifld >> 9) | (pdp8.df >> 12);
}

static bool lamps_on;
static uint32_t lamp_cd = 1, lamp_lfsr = 0xace1u;
static pdp8_lamps_t lamps;

static inline void lamp_bits(uint32_t *c, uint32_t v, int n)
{
    for (int i = 0; i < n; i++)
        c[i] += (v >> (n - 1 - i)) & 1;
}

static void lamps_sample(void)
{
    /* next sample in 32..63 instructions, jittered so it can't lock onto a loop */
    lamp_lfsr = (lamp_lfsr >> 1) ^ (-(lamp_lfsr & 1u) & 0xb400u);
    lamp_cd = 32 + (lamp_lfsr & 31);
    lamps.samples++;
    lamp_bits(lamps.ma, pdp8.ma & 077777, 15);
    lamp_bits(lamps.ac, pdp8.lac & 017777, 13);
    lamp_bits(lamps.mq, pdp8.mq, 12);
    lamp_bits(lamps.md, pdp8.md, 12);
    lamp_bits(lamps.ir, pdp8.ir, 12);
    lamp_bits(lamps.st, pdp8_status_word(), 12);
    lamps.ion += pdp8.ion;
}

void pdp8_lamps_enable(bool on) { lamps_on = on; lamp_cd = 1; }

void pdp8_lamps_take(pdp8_lamps_t *out)
{
    *out = lamps;
    memset(&lamps, 0, sizeof lamps);
}

/* ---------------- main loop ---------------- */
uint32_t pdp8_run(uint32_t max)
{
    uint32_t n = 0;
    if (pdp8.halted) return 0;

    while (n < max) {
        uint32_t ma = pdp8.ifld | pdp8.pc;
        uint32_t ir = M[ma];
#ifdef PDP8_TRACE
        { extern uint32_t pdp8_trace[256][3]; extern uint32_t pdp8_trace_n;
          uint32_t *t = pdp8_trace[pdp8_trace_n++ & 255]; t[0] = ma; t[1] = ir; t[2] = pdp8.lac | (pdp8.df << 1); }
#endif
        uint32_t ea, mb;
        pdp8.pc = (pdp8.pc + 1) & 07777;
        pdp8.ir = ir;
        pdp8.ma = ma;
        pdp8.md = ir;
        bool ion_delay_was = pdp8.ion_delay;
        pdp8.ion_delay = false;

        switch (ir >> 9) {
        case 0: case 1: case 2: case 3:                    /* AND TAD ISZ DCA */
            ea = (ir & 0200) ? ((ma & 07600) | (ir & 0177)) : (ir & 0177);
            if (ir & 0400) {
                ea |= pdp8.ifld;
                mb = M[ea];
                if ((ea & 07770) == 00010) { mb = (mb + 1) & 07777; M[ea] = mb; }
                ea = pdp8.df | mb;
            } else ea |= pdp8.ifld;
            switch (ir >> 9) {
            case 0: pdp8.lac &= (M[ea] | 010000); break;
            case 1: pdp8.lac = (pdp8.lac + M[ea]) & 017777; break;
            case 2:
                mb = (M[ea] + 1) & 07777; M[ea] = mb;
                if (mb == 0) pdp8.pc = (pdp8.pc + 1) & 07777;
                break;
            case 3: M[ea] = pdp8.lac & 07777; pdp8.lac &= 010000; break;
            }
            pdp8.ma = ea;
            pdp8.md = M[ea];
            break;

        case 4:                                            /* JMS */
        case 5:                                            /* JMP */
            ea = (ir & 0200) ? ((ma & 07600) | (ir & 0177)) : (ir & 0177);
            if (ir & 0400) {
                uint32_t pa = pdp8.ifld | ea;
                mb = M[pa];
                if ((pa & 07770) == 00010) { mb = (mb + 1) & 07777; M[pa] = mb; }
                ea = mb;
            }
            pdp8.ifld = pdp8.ib;
            pdp8.uf = pdp8.ub;
            pdp8.cif_pending = false;
            if ((ir >> 9) == 4) {
                M[pdp8.ifld | ea] = (uint16_t)pdp8.pc;
                pdp8.pc = (ea + 1) & 07777;
            } else {
                pdp8.pc = ea;
            }
            pdp8.ma = pdp8.ifld | ea;
            pdp8.md = M[pdp8.ma];
            break;

        case 6:                                            /* IOT */
            do_iot(ir);
            break;

        case 7:                                            /* OPR */
            if (!(ir & 0400)) {                            /* group 1 */
                uint32_t L = pdp8.lac;
                if (ir & 0200) L &= 010000;
                if (ir & 0100) L &= 007777;
                if (ir & 0040) L ^= 007777;
                if (ir & 0020) L ^= 010000;
                if (ir & 0001) L = (L + 1) & 017777;
                switch (ir & 016) {
                case 002: L = (L & 010000) | ((L >> 6) & 077) | ((L & 077) << 6); break; /* BSW */
                case 004: L = ((L << 1) | (L >> 12)) & 017777; break;               /* RAL */
                case 006: L = ((L << 2) | (L >> 11)) & 017777; break;               /* RTL */
                case 010: L = ((L >> 1) | (L << 12)) & 017777; break;               /* RAR */
                case 012: L = ((L >> 2) | (L << 11)) & 017777; break;               /* RTR */
                case 014: L = L & (ir | 010000); break;
                case 016: L = (L & 010000) | (ma & 07600) | (ir & 0177); break;
                }
                pdp8.lac = L;
            } else if (!(ir & 1)) {                        /* group 2 */
                uint32_t L = pdp8.lac;
                bool cond;
                bool sma = ir & 0100, sza = ir & 0040, snl = ir & 0020;
                cond = (sma && (L & 04000)) || (sza && (L & 07777) == 0) || (snl && (L & 010000));
                if (ir & 0010) cond = !cond;               /* reverse sense */
                if (cond) pdp8.pc = (pdp8.pc + 1) & 07777;
                if (ir & 0200) pdp8.lac &= 010000;         /* CLA */
                if (ir & 06) {
                    if (pdp8.uf) int_req |= INT_UF;
                    else {
                        if (ir & 04) pdp8.lac |= pdp8.sr;  /* OSR */
                        if (ir & 02) {                     /* HLT */
                            pdp8.halted = true;
                            pdp8.halt_pc = pdp8.ifld | ((pdp8.pc - 1) & 07777);
                        }
                    }
                }
            } else {
                opr_group3(ir);
            }
            break;
        }

        n++;
        pdp8.icount++;
        if (lamps_on && --lamp_cd == 0) lamps_sample();
        if (pdp8.icount >= ev_next) service_events();

        /* interrupt? */
        if (pdp8.ion && !pdp8.ion_delay && !pdp8.cif_pending && int_req) {
            pdp8.ion = false;
            pdp8.sf = (pdp8.uf << 6) | (pdp8.ifld >> 9) | (pdp8.df >> 12);
            pdp8.ifld = pdp8.ib = pdp8.df = pdp8.uf = pdp8.ub = 0;
            M[0] = (uint16_t)pdp8.pc;
            pdp8.pc = 1;
        }
        (void)ion_delay_was;
        if (pdp8.halted) break;
    }
    return n;
}
