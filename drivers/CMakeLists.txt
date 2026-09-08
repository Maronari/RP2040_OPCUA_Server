option(FIRMWARE_ENABLE_NATIVE_INTERNAL_TEMP_DRIVER "Build the native internal-temperature channel driver" ON)
option(FIRMWARE_ENABLE_NATIVE_ADC_VOLTAGE_DRIVER "Build the native ADC-voltage channel driver" ON)
option(FIRMWARE_ENABLE_NATIVE_GPIO_INPUT_DRIVER "Build the native GPIO-input channel driver" ON)
option(FIRMWARE_ENABLE_NATIVE_GPIO_OUTPUT_DRIVER "Build the native GPIO-output (writable) channel driver" ON)
option(FIRMWARE_ENABLE_NATIVE_SYNTHETIC_DRIVER "Build the native synthetic benchmark/loopback channel driver (no hardware dependency)" ON)
option(FIRMWARE_ENABLE_NATIVE_ADS8688_DRIVER "Build the native ADS8688 industrial SPI ADC channel driver (pre-hardware: compile/mock validated only)" ON)
option(FIRMWARE_ENABLE_NATIVE_AD5422_DRIVER "Build the native AD5422 industrial analog-output channel driver (pre-hardware: compile/mock validated only)" ON)
option(FIRMWARE_ENABLE_NATIVE_MAX22190_DRIVER "Build the native MAX22190 industrial digital-input channel driver (pre-hardware: compile/mock validated only)" ON)
option(FIRMWARE_ENABLE_NATIVE_IPS2050_DRIVER "Build the native IPS2050 protected high-side digital-output channel driver (pre-hardware: compile/mock validated only)" ON)
option(FIRMWARE_ENABLE_NATIVE_UIC_DRIVER "Build the native UIC universal-input channel driver (digital input/pulse count/frequency/period; pre-hardware: compile/mock validated only)" ON)
option(FIRMWARE_ENABLE_NATIVE_BOARD_POWER_SERVICE "Build the TCAL9539/TCA4307/TPS2553 board-power-service block (pre-hardware: compile/mock validated only)" ON)
option(FIRMWARE_ENABLE_NATIVE_RS485_TRANSPORT "Build the real RP2350 UART-backed half-duplex RS-485 transport binding for ISOW1432 (pre-hardware: compile/mock validated only)" ON)
option(FIRMWARE_ENABLE_NATIVE_TPS3851_WATCHDOG_DRIVER "Build the TPS3851 external supervisor/watchdog GPIO driver (pre-hardware: compile/mock validated only)" ON)
option(FIRMWARE_ENABLE_NATIVE_BOARD_DIAGNOSTICS_SERVICE "Build the board-level diagnostics collection service (requires FIRMWARE_ENABLE_NATIVE_IPS2050_DRIVER and FIRMWARE_ENABLE_NATIVE_BOARD_POWER_SERVICE; pre-hardware: compile/mock validated only)" ON)

set(FIRMWARE_NATIVE_CHANNEL_DRIVER_SOURCES)
if(FIRMWARE_ENABLE_NATIVE_BOARD_DIAGNOSTICS_SERVICE)
    list(APPEND FIRMWARE_NATIVE_CHANNEL_DRIVER_SOURCES src/board_fault_inputs.c)
endif()
if(FIRMWARE_ENABLE_NATIVE_INTERNAL_TEMP_DRIVER)
    list(APPEND FIRMWARE_NATIVE_CHANNEL_DRIVER_SOURCES src/internal_temperature_channel_driver.c)
endif()
if(FIRMWARE_ENABLE_NATIVE_ADC_VOLTAGE_DRIVER)
    list(APPEND FIRMWARE_NATIVE_CHANNEL_DRIVER_SOURCES src/adc_voltage_channel_driver.c)
endif()
if(FIRMWARE_ENABLE_NATIVE_GPIO_INPUT_DRIVER)
    list(APPEND FIRMWARE_NATIVE_CHANNEL_DRIVER_SOURCES src/gpio_input_channel_driver.c)
