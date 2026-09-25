/*
 * Host (Linux/macOS) test harness for the PDP-8/E core.
 *   pdp8host [-u] [-t] rk0.rk05 [rk1.rk05 ...]
 *   pdp8host [-u] [-t] -tss8 rf.dsk init.bin [port]
 *      TSS/8: loads the BIN tape, starts at 24200; multiplexer lines 0-3 are
 *      offered as raw TCP connections on the given port (default 4000).
 *   -u  pass lowercase through (default: fold to uppercase, KSR style)
 *   -7  send console input as 7-bit ASCII (no KSR mark bit)
 *   -t  throttle to roughly real PDP-8/E speed (~400K instructions/s)
 * Ctrl-E exits.  Line printer output goes to lpt.txt.
 */
#include "../core/pdp8.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <fcntl.h>
#include <time.h>
#include <ctype.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/stat.h>

static FILE *disk[RK_NUMDR];
static bool  disk_ro[RK_NUMDR];
static FILE *lpt;

bool host_disk_present(int u) { return u >= 0 && u < RK_NUMDR && disk[u]; }
bool host_disk_readonly(int u) { return disk_ro[u]; }

bool host_disk_read(int u, uint32_t sector, uint16_t *w)
{
    uint8_t b[512];
    memset(b, 0, sizeof b);
    if (fseek(disk[u], (long)sector * 512, SEEK_SET) != 0) return false;
    size_t n = fread(b, 1, 512, disk[u]);
    (void)n;                                 /* short read past EOF = zeros */
    for (int i = 0; i < 256; i++) w[i] = (uint16_t)(b[2 * i] | (b[2 * i + 1] << 8));
    return true;
}

bool host_disk_write(int u, uint32_t sector, const uint16_t *w)
{
    uint8_t b[512];
    for (int i = 0; i < 256; i++) { b[2 * i] = w[i] & 0xff; b[2 * i + 1] = w[i] >> 8; }
    if (fseek(disk[u], (long)sector * 512, SEEK_SET) != 0) return false;
    if (fwrite(b, 1, 512, disk[u]) != 512) return false;
    fflush(disk[u]);
    return true;
}

void host_tty_out(uint8_t c)
{
    c &= 0177;
    if (c == 0 || c == 0177) return;
    putchar(c);
    fflush(stdout);
}

void host_lpt_out(uint8_t c)
{
    if (!lpt) lpt = fopen("lpt.txt", "a");
    if (lpt) { fputc(c & 0177, lpt); fflush(lpt); }
}

/* ---- TSS/8 support: RF08 backing file and multiplexer sockets ---- */
static FILE *rf_file;
static uint32_t rf_words_cap;
static int ttx_fd[4] = { -1, -1, -1, -1 };

uint32_t host_rf_words(void) { return rf_file ? rf_words_cap : 0; }
bool host_rf_read(uint32_t blk, uint16_t *w)
{
    uint8_t b[512];
    memset(b, 0, sizeof b);
    fseek(rf_file, (long)blk * 512, SEEK_SET);
    size_t n = fread(b, 1, 512, rf_file); (void)n;
    for (int i = 0; i < 256; i++) w[i] = (uint16_t)(b[2 * i] | (b[2 * i + 1] << 8));
    return true;
}
bool host_rf_write(uint32_t blk, const uint16_t *w)
{
    uint8_t b[512];
    for (int i = 0; i < 256; i++) { b[2 * i] = w[i] & 0xff; b[2 * i + 1] = w[i] >> 8; }
    fseek(rf_file, (long)blk * 512, SEEK_SET);
    fwrite(b, 1, 512, rf_file);
    return true;
}
void host_ttx_out(int line, uint8_t c)
{
    if (ttx_fd[line] >= 0) {
        char ch = (char)(c & 0177);
        if (write(ttx_fd[line], &ch, 1) < 0) { close(ttx_fd[line]); ttx_fd[line] = -1; }
    }
}

#ifdef PDP8_TRACE
uint32_t pdp8_trace[256][3]; uint32_t pdp8_trace_n;
static void dump_trace(void)
{
    FILE *f = fopen("trace.txt", "w");
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t *t = pdp8_trace[(pdp8_trace_n + i) & 255];
        fprintf(f, "%05o %04o L%o AC=%04o DF=%o\n", t[0], t[1], (t[2] >> 12) & 1, t[2] & 07777, (t[2] >> 13) >> 12);
    }
    fclose(f);
}
#endif
static FILE *ptr_file, *ptp_file;
int host_ptr_getc(void) { return ptr_file ? fgetc(ptr_file) : -1; }
void host_ptp_putc(uint8_t c) { if (!ptp_file) ptp_file = fopen("ptp.out", "ab"); if (ptp_file) { fputc(c, ptp_file); fflush(ptp_file); } }

