# Dedicated T0 evidence build. It is opt-in and restricted so benchmark,
# diagnostic, industrial and non-Debug builds cannot claim this profile.
option(FIRMWARE_OTA_MEASUREMENT
       "Emit v0.6.0 T0 startup and stack measurement console markers (reference Debug only)"
       OFF)

if(FIRMWARE_OTA_MEASUREMENT)
    if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug" OR
       NOT FIRMWARE_DRIVER_PACK STREQUAL "base" OR
       NOT PICO_BOARD STREQUAL "waveshare_core2350b_16mb_psram8mb" OR
       NOT FIRMWARE_BENCHMARK_PROFILE STREQUAL "off" OR
       NOT FIRMWARE_BENCHMARK_SECURITY STREQUAL "basic256sha256-signandencrypt" OR
       W5X00_DIAGNOSTIC_NO_OPCUA OR OTA_BUILD_BOOTLOADER)
        message(FATAL_ERROR
                "FIRMWARE_OTA_MEASUREMENT requires Debug/base/waveshare_core2350b_16mb_psram8mb, "
                "benchmark profile off, Basic256Sha256 SignAndEncrypt, OPC UA enabled and no bootloader")
    endif()
    message(STATUS "T0 OTA measurement console markers enabled (reference Debug only)")
endif()

function(ota_measurement_apply_to_target target_name)
    target_compile_definitions(${target_name} PRIVATE
            FIRMWARE_OTA_MEASUREMENT=$<BOOL:${FIRMWARE_OTA_MEASUREMENT}>)
endfunction()
