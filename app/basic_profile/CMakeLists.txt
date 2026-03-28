set(TARGET_NAME device_profile_basic)
set(DEVICE_PROFILE_FW_REVISION "0.1.0")
string(TIMESTAMP DEVICE_PROFILE_BUILD_TIME "%Y-%m-%dT%H:%M:%SZ" UTC)
execute_process(
        COMMAND git -C ${CMAKE_SOURCE_DIR} rev-parse --short=12 --verify HEAD
        OUTPUT_VARIABLE DEVICE_PROFILE_GIT_HASH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)

if(NOT DEVICE_PROFILE_GIT_HASH)
    set(DEVICE_PROFILE_GIT_HASH "unknown")
endif()

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
        PICO_DEBUG_MALLOC_LOW_WATER=5000
        DEVICE_PROFILE_HW_REVISION=\"${PICO_BOARD}/${WIZNET_CHIP}\"
        DEVICE_PROFILE_FW_REVISION=\"${DEVICE_PROFILE_FW_REVISION}\"
        DEVICE_PROFILE_BUILD_TIME=\"${DEVICE_PROFILE_BUILD_TIME}\"
        DEVICE_PROFILE_GIT_HASH=\"${DEVICE_PROFILE_GIT_HASH}\"
        )
