# ioLibrary_Driver
# FIRMWARE_ENABLE_INDUSTRIAL_W5500_PINS is not an option: the root
# CMakeLists.txt derives it from FIRMWARE_DRIVER_PACK (industrial -> ON,
# base -> OFF), so the Ethernet pins always match the board the pack is for.

add_library(IOLIBRARY_FILES STATIC)

target_sources(IOLIBRARY_FILES PRIVATE
        ${PORT_DIR}/ioLibrary_Driver/src/w5x00_spi.c
        ${PORT_DIR}/ioLibrary_Driver/src/w5x00_gpio_irq.c
        )

if(FIRMWARE_ENABLE_INDUSTRIAL_W5500_PINS)
    # RP2350_INDUSTRIAL_MVP_V1's frozen GPIO map (HARDWARE_MVP.md):
    # GPIO4 ETH_MISO, GPIO5 ETH_CS_N, GPIO6 ETH_SCK, GPIO7 ETH_MOSI,
    # GPIO8 ETH_INT_N -- all four SPI pins remain on hardware SPI0 (the
    # same repeating GPIO-function-group pattern that also puts the
    # reference module's GPIO0..GPIO3 default on SPI0), so SPI_PORT itself
    # is unchanged.
    target_compile_definitions(IOLIBRARY_FILES PUBLIC
            PIN_MISO=4
            PIN_CS=5
            PIN_SCK=6
            PIN_MOSI=7
            PIN_INT=8
            )
endif()

target_include_directories(IOLIBRARY_FILES PUBLIC
        ${WIZNET_DIR}/Ethernet
        ${PORT_DIR}/ioLibrary_Driver/inc
        ${PORT_DIR}
        )

target_link_libraries(IOLIBRARY_FILES PRIVATE
        pico_stdlib
        hardware_spi
        hardware_dma
        hardware_clocks
        FREERTOS_FILES
        )

add_library(PSRAM_ALLOCATOR STATIC)

target_sources(PSRAM_ALLOCATOR PRIVATE
        ${PORT_DIR}/psram/psram_allocator.c
        )

target_include_directories(PSRAM_ALLOCATOR PUBLIC
        ${PORT_DIR}/psram
        )

target_link_libraries(PSRAM_ALLOCATOR PRIVATE
        FREERTOS_FILES
        pico_stdlib
        hardware_clocks
        hardware_gpio
        hardware_sync
        )

add_library(UA_PORT_ALLOC STATIC)

target_sources(UA_PORT_ALLOC PRIVATE
        ${PORT_DIR}/FreeRTOS-Kernel/inc/ua_port_alloc/ua_port_alloc.c
        )

target_include_directories(UA_PORT_ALLOC PUBLIC
        ${PORT_DIR}/FreeRTOS-Kernel/inc/ua_port_alloc
        )

target_link_libraries(UA_PORT_ALLOC PRIVATE
        FREERTOS_FILES
        PSRAM_ALLOCATOR
        pico_stdlib
        )

# lwip
add_library(LWIP_FILES STATIC)

if(W5X00_DIAGNOSTIC_NO_OPCUA)
    target_compile_definitions(LWIP_FILES PRIVATE W5X00_LWIP_ALL_DEBUG)
endif()

target_sources(LWIP_FILES PRIVATE
        ${PORT_DIR}/lwip/w5x00_lwip.c
        ${PORT_DIR}/lwip/lwip_psram_heap.c
        )

target_include_directories(LWIP_FILES PUBLIC
        ${WIZNET_DIR}/Ethernet
        ${PORT_DIR}
        ${PORT_DIR}/lwip
        ${PICO_SDK_PATH}/lib/lwip/src/include
        ${PICO_SDK_PATH}/lib/lwip/contrib/ports/freertos/include
        ${PICO_SDK_PATH}/src/rp2_common/pico_lwip/include
        ${PICO_SDK_PATH}/src/rp2_common/pico_rand/include
        )

