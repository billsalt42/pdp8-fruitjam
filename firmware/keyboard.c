/*
 * keyboard.c - USB HID boot-protocol keyboard support (TinyUSB host on the
 * Fruit Jam's PIO-USB port) with software auto-repeat.
 */
#include "pico/stdlib.h"
#include "tusb.h"
#include "keyboard.h"

#define KQ_SIZE 64
static uint16_t kq[KQ_SIZE];
static volatile uint32_t kq_head, kq_tail;

static bool caps_lock = true;                 /* default: upper case, like a Teletype */
static uint8_t kbd_addr, kbd_inst;
static bool kbd_present;
static uint8_t leds;
static bool update_leds_later;

static uint8_t rep_key, rep_mod;
static absolute_time_t rep_next;

bool kbd_caps(void) { return caps_lock; }
void kbd_set_caps(bool on) { caps_lock = on; update_leds_later = true; }

static void push(uint16_t k)
{
    uint32_t n = (kq_head + 1) % KQ_SIZE;
    if (n != kq_tail) { kq[kq_head] = k; kq_head = n; }
}

int kbd_get(void)
{
    if (kq_head == kq_tail) return -1;
    uint16_t k = kq[kq_tail];
    kq_tail = (kq_tail + 1) % KQ_SIZE;
    return k;
}

static const uint8_t keymap[128][2] = { HID_KEYCODE_TO_ASCII };

static void update_leds(void)
{
    if (!kbd_present) return;
    leds = caps_lock ? KEYBOARD_LED_CAPSLOCK : 0;
    tuh_hid_set_report(kbd_addr, kbd_inst, 0, HID_REPORT_TYPE_OUTPUT, &leds, 1);
}

static void emit(uint8_t code, uint8_t mod)
{
    bool shift = mod & (KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT);
    bool ctrl  = mod & (KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTCTRL);
    bool alt   = mod & (KEYBOARD_MODIFIER_LEFTALT | KEYBOARD_MODIFIER_RIGHTALT);

    if (code == HID_KEY_F11 || (alt && code == HID_KEY_F6)) { push(KEY_PANEL); return; }
    if (alt && code >= HID_KEY_F1 && code <= HID_KEY_F5) {   /* Alt-F1..F5: switch terminal */
        push((uint16_t)(KEY_VT0 + (code - HID_KEY_F1)));
        return;
    }
    switch (code) {
    case HID_KEY_F12: push(KEY_MENU); return;
    case HID_KEY_CAPS_LOCK: caps_lock = !caps_lock; update_leds(); return;
    case HID_KEY_BACKSPACE: case HID_KEY_DELETE: push(0x7f); return;   /* RUBOUT */
    case HID_KEY_ENTER: case HID_KEY_KEYPAD_ENTER: push('\r'); return;
    case HID_KEY_ESCAPE: push(27); return;
    case HID_KEY_ARROW_UP:    push(27); push('A'); return;               /* VT52 */
    case HID_KEY_ARROW_DOWN:  push(27); push('B'); return;
    case HID_KEY_ARROW_RIGHT: push(27); push('C'); return;
    case HID_KEY_ARROW_LEFT:  push(27); push('D'); return;
    case HID_KEY_F1: push(27); push('P'); return;                          /* PF1-PF4 */
    case HID_KEY_F2: push(27); push('Q'); return;
    case HID_KEY_F3: push(27); push('R'); return;
    case HID_KEY_F4: push(27); push('S'); return;
    case HID_KEY_PAUSE: push(KEY_BREAK); return;
    default: break;
    }
    if (code >= 128) return;
    uint8_t ch = keymap[code][shift ? 1 : 0];
    if (!ch) return;
    if (ctrl) {
        if (ch >= 'a' && ch <= 'z') ch -= 'a' - 1;
        else if (ch >= '@' && ch <= '_') ch -= '@';
        else if (ch == ' ' || ch == '2') ch = 0;
        else if (ch == '6') ch = 036;
        else if (ch == '-') ch = 037;
        else return;
    } else if (caps_lock && ch >= 'a' && ch <= 'z') {
        ch -= 32;
    }
    if (alt) push(27);
    push(ch);
}

static bool in_report(const hid_keyboard_report_t *r, uint8_t code)
{
    for (int i = 0; i < 6; i++) if (r->keycode[i] == code) return true;
    return false;
}

static void process_report(const hid_keyboard_report_t *r)
{
    static hid_keyboard_report_t prev;
    for (int i = 0; i < 6; i++) {
        uint8_t k = r->keycode[i];
        if (k > 1 && !in_report(&prev, k)) {          /* newly pressed (skip rollover err) */
            emit(k, r->modifier);
            if (k != HID_KEY_CAPS_LOCK && k != HID_KEY_F12 && k != HID_KEY_F11) {
                rep_key = k;
                rep_next = make_timeout_time_ms(500);
            }
        }
    }
    if (rep_key && !in_report(r, rep_key)) rep_key = 0;
    rep_mod = r->modifier;
    prev = *r;
}

void kbd_task(void)
{
    if (update_leds_later) { update_leds_later = false; update_leds(); }
    if (rep_key && time_reached(rep_next)) {
        emit(rep_key, rep_mod);
        rep_next = make_timeout_time_ms(40);
    }
}

bool kbd_connected(void) { return kbd_present; }

/* ---------------- TinyUSB host callbacks ---------------- */
void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance, uint8_t const *desc, uint16_t len)
{
    (void)desc; (void)len;
    if (tuh_hid_interface_protocol(dev_addr, instance) == HID_ITF_PROTOCOL_KEYBOARD) {
        kbd_addr = dev_addr;
        kbd_inst = instance;
        kbd_present = true;
        update_leds();
    }
    tuh_hid_receive_report(dev_addr, instance);
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance)
{
    if (dev_addr == kbd_addr && instance == kbd_inst) {
        kbd_present = false;
        rep_key = 0;
    }
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance, uint8_t const *report, uint16_t len)
{
    if (tuh_hid_interface_protocol(dev_addr, instance) == HID_ITF_PROTOCOL_KEYBOARD && len >= 8)
        process_report((const hid_keyboard_report_t *)report);
    tuh_hid_receive_report(dev_addr, instance);
}
