/*
 * term.c - a small glass-TTY / VT52 / ANSI(VT100 subset) terminal emulator
 * that draws into the video text buffer.
 *
 * OS/8 software mostly just prints; editors such as TECO's VTEDIT and KED
 * use VT52 escape sequences; a few later programs emit VT100/ANSI.
 */
#include <string.h>
#include "term.h"

enum { S_NORMAL, S_ESC, S_ESC_Y1, S_ESC_Y2, S_CSI };

/* the parser keeps its position in locals while working, then publishes */
#define cx (t->cx)
#define cy (t->cy)
#define cur_attr (t->attr)
#define state (t->state)
#define y_row (t->y_row)
#define csi_args (t->csi_args)
#define csi_n (t->csi_n)
#define csi_priv (t->csi_priv)
#define save_x (t->save_x)
#define save_y (t->save_y)
#define text_buf (t->buf)
#define cursor_visible (t->cursor_visible)
static inline void sync_cursor(term_t *t) { (void)t; }

static void clear_cells(term_t *t, int row, int from, int to)
{
    for (int x = from; x < to; x++) text_buf[row][x] = ' ';
}

static void scroll_up(term_t *t)
{
    for (int y = 0; y < TEXT_ROWS - 1; y++)
        for (int x = 0; x < TEXT_COLS; x++) text_buf[y][x] = text_buf[y + 1][x];
    clear_cells(t, TEXT_ROWS - 1, 0, TEXT_COLS);
}

static void scroll_down(term_t *t)
{
    for (int y = TEXT_ROWS - 1; y > 0; y--)
        for (int x = 0; x < TEXT_COLS; x++) text_buf[y][x] = text_buf[y - 1][x];
    clear_cells(t, 0, 0, TEXT_COLS);
}

static void linefeed(term_t *t)
{
    if (cy < TEXT_ROWS - 1) cy++;
    else scroll_up(t);
}

