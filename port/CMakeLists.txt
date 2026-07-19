# ioLibrary_Driver
add_library(IOLIBRARY_FILES STATIC)

target_sources(IOLIBRARY_FILES PRIVATE
        ${PORT_DIR}/ioLibrary_Driver/src/w5x00_spi.c
        ${PORT_DIR}/ioLibrary_Driver/src/w5x00_gpio_irq.c
        )

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

target_sources(LWIP_FILES PRIVATE
        ${PORT_DIR}/lwip/w5x00_lwip.c
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

#open62541
add_library(OPEN62541_FILES STATIC)

target_sources(OPEN62541_FILES PRIVATE
        ${PORT_DIR}/open62541/open62541.c
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
        ${CMAKE_SOURCE_DIR}/open62541_v1.5.3/deps/mqtt-c/include
        ${CMAKE_SOURCE_DIR}/open62541_v1.5.3/deps/mqtt-c/src
        ${PICO_SDK_PATH}/lib/lwip/src/include
        ${PICO_SDK_PATH}/lib/lwip/contrib/ports/freertos/include
        ${PICO_SDK_PATH}/src/rp2_common/pico_lwip/include
        ${PICO_SDK_PATH}/src/rp2_common/pico_rand/include
        ${PORT_DIR}/mbedtls/inc
        )

target_compile_definitions(OPEN62541_FILES PUBLIC
        MBEDTLS_USER_CONFIG_FILE="ssl_config.h"
        UA_LOGLEVEL=300
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

