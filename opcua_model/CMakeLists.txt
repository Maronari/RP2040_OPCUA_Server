add_library(OPCUA_DEVICE_MODEL STATIC
        src/opcua_model.c
        src/opcua_model_context.c
        src/model_common.c
        src/model_access.c
        src/model_channels.c
        src/model_pubsub.c
        src/model_events.c
        src/model_maintenance.c
        src/model_history.c
        src/model_configuration.c
        src/model_security.c
        src/model_ota.c
        src/model_diagnostics.c
        src/model_identity_di_amb.c
        src/model_address_space.c
        src/model_core_lifecycle.c
        )

target_include_directories(OPCUA_DEVICE_MODEL PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        )

# Ties Device.Compatibility.FirmwareProfile (opcua_model_internal.h's
# MODEL_FIRMWARE_DRIVER_PACK_NAME) to the actual top-level FIRMWARE_DRIVER_PACK
# selection, instead of that macro's own stale hardcoded "base" default.
target_compile_definitions(OPCUA_DEVICE_MODEL PRIVATE
        MODEL_FIRMWARE_DRIVER_PACK_NAME=\"${FIRMWARE_DRIVER_PACK}\"
        )
ota_measurement_apply_to_target(OPCUA_DEVICE_MODEL)

target_include_directories(OPCUA_DEVICE_MODEL PRIVATE
        ${PICO_SDK_PATH}/lib/mbedtls/tests/src
        )

target_link_libraries(OPCUA_DEVICE_MODEL PUBLIC
        FIRMWARE_SERVICES
        OPEN62541_FILES
        pico_stdlib
        )

target_link_libraries(OPCUA_DEVICE_MODEL PRIVATE
        PSRAM_ALLOCATOR
        )