void term_clear(term_t *t)
{
    for (int y = 0; y < TEXT_ROWS; y++) clear_cells(t, y, 0, TEXT_COLS);
    cx = cy = 0;
    state = S_NORMAL;
    cur_attr = 0;
    sync_cursor(t);
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void do_csi(term_t *t, char f)
{
    int a0 = csi_n > 0 ? csi_args[0] : 0;
    int n = a0 ? a0 : 1;
    switch (f) {
    case 'A': cy = clampi(cy - n, 0, TEXT_ROWS - 1); break;
    case 'B': cy = clampi(cy + n, 0, TEXT_ROWS - 1); break;
    case 'C': cx = clampi(cx + n, 0, TEXT_COLS - 1); break;
    case 'D': cx = clampi(cx - n, 0, TEXT_COLS - 1); break;
    case 'H': case 'f': {
        int r = (csi_n > 0 && csi_args[0]) ? csi_args[0] : 1;
        int c = (csi_n > 1 && csi_args[1]) ? csi_args[1] : 1;
        cy = clampi(r - 1, 0, TEXT_ROWS - 1);
        cx = clampi(c - 1, 0, TEXT_COLS - 1);
        break;
    }
    case 'J':
        if (a0 == 0) {
            clear_cells(t, cy, cx, TEXT_COLS);
            for (int y = cy + 1; y < TEXT_ROWS; y++) clear_cells(t, y, 0, TEXT_COLS);
        } else if (a0 == 1) {
            for (int y = 0; y < cy; y++) clear_cells(t, y, 0, TEXT_COLS);
            clear_cells(t, cy, 0, cx + 1);
        } else {
            for (int y = 0; y < TEXT_ROWS; y++) clear_cells(t, y, 0, TEXT_COLS);
        }
        break;
    case 'K':
        if (a0 == 0) clear_cells(t, cy, cx, TEXT_COLS);
        else if (a0 == 1) clear_cells(t, cy, 0, cx + 1);
        else clear_cells(t, cy, 0, TEXT_COLS);
        break;
    case 'm':
        if (csi_n == 0) cur_attr = 0;
        for (int i = 0; i < csi_n; i++) {
            switch (csi_args[i]) {
            case 0: cur_attr = 0; break;
            case 1: cur_attr |= ATTR_BOLD; break;
            case 4: cur_attr |= ATTR_UNDERLINE; break;
            case 7: cur_attr |= ATTR_REVERSE; break;
            case 22: cur_attr &= ~ATTR_BOLD; break;
            case 24: cur_attr &= ~ATTR_UNDERLINE; break;
            case 27: cur_attr &= ~ATTR_REVERSE; break;
            }
        }
        break;
    case 's': save_x = cx; save_y = cy; break;
    case 'u': cx = save_x; cy = save_y; break;
    case 'n':
        if (a0 == 6 && t->answerback) {
            char b[16], *p = b;
            int r = cy + 1, c = cx + 1;
            *p++ = 033; *p++ = '[';
            if (r >= 10) *p++ = '0' + r / 10;
            *p++ = '0' + r % 10; *p++ = ';';
            if (c >= 10) *p++ = '0' + c / 10;
            *p++ = '0' + c % 10; *p++ = 'R'; *p = 0;
            if (t->answerback) t->answerback(t, b);
        }
        break;
    case 'h': case 'l':
        if (csi_priv && a0 == 25) cursor_visible = (f == 'h');
        break;
    default: break;
    }
}

void term_putc(term_t *t, uint8_t c)
{
    c &= 0x7f;
    switch (state) {
    case S_ESC:
        state = S_NORMAL;
        switch (c) {
        case 'A': if (cy > 0) cy--; break;                       /* VT52 up */
        case 'B': if (cy < TEXT_ROWS - 1) cy++; break;           /* VT52 down */
        case 'C': if (cx < TEXT_COLS - 1) cx++; break;           /* VT52 right */
        case 'D': if (cx > 0) cx--; break;                       /* VT52 left */
        case 'H': cx = cy = 0; break;                            /* home */
        case 'I': if (cy > 0) cy--; else scroll_down(t); break;   /* reverse LF */
        case 'J':                                                /* erase to EOS */
            clear_cells(t, cy, cx, TEXT_COLS);
            for (int y = cy + 1; y < TEXT_ROWS; y++) clear_cells(t, y, 0, TEXT_COLS);
            break;
        case 'K': clear_cells(t, cy, cx, TEXT_COLS); break;         /* erase to EOL */
        case 'Y': state = S_ESC_Y1; break;                       /* direct cursor */
        case 'Z': if (t->answerback) t->answerback(t, "\033/K"); break;   /* identify: VT52 */
        case '[': state = S_CSI; csi_n = 0; csi_priv = false;
                  memset(csi_args, 0, sizeof csi_args); break;
        case 'M': if (cy > 0) cy--; else scroll_down(t); break;   /* ANSI RI */
        case 'c': term_clear(t); break;
        default: break;                                          /* F G = > < etc. */
        }
        break;
    case S_ESC_Y1:
        y_row = c - 32;
        state = S_ESC_Y2;
        break;
    case S_ESC_Y2:
        cy = clampi(y_row, 0, TEXT_ROWS - 1);
        cx = clampi(c - 32, 0, TEXT_COLS - 1);
        state = S_NORMAL;
        break;
    case S_CSI:
        if (c >= '0' && c <= '9') {
            if (csi_n == 0) csi_n = 1;
            csi_args[csi_n - 1] = csi_args[csi_n - 1] * 10 + (c - '0');
        } else if (c == ';') {
            if (csi_n == 0) csi_n = 1;
            if (csi_n < 8) csi_n++;
        } else if (c == '?') {
            csi_priv = true;
        } else if (c >= 0x40 && c <= 0x7e) {
            do_csi(t, (char)c);
            state = S_NORMAL;
        } else if (c < 0x20) {
            state = S_NORMAL;
        }
        break;
    case S_NORMAL:
        switch (c) {
        case 0: case 0x7f: break;
        case 7: video_flash(); break;
        case 8: if (cx > 0) cx--; break;
        case 9: cx = (cx + 8) & ~7; if (cx >= TEXT_COLS) cx = TEXT_COLS - 1; break;
        case 10: case 11: linefeed(t); break;
        case 12: linefeed(t); break;            /* form feed: VT100 treats it as LF */
        case 13: cx = 0; break;
        case 27: state = S_ESC; break;
        default:
            if (c < 32) break;
            if (cx >= TEXT_COLS) {             /* deferred autowrap */
                cx = 0;
                linefeed(t);
            }
            text_buf[cy][cx] = (uint16_t)(c | cur_attr);
            cx++;
            break;
        }
        break;
    }
    sync_cursor(t);
}

void term_puts(term_t *t, const char *s) { while (*s) term_putc(t, (uint8_t)*s++); }

void term_goto(term_t *t, int x, int y)
{
    cx = clampi(x, 0, TEXT_COLS - 1);
    cy = clampi(y, 0, TEXT_ROWS - 1);
}

void term_set_attr(term_t *t, uint16_t a) { cur_attr = a; }

void term_init(term_t *t, int id)
{
    memset((void *)t, 0, sizeof *t);
    t->id = id;
    cursor_visible = true;
    term_clear(t);
}