endif()
if(FIRMWARE_ENABLE_NATIVE_GPIO_OUTPUT_DRIVER)
    list(APPEND FIRMWARE_NATIVE_CHANNEL_DRIVER_SOURCES src/gpio_output_channel_driver.c)
endif()
if(FIRMWARE_ENABLE_NATIVE_SYNTHETIC_DRIVER)
    list(APPEND FIRMWARE_NATIVE_CHANNEL_DRIVER_SOURCES src/synthetic_driver.c)
endif()
if(FIRMWARE_ENABLE_NATIVE_ADS8688_DRIVER)
    list(APPEND FIRMWARE_NATIVE_CHANNEL_DRIVER_SOURCES src/ads8688_driver.c)
endif()
if(FIRMWARE_ENABLE_NATIVE_AD5422_DRIVER)
    list(APPEND FIRMWARE_NATIVE_CHANNEL_DRIVER_SOURCES src/ad5422_driver.c)
endif()
if(FIRMWARE_ENABLE_NATIVE_MAX22190_DRIVER)
    list(APPEND FIRMWARE_NATIVE_CHANNEL_DRIVER_SOURCES src/max22190_driver.c)
endif()
if(FIRMWARE_ENABLE_NATIVE_IPS2050_DRIVER)
    list(APPEND FIRMWARE_NATIVE_CHANNEL_DRIVER_SOURCES src/ips2050_driver.c)
endif()
if(FIRMWARE_ENABLE_NATIVE_UIC_DRIVER)
    list(APPEND FIRMWARE_NATIVE_CHANNEL_DRIVER_SOURCES src/uic_driver.c src/rp2350_pio_edge_counter.c)
endif()
if(FIRMWARE_ENABLE_NATIVE_BOARD_POWER_SERVICE)
    # tcal9539_driver.c/rp2350_i2c_bus.c are this feature's real hardware
    # backing; i2c_transport.c (the generic, dependency-free bus contract,
    # mirroring spi_transport.c) is always built below regardless of this
    # option, matching that precedent.
    list(APPEND FIRMWARE_NATIVE_CHANNEL_DRIVER_SOURCES src/tcal9539_driver.c src/rp2350_i2c_bus.c)
endif()
if(FIRMWARE_ENABLE_NATIVE_RS485_TRANSPORT)
    # uart_transport.c (the generic, dependency-free bus contract,
    # mirroring spi_transport.c/i2c_transport.c) is always built below
    # regardless of this option; only the real hardware_uart-backed
    # binding is conditional.
    list(APPEND FIRMWARE_NATIVE_CHANNEL_DRIVER_SOURCES src/rp2350_rs485_uart.c)
endif()
if(FIRMWARE_ENABLE_NATIVE_TPS3851_WATCHDOG_DRIVER)
    list(APPEND FIRMWARE_NATIVE_CHANNEL_DRIVER_SOURCES src/tps3851_watchdog_driver.c)
endif()
if(FIRMWARE_ENABLE_NATIVE_ADS8688_DRIVER OR FIRMWARE_ENABLE_NATIVE_AD5422_DRIVER OR FIRMWARE_ENABLE_NATIVE_MAX22190_DRIVER)
    # Shared RP2350 SPI board-binding, reused by every SPI-based industrial
    # driver; excluded from the ELF only when none of them are enabled.
    list(APPEND FIRMWARE_NATIVE_CHANNEL_DRIVER_SOURCES src/rp2350_spi_bus.c)
endif()

