add_library(FIRMWARE_DRIVERS STATIC
        src/flash_storage_driver.c
        src/led_driver.c
        src/platform_clock.c
        src/rtc_driver.c
        src/temp_sensor_driver.c
        src/w5x00_ethernet_driver.c
        src/watchdog_driver.c
        )

target_include_directories(FIRMWARE_DRIVERS PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${PICO_SDK_PATH}/lib/lwip/src/include
        ${PICO_SDK_PATH}/lib/lwip/contrib/ports/freertos/include
        ${PICO_SDK_PATH}/src/rp2_common/pico_lwip/include
        ${PICO_SDK_PATH}/src/rp2_common/pico_rand/include
        )

target_link_libraries(FIRMWARE_DRIVERS PUBLIC
        pico_stdlib
        pico_aon_timer
        hardware_adc
        hardware_clocks
        hardware_dma
        hardware_spi
        hardware_watchdog
        pico_async_context_freertos
        pico_lwip_freertos
        ETHERNET_FILES
        FREERTOS_FILES
        IOLIBRARY_FILES
        LWIP_FILES
        TIMER_FILES
        )
