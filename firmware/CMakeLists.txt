cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD adafruit_fruit_jam CACHE STRING "Board")
set(PICO_PLATFORM rp2350-arm-s CACHE STRING "Platform")

include(pico_sdk_import.cmake)
project(pdp8_fruitjam C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()

# Pico-PIO-USB (USB host on the Fruit Jam's USB-A ports)
if (NOT DEFINED PICO_PIO_USB_PATH)
    set(PICO_PIO_USB_PATH ${CMAKE_CURRENT_LIST_DIR}/../lib/Pico-PIO-USB)
endif()
add_subdirectory(${PICO_PIO_USB_PATH} pico_pio_usb)

add_executable(pdp8_fruitjam
    main.c
    video.c
    term.c
    panel.c
    keyboard.c
    sdcard.c
    usb_descriptors.c
    font8x16.c
    ../core/pdp8.c
    fatfs/ff.c
    fatfs/ffunicode.c
    ${PICO_TINYUSB_PATH}/src/portable/raspberrypi/pio_usb/hcd_pio_usb.c
)

target_include_directories(pdp8_fruitjam PRIVATE ${CMAKE_CURRENT_LIST_DIR})
target_compile_definitions(pdp8_fruitjam PRIVATE PIO_USB_USE_TINYUSB)
target_compile_options(pdp8_fruitjam PRIVATE -O2 -Wall)

target_link_libraries(pdp8_fruitjam PRIVATE
    pico_stdlib
    pico_multicore
    pico_unique_id
    hardware_dma
    hardware_spi
    hardware_pll
    hardware_vreg
    pico_pio_usb
    tinyusb_device
    tinyusb_host
)

# Run from RAM: keeps the CPU emulator and video out of the XIP cache's way.
pico_set_binary_type(pdp8_fruitjam copy_to_ram)
pico_set_program_name(pdp8_fruitjam "PDP-8/E emulator for Adafruit Fruit Jam")
pico_add_extra_outputs(pdp8_fruitjam)