uint32_t host_millis(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static struct termios saved;
static bool tty_raw;
static void restore(void) { if (tty_raw) tcsetattr(0, TCSANOW, &saved); }

int main(int argc, char **argv)
{
    bool lower = false, throttle = false, tss8 = false, seven = false;
    int nd = 0, port = 4000, listen_fd = -1;
    const char *binfile = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-u")) lower = true;
        else if (!strcmp(argv[i], "-7")) seven = true;
        else if (!strcmp(argv[i], "-ptr") && i + 1 < argc) {
            ptr_file = fopen(argv[++i], "rb");
            if (!ptr_file) { perror(argv[i]); return 1; }
        }
        else if (!strcmp(argv[i], "-tss8") && i + 2 < argc) {
            tss8 = true;
            rf_file = fopen(argv[++i], "r+b");
            if (!rf_file) { perror(argv[i]); return 1; }
            struct stat st; fstat(fileno(rf_file), &st);
            uint32_t words = (uint32_t)(st.st_size / 2);
            uint32_t plat = (words + 262143) / 262144;
            if (plat < 1) plat = 1;
            if (plat > 4) plat = 4;
            rf_words_cap = plat * 262144;
            binfile = argv[++i];
            if (i + 1 < argc && argv[i + 1][0] != '-') port = atoi(argv[++i]);
        }
        else if (!strcmp(argv[i], "-t")) throttle = true;
        else if (nd < RK_NUMDR) {
            disk[nd] = fopen(argv[i], "r+b");
            if (!disk[nd]) { disk[nd] = fopen(argv[i], "rb"); disk_ro[nd] = true; }
            if (!disk[nd]) { perror(argv[i]); return 1; }
            nd++;
        }
    }
    if (nd == 0 && !tss8) { fprintf(stderr, "usage: %s [-u] [-t] rk0.rk05 [rk1.rk05...]\n", argv[0]); return 1; }

    if (isatty(0)) {
        tcgetattr(0, &saved);
        struct termios t = saved;
        cfmakeraw(&t);
        t.c_oflag |= OPOST | ONLCR;
        t.c_oflag &= ~ONLCR;
        tcsetattr(0, TCSANOW, &t);
        tty_raw = true;
        atexit(restore);
    }

    if (tss8) {
        static uint8_t tape[65536];
        FILE *bf = fopen(binfile, "rb");
        if (!bf) { perror(binfile); return 1; }
        size_t n = fread(tape, 1, sizeof tape, bf);
        fclose(bf);
        pdp8_reset();
        int w = pdp8_load_bin(tape, (uint32_t)n);
        if (w < 0) { fprintf(stderr, "bad BIN tape (%d)\n", w); return 1; }
        pdp8_start(024200);
        listen_fd = socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
        if (bind(listen_fd, (struct sockaddr *)&a, sizeof a) < 0 || listen(listen_fd, 4) < 0) { perror("listen"); return 1; }
        fcntl(listen_fd, F_SETFL, O_NONBLOCK);
    } else {
        pdp8_boot_rk(0);
    }
    uint64_t t0 = host_millis(), executed = 0;
    for (;;) {
        struct pollfd p = { .fd = 0, .events = POLLIN };
        if (poll(&p, 1, 0) > 0 && (p.revents & POLLIN)) {
            char buf[64];
            ssize_t n = read(0, buf, sizeof buf);
            if (n <= 0) { /* EOF on a pipe: keep running briefly then exit */
                if (!isatty(0)) { usleep(200000); pdp8_run(2000000); }
                break;
            }
            for (ssize_t i = 0; i < n; i++) {
                int c = (unsigned char)buf[i];
                if (c == 5) goto out;                    /* Ctrl-E */
                if (c == '\n') c = '\r';
                if (!lower && islower(c)) c = toupper(c);
                pdp8_key((uint8_t)(seven ? c : (c | 0200)));
            }
        }
        if (tss8) {
            int fd = accept(listen_fd, NULL, NULL);
            if (fd >= 0) {
                int ln = 0;
                while (ln < 4 && ttx_fd[ln] >= 0) ln++;
                if (ln == 4) close(fd);
                else { fcntl(fd, F_SETFL, O_NONBLOCK); int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one); ttx_fd[ln] = fd; }
            }
            for (int ln = 0; ln < 4; ln++) {
                if (ttx_fd[ln] < 0 || pdp8_ttx_backlog(ln) > 32) continue;
                char b[32];
                ssize_t r = read(ttx_fd[ln], b, sizeof b);
                if (r == 0) { close(ttx_fd[ln]); ttx_fd[ln] = -1; continue; }
                for (ssize_t k = 0; k < r; k++) {
                    int c = (unsigned char)b[k] & 0177;
                    if (c == '\n') continue;
                    if (islower(c)) c = toupper(c);
                    pdp8_ttx_key(ln, (uint8_t)c);
                }
            }
        }
        executed += pdp8_run(throttle ? 4000 : 200000);
        if (pdp8.halted) {
            fprintf(stderr, "\r\nHALT at %05o\r\n", pdp8.halt_pc);
#ifdef PDP8_TRACE
            dump_trace();
#endif
            break;
        }
        if (throttle) {
            uint64_t want_ms = executed / 400;
            uint64_t have = host_millis() - t0;
            if (want_ms > have) usleep((useconds_t)((want_ms - have) * 1000));
        } else if (pdp8_key_backlog() == 0) {
            usleep(1000);                                /* be polite when idle-ish */
        }
    }
out:
    restore();
    for (int i = 0; i < RK_NUMDR; i++) if (disk[i]) fclose(disk[i]);
    pdp8_rf_flush();
    if (rf_file) fclose(rf_file);
    return 0;
}