add_library(FIRMWARE_DRIVERS STATIC
        src/maintenance_journal_backend.c
        src/history_flash_backend.c
        src/config_transaction_backend.c
        ../services/src/maintenance_journal_codec.c
        src/led_driver.c
        src/platform_clock.c
        src/rtc_driver.c
        src/i2c_transport.c
        src/spi_transport.c
        src/uart_transport.c
        src/temp_sensor_driver.c
        src/w5x00_ethernet_driver.c
        src/watchdog_driver.c
        ${FIRMWARE_NATIVE_CHANNEL_DRIVER_SOURCES}
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

if(FIRMWARE_ENABLE_NATIVE_UIC_DRIVER)
    # Generates uic_pulse_counter.pio.h (pio_program_t + default-config
    # helper) into the build tree and adds it as a public include dir of
    # FIRMWARE_DRIVERS -- see src/uic_pulse_counter.pio and
    # src/rp2350_pio_edge_counter.c.
    pico_generate_pio_header(FIRMWARE_DRIVERS ${CMAKE_CURRENT_SOURCE_DIR}/src/uic_pulse_counter.pio)
endif()

ota_layout_apply_to_target(FIRMWARE_DRIVERS)

target_compile_definitions(FIRMWARE_DRIVERS PUBLIC
        PICO_FLASH_ASSERT_ON_UNSAFE=0
        PICO_FLASH_ASSUME_CORE1_SAFE=1
        FIRMWARE_ENABLE_NATIVE_INTERNAL_TEMP_DRIVER=$<BOOL:${FIRMWARE_ENABLE_NATIVE_INTERNAL_TEMP_DRIVER}>
        FIRMWARE_ENABLE_NATIVE_ADC_VOLTAGE_DRIVER=$<BOOL:${FIRMWARE_ENABLE_NATIVE_ADC_VOLTAGE_DRIVER}>
        FIRMWARE_ENABLE_NATIVE_GPIO_INPUT_DRIVER=$<BOOL:${FIRMWARE_ENABLE_NATIVE_GPIO_INPUT_DRIVER}>
        FIRMWARE_ENABLE_NATIVE_GPIO_OUTPUT_DRIVER=$<BOOL:${FIRMWARE_ENABLE_NATIVE_GPIO_OUTPUT_DRIVER}>
        FIRMWARE_ENABLE_NATIVE_SYNTHETIC_DRIVER=$<BOOL:${FIRMWARE_ENABLE_NATIVE_SYNTHETIC_DRIVER}>
        FIRMWARE_ENABLE_NATIVE_ADS8688_DRIVER=$<BOOL:${FIRMWARE_ENABLE_NATIVE_ADS8688_DRIVER}>
        FIRMWARE_ENABLE_NATIVE_AD5422_DRIVER=$<BOOL:${FIRMWARE_ENABLE_NATIVE_AD5422_DRIVER}>
        FIRMWARE_ENABLE_NATIVE_MAX22190_DRIVER=$<BOOL:${FIRMWARE_ENABLE_NATIVE_MAX22190_DRIVER}>
        FIRMWARE_ENABLE_NATIVE_IPS2050_DRIVER=$<BOOL:${FIRMWARE_ENABLE_NATIVE_IPS2050_DRIVER}>
        FIRMWARE_ENABLE_NATIVE_UIC_DRIVER=$<BOOL:${FIRMWARE_ENABLE_NATIVE_UIC_DRIVER}>
        FIRMWARE_ENABLE_NATIVE_BOARD_POWER_SERVICE=$<BOOL:${FIRMWARE_ENABLE_NATIVE_BOARD_POWER_SERVICE}>
        FIRMWARE_ENABLE_NATIVE_RS485_TRANSPORT=$<BOOL:${FIRMWARE_ENABLE_NATIVE_RS485_TRANSPORT}>
        FIRMWARE_ENABLE_NATIVE_TPS3851_WATCHDOG_DRIVER=$<BOOL:${FIRMWARE_ENABLE_NATIVE_TPS3851_WATCHDOG_DRIVER}>
        FIRMWARE_ENABLE_NATIVE_BOARD_DIAGNOSTICS_SERVICE=$<BOOL:${FIRMWARE_ENABLE_NATIVE_BOARD_DIAGNOSTICS_SERVICE}>
        )

target_link_libraries(FIRMWARE_DRIVERS PUBLIC
        FLASH_OPERATION_COORDINATOR
        pico_stdlib
        pico_aon_timer
        pico_flash
        hardware_adc
        hardware_clocks
        hardware_dma
        hardware_i2c
        hardware_pio
        hardware_spi
        hardware_uart
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
