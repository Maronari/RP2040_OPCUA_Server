set(BOOTLOADER_TARGET ota_bootloader)

add_executable(${BOOTLOADER_TARGET}
        main.c
        )

ota_layout_apply_to_target(${BOOTLOADER_TARGET})
ota_layout_set_bootloader_linker_script(${BOOTLOADER_TARGET})

target_link_libraries(${BOOTLOADER_TARGET} PRIVATE
        OTA_SHARED
        pico_stdlib
        pico_multicore
        hardware_watchdog
        )

pico_enable_stdio_usb(${BOOTLOADER_TARGET} 1)
pico_enable_stdio_uart(${BOOTLOADER_TARGET} 1)
pico_add_extra_outputs(${BOOTLOADER_TARGET})

pico_set_program_name(${BOOTLOADER_TARGET} "ota_bootloader")
pico_set_program_version(${BOOTLOADER_TARGET} "0.1.0")

target_compile_definitions(${BOOTLOADER_TARGET} PRIVATE
        PICO_STDIO_USB_CONNECTION_WITHOUT_DTR=1
        PICO_STDIO_USB_CONNECT_WAIT_TIMEOUT_MS=0
        PICO_STDIO_USB_POST_CONNECT_WAIT_DELAY_MS=0
        PICO_STDIO_USB_STDOUT_TIMEOUT_US=0
        PICO_STDIO_USB_ENABLE_RESET_VIA_VENDOR_INTERFACE=0
        PICO_STDIO_USB_ENABLE_RESET_VIA_BAUD_RATE=0
        )