#ifndef VIDEO_H
#define VIDEO_H
#include <stdint.h>
#include <stdbool.h>

#define TEXT_COLS 80
#define TEXT_ROWS 30

/* cell = character (low 8 bits) | attributes */
#define ATTR_REVERSE   0x0100
#define ATTR_BOLD      0x0200
#define ATTR_UNDERLINE 0x0400

struct term;
void video_show(struct term *t);       /* which virtual terminal is on screen */
struct term *video_shown(void);

void video_clock_init(void);          /* call first thing in main (core 0) */
void video_start_core1(void);         /* launches DVI output on core 1 */
void video_set_colors(uint8_t fg_rgb332, uint8_t bold_rgb332, uint8_t bg_rgb332);
void video_flash(void);               /* visual bell */

#endif
