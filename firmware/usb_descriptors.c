/* USB device descriptors: five CDC-ACM serial ports on the USB-C connector.
 *   port 0: PDP-8 console (OS/8 or TSS/8 terminal K00) + system menu
 *   ports 1-4: TSS/8 terminal lines K01-K04 (KL8E multiplexer lines 0-3) */
#include <string.h>
#include "tusb.h"
#include "pico/unique_id.h"

#define USB_VID 0x2E8A    /* Raspberry Pi */
#define USB_PID 0x000A    /* Pico SDK CDC; bcdDevice 2.00 marks the 5-port layout */

static const tusb_desc_device_t desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = USB_VID,
    .idProduct = USB_PID,
    .bcdDevice = 0x0200,
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 3,
    .bNumConfigurations = 1
};

const uint8_t *tud_descriptor_device_cb(void) { return (const uint8_t *)&desc_device; }

#define ITF_NUM_TOTAL (2 * CFG_TUD_CDC)
#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + CFG_TUD_CDC * TUD_CDC_DESC_LEN)

/* CDC n: interfaces 2n/2n+1, notify EP 0x81+2n, data OUT 0x02+2n, data IN 0x82+2n */
#define CDC_DESC(n) TUD_CDC_DESCRIPTOR(2 * (n), 4 + (n), 0x81 + 2 * (n), 8, 0x02 + 2 * (n), 0x82 + 2 * (n), 64)

static const uint8_t desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),
    CDC_DESC(0), CDC_DESC(1), CDC_DESC(2), CDC_DESC(3), CDC_DESC(4),
};

const uint8_t *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return desc_configuration;
}

static const char *string_desc[] = {
    (const char[]){ 0x09, 0x04 },
    "Adafruit Fruit Jam",
    "PDP-8/E Emulator",
    NULL,                        /* serial: board unique id */
    "PDP-8 Console K00",
    "TSS/8 Line K01",
    "TSS/8 Line K02",
    "TSS/8 Line K03",
    "TSS/8 Line K04",
};

static uint16_t desc_str[33];

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;
    uint8_t n;
    if (index == 0) {
        memcpy(&desc_str[1], string_desc[0], 2);
        n = 1;
    } else {
        char serial[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
        const char *s;
        if (index == 3) {
            pico_get_unique_board_id_string(serial, sizeof serial);
            s = serial;
        } else {
            if (index >= sizeof(string_desc) / sizeof(string_desc[0])) return NULL;
            s = string_desc[index];
        }
        n = (uint8_t)strlen(s);
        if (n > 32) n = 32;
        for (uint8_t i = 0; i < n; i++) desc_str[1 + i] = (uint8_t)s[i];
    }
    desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * n + 2));
    return desc_str;
}
