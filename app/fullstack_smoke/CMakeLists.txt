set(TARGET_NAME fullstack_smoke)

add_executable(${TARGET_NAME}
        main.c
        )

target_link_libraries(${TARGET_NAME} PRIVATE
        FIRMWARE_DRIVERS
        FIRMWARE_SERVICES
        OPCUA_DEVICE_MODEL
        )

pico_enable_stdio_usb(${TARGET_NAME} 1)
pico_enable_stdio_uart(${TARGET_NAME} 0)
pico_add_extra_outputs(${TARGET_NAME})

target_compile_definitions(${TARGET_NAME} PRIVATE
        PICO_STDIO_USB_CONNECTION_WITHOUT_DTR=1
        PICO_STDIO_USB_CONNECT_WAIT_TIMEOUT_MS=10000
        PICO_STDIO_USB_POST_CONNECT_WAIT_DELAY_MS=200
        PICO_STDIO_USB_ENABLE_RESET_VIA_VENDOR_INTERFACE=0
        PICO_STDIO_USB_ENABLE_RESET_VIA_BAUD_RATE=0
        )
