#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

/* Root hub port 0: native USB (USB-C) as a CDC serial console.
 * Root hub port 1: PIO-USB host on GPIO1/2 (the Fruit Jam's USB-A hub). */
#define CFG_TUSB_OS               OPT_OS_PICO

#define CFG_TUD_ENABLED           1
#define BOARD_TUD_RHPORT          0

#define CFG_TUH_ENABLED           1
#define CFG_TUH_RPI_PIO_USB       1
#define BOARD_TUH_RHPORT          1

#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif
#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN        __attribute__ ((aligned(4)))
#endif

/* device */
#define CFG_TUD_ENDPOINT0_SIZE    64
#define CFG_TUD_CDC               5      /* console + four TSS/8 lines */
#define CFG_TUD_CDC_RX_BUFSIZE    256
#define CFG_TUD_CDC_TX_BUFSIZE    512
#define CFG_TUD_CDC_EP_BUFSIZE    64

/* host */
#define CFG_TUH_ENUMERATION_BUFSIZE 256
#define CFG_TUH_HUB                 1
#define CFG_TUH_DEVICE_MAX          (CFG_TUH_HUB ? 4 : 1)
#define CFG_TUH_HID                 4
#define CFG_TUH_HID_EPIN_BUFSIZE    64
#define CFG_TUH_HID_EPOUT_BUFSIZE   64

#endif
