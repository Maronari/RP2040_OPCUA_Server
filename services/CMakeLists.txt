set(FIRMWARE_SERVICES_SOURCES
        src/alarm_service.c
        src/channel_manager.c
        src/config_export_service.c
        src/config_compat.c
        src/config_import_service.c
        src/config_presets.c
        src/config_reset_service.c
        src/config_service.c
        src/config_store_service.c
        src/config_validate_service.c
        src/driver_catalog.c
        src/diag_service.c
        src/device_health_service.c
        src/firmware_update_service.c
        src/history_service.c
        src/maintenance_service.c
        src/network_manager.c
        src/security_service.c
        src/security_state_service.c
        src/security_trust_expiry_service.c
        src/security_trust_import_service.c
        src/sensor_manager.c
        src/time_service.c
        )

if(FIRMWARE_ENABLE_NATIVE_BOARD_DIAGNOSTICS_SERVICE AND FIRMWARE_ENABLE_NATIVE_IPS2050_DRIVER)
    # board_diagnostics_service.c calls into ips2050_driver.h
    # (native_ips2050_read_fault()) directly, so it is only ever compiled in
    # when that dependency is -- never link-fails from a partial dependency
    # set.
    list(APPEND FIRMWARE_SERVICES_SOURCES src/board_diagnostics_service.c)
endif()

if(NOT W5X00_DIAGNOSTIC_NO_OPCUA)
    list(APPEND FIRMWARE_SERVICES_SOURCES
            src/security_identity_import_service.c
            src/security_certificate_group_service.c
            )
endif()

add_library(FIRMWARE_SERVICES STATIC
        ${FIRMWARE_SERVICES_SOURCES}
        )

target_include_directories(FIRMWARE_SERVICES PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${PICO_SDK_PATH}/lib/lwip/src/include
        ${PICO_SDK_PATH}/lib/lwip/contrib/ports/freertos/include
        ${PICO_SDK_PATH}/src/rp2_common/pico_lwip/include
        ${PICO_SDK_PATH}/src/rp2_common/pico_rand/include
        ${PORT_DIR}/mbedtls/inc
        )

target_include_directories(FIRMWARE_SERVICES PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        )

ota_layout_apply_to_target(FIRMWARE_SERVICES)

target_compile_definitions(FIRMWARE_SERVICES PUBLIC
        MBEDTLS_USER_CONFIG_FILE="ssl_config.h"
        )

target_link_libraries(FIRMWARE_SERVICES PUBLIC
        FIRMWARE_DRIVERS
        PSRAM_ALLOCATOR
        OTA_SHARED
        FREERTOS_FILES
        pico_sha256
        pico_mbedtls
        pico_lwip_sntp
        pico_unique_id
        pico_stdlib
        )

if(NOT W5X00_DIAGNOSTIC_NO_OPCUA)
    target_link_libraries(FIRMWARE_SERVICES PUBLIC
            OPEN62541_FILES
            )
else()
    target_link_libraries(FIRMWARE_SERVICES PUBLIC
            UA_PORT_ALLOC
            )
endif()
