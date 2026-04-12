add_library(FIRMWARE_SERVICES STATIC
        src/alarm_service.c
        src/channel_manager.c
        src/config_service.c
        src/driver_catalog.c
        src/diag_service.c
        src/history_service.c
        src/network_manager.c
        src/security_service.c
        src/sensor_manager.c
        src/time_service.c
        )

target_include_directories(FIRMWARE_SERVICES PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${PICO_SDK_PATH}/lib/lwip/src/include
        ${PICO_SDK_PATH}/lib/lwip/contrib/ports/freertos/include
        ${PICO_SDK_PATH}/src/rp2_common/pico_lwip/include
        ${PICO_SDK_PATH}/src/rp2_common/pico_rand/include
        ${PORT_DIR}/mbedtls/inc
        )

target_compile_definitions(FIRMWARE_SERVICES PUBLIC
        MBEDTLS_USER_CONFIG_FILE="ssl_config.h"
        )

target_link_libraries(FIRMWARE_SERVICES PUBLIC
        FIRMWARE_DRIVERS
        FREERTOS_FILES
        pico_sha256
        pico_mbedtls
        pico_lwip_sntp
        pico_unique_id
        pico_stdlib
        )
