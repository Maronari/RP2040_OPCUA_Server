add_library(OPCUA_DEVICE_MODEL STATIC
        src/opcua_model.c
        )

target_include_directories(OPCUA_DEVICE_MODEL PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        )

target_include_directories(OPCUA_DEVICE_MODEL PRIVATE
        ${PICO_SDK_PATH}/lib/mbedtls/tests/src
        )

target_link_libraries(OPCUA_DEVICE_MODEL PUBLIC
        FIRMWARE_SERVICES
        OPEN62541_FILES
        pico_stdlib
        )