target_link_libraries(LWIP_FILES PUBLIC
        FREERTOS_FILES
        ETHERNET_FILES
        PSRAM_ALLOCATOR
        pico_lwip
        pico_lwip_nosys
        pico_lwip_contrib_freertos_headers
        pico_lwip_freertos
        )

# timer
add_library(TIMER_FILES STATIC)

target_sources(TIMER_FILES PRIVATE
        ${PORT_DIR}/timer/timer.c
        )

target_include_directories(TIMER_FILES PUBLIC
        ${PORT_DIR}/timer
        )

target_link_libraries(TIMER_FILES PRIVATE
        pico_stdlib      
        )

if(NOT W5X00_DIAGNOSTIC_NO_OPCUA)
    # MQTT-C is included directly by the open62541 amalgamation when PubSub MQTT
    # transport is enabled. Pin the same revision used by open62541 v1.5.3 so a
    # clean build does not depend on the ignored open62541 source checkout.
    include(FetchContent)
    cmake_policy(PUSH)
    if(POLICY CMP0135)
        cmake_policy(SET CMP0135 NEW)
    endif()
    if(POLICY CMP0169)
        cmake_policy(SET CMP0169 OLD)
    endif()
    FetchContent_Declare(
            mqtt_c
            URL https://github.com/LiamBindle/MQTT-C/archive/0f4c34c8cc00b16cfee094745d68b8cdbaecd8e0.tar.gz
            URL_HASH SHA256=985898405912dbddf50d8b446226763696e6390fbd6f38b66cede6f38e703086
            )

    # CMake 3.30 deprecated direct population, but it is required while this
    # project supports CMake 3.13 and avoids building MQTT-C's host PAL target.
    FetchContent_GetProperties(mqtt_c)
    if(NOT mqtt_c_POPULATED)
        FetchContent_Populate(mqtt_c)
    endif()
    cmake_policy(POP)

    #open62541
    add_library(OPEN62541_FILES STATIC)

    target_sources(OPEN62541_FILES PRIVATE
            ${PORT_DIR}/open62541/open62541.c
            ${PORT_DIR}/open62541/rp2_ua_datetime_clock.c
            ${PORT_DIR}/open62541/nodesetinjector.c
            ${PORT_DIR}/mbedtls/mbedtls_alt_alloc.c
            ${PORT_DIR}/mbedtls/mbedtls_alt_time.c
            )

    target_include_directories(OPEN62541_FILES PUBLIC
            ${PORT_DIR}/open62541
            ${WIZNET_DIR}/Ethernet
            ${PORT_DIR}
            ${PORT_DIR}/lwip
            ${PORT_DIR}/open62541/include
            ${CMAKE_SOURCE_DIR}/services/include
            ${mqtt_c_SOURCE_DIR}/include
            ${mqtt_c_SOURCE_DIR}/src
            ${PICO_SDK_PATH}/lib/lwip/src/include
            ${PICO_SDK_PATH}/lib/lwip/contrib/ports/freertos/include
            ${PICO_SDK_PATH}/src/rp2_common/pico_lwip/include
            ${PICO_SDK_PATH}/src/rp2_common/pico_rand/include
            ${PORT_DIR}/mbedtls/inc
            )

    target_compile_definitions(OPEN62541_FILES PUBLIC
            MBEDTLS_USER_CONFIG_FILE="ssl_config.h"
            UA_LOGLEVEL=300
            UA_ENABLE_NODESET_INJECTOR
            RP2_OPEN62541_SKIP_DIAGNOSTICS_NODES=1
            )

    target_link_libraries(OPEN62541_FILES
            PUBLIC
            UA_PORT_ALLOC
            PRIVATE
            pico_lwip
            pico_lwip_nosys
            pico_lwip_contrib_freertos_headers
            FREERTOS_FILES
            pico_stdlib
            pico_lwip_contrib_freertos
            pico_mbedtls
            )
endif()

