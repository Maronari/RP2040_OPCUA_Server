add_library(FIRMWARE_DRIVERS STATIC
        src/flash_storage_driver.c
        src/maintenance_journal_backend.c
        ../services/src/maintenance_journal_codec.c
        src/led_driver.c
        src/platform_clock.c
        src/rtc_driver.c
        src/temp_sensor_driver.c
        src/w5x00_ethernet_driver.c
        src/watchdog_driver.c
        )

add_library(FLASH_OPERATION_COORDINATOR STATIC
        src/flash_operation_coordinator.c
        )
target_include_directories(FLASH_OPERATION_COORDINATOR PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        )
target_link_libraries(FLASH_OPERATION_COORDINATOR PUBLIC
        FREERTOS_FILES
        pico_flash
        hardware_flash
        )

target_include_directories(FIRMWARE_DRIVERS PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/services/include
        ${PICO_SDK_PATH}/lib/lwip/src/include
        ${PICO_SDK_PATH}/lib/lwip/contrib/ports/freertos/include
        ${PICO_SDK_PATH}/src/rp2_common/pico_lwip/include
        ${PICO_SDK_PATH}/src/rp2_common/pico_rand/include
        )

ota_layout_apply_to_target(FIRMWARE_DRIVERS)

target_compile_definitions(FIRMWARE_DRIVERS PUBLIC
        PICO_FLASH_ASSERT_ON_UNSAFE=0
        PICO_FLASH_ASSUME_CORE1_SAFE=1
        )

target_link_libraries(FIRMWARE_DRIVERS PUBLIC
        FLASH_OPERATION_COORDINATOR
        pico_stdlib
        pico_aon_timer
        pico_flash
        hardware_adc
        hardware_clocks
        hardware_dma
        hardware_spi
        hardware_watchdog
        hardware_flash
        pico_async_context_freertos
        pico_lwip_freertos
        ETHERNET_FILES
        FREERTOS_FILES
        IOLIBRARY_FILES
        LWIP_FILES
        TIMER_FILES
        )
