#ifndef KEYBOARD_H
#define KEYBOARD_H
#include <stdbool.h>

#define KEY_MENU  0x100   /* F12: system menu */
#define KEY_BREAK 0x101   /* Pause/Break */
#define KEY_PANEL 0x102   /* F11 or Alt-F6: front panel */
#define KEY_VT0   0x110   /* Alt-F1 .. Alt-F5 -> KEY_VT0 .. KEY_VT0+4 */

int  kbd_get(void);       /* -1 if none; char or KEY_* */
void kbd_task(void);      /* auto-repeat; call often */
bool kbd_caps(void);
void kbd_set_caps(bool on);
bool kbd_connected(void);

#endif
