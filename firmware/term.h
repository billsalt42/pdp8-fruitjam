#ifndef TERM_H
#define TERM_H
#include <stdint.h>
#include <stdbool.h>
#include "video.h"

/* One virtual terminal: an 80x30 screen plus VT52/ANSI parser state. */
typedef struct term {
    volatile uint16_t buf[TEXT_ROWS][TEXT_COLS];
    volatile int cx, cy;
    volatile bool cursor_visible;
    uint16_t attr;
    int state, y_row;
    int csi_args[8], csi_n;
    bool csi_priv;
    int save_x, save_y;
    int id;                                   /* owner-defined */
    void (*answerback)(struct term *t, const char *s);
} term_t;

void term_init(term_t *t, int id);
void term_clear(term_t *t);
void term_putc(term_t *t, uint8_t c);
void term_puts(term_t *t, const char *s);
void term_goto(term_t *t, int x, int y);
void term_set_attr(term_t *t, uint16_t attr);

#endif
