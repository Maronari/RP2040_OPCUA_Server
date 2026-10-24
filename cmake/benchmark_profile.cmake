# Explicit test firmware only. Keep all production defaults unchanged.
set(FIRMWARE_BENCHMARK_PROFILE "off" CACHE STRING "off, synthetic or resource-exhaustion (test firmware)")
set(FIRMWARE_BENCHMARK_CHANNELS "1" CACHE STRING "Synthetic channels: 1,4,8,16")
set(FIRMWARE_BENCHMARK_RATE_HZ "1" CACHE STRING "Synthetic sampling/subscription rate: 1,10")
set(FIRMWARE_BENCHMARK_CLIENTS "1" CACHE STRING "Declared external client load: 1,2,4")
set(FIRMWARE_BENCHMARK_SECURITY "basic256sha256-signandencrypt" CACHE STRING "Benchmark security")
option(FIRMWARE_BENCHMARK_EVENTS "Benchmark event publication" ON)
option(FIRMWARE_BENCHMARK_HISTORY "Benchmark channel history" ON)
if(NOT FIRMWARE_BENCHMARK_PROFILE MATCHES "^(off|synthetic|resource-exhaustion)$")
    message(FATAL_ERROR "Unsupported FIRMWARE_BENCHMARK_PROFILE")
endif()
if(NOT FIRMWARE_BENCHMARK_CHANNELS MATCHES "^(1|4|8|16)$" OR
   NOT FIRMWARE_BENCHMARK_RATE_HZ MATCHES "^(1|10)$" OR
   NOT FIRMWARE_BENCHMARK_CLIENTS MATCHES "^(1|2|4)$" OR
   NOT FIRMWARE_BENCHMARK_SECURITY MATCHES "^(basic256sha256-signandencrypt|test-none)$")
    message(FATAL_ERROR "Unsupported benchmark axis value")
endif()
if(FIRMWARE_BENCHMARK_PROFILE STREQUAL "off")
    if(FIRMWARE_BENCHMARK_SECURITY STREQUAL "test-none")
        message(FATAL_ERROR "test-none requires explicit synthetic test firmware")
    endif()
    return()
endif()
if(NOT FIRMWARE_DRIVER_PACK STREQUAL "base" OR
   NOT PICO_BOARD STREQUAL "waveshare_core2350b_16mb_psram8mb" OR
   NOT WIZNET_CHIP STREQUAL "W5500" OR
   OTA_BUILD_BOOTLOADER OR W5X00_DIAGNOSTIC_NO_OPCUA OR BUILD_LEGACY_EXAMPLES)
    message(FATAL_ERROR "Benchmark firmware requires reference base/W5500 without bootloader, legacy or network-only targets")
endif()
if(FIRMWARE_BENCHMARK_PROFILE STREQUAL "resource-exhaustion")
    if(FIRMWARE_BENCHMARK_SECURITY STREQUAL "test-none")
        message(FATAL_ERROR "resource-exhaustion does not expose an OPC UA test-none endpoint")
    endif()
    set(RP2_RESOURCE_EXHAUSTION_PROFILE_ID "v052-resource-exhaustion-reference")
    if(NOT CMAKE_SCRIPT_MODE_FILE)
        add_compile_definitions(
            RP2_RESOURCE_EXHAUSTION_TEST_ENABLED=1
            RP2_RESOURCE_EXHAUSTION_PROFILE_ID="${RP2_RESOURCE_EXHAUSTION_PROFILE_ID}")
    endif()
    message(STATUS "TEST-ONLY resource exhaustion: ${RP2_RESOURCE_EXHAUSTION_PROFILE_ID}")
    return()
endif()
foreach(axis EVENTS HISTORY)
    if(FIRMWARE_BENCHMARK_${axis})
        set(RP2_BENCHMARK_${axis}_ENABLED 1)
        set(RP2_BENCHMARK_${axis}_JSON true)
    else()
        set(RP2_BENCHMARK_${axis}_ENABLED 0)
        set(RP2_BENCHMARK_${axis}_JSON false)
    endif()
endforeach()
set(RP2_BENCHMARK_SECURITY_NONE 0)
if(FIRMWARE_BENCHMARK_SECURITY STREQUAL "test-none")
    set(RP2_BENCHMARK_SECURITY_NONE 1)
endif()
set(RP2_BENCHMARK_PROFILE_ID "v052-syn-c${FIRMWARE_BENCHMARK_CHANNELS}-ns0full-sec${FIRMWARE_BENCHMARK_SECURITY}-cl${FIRMWARE_BENCHMARK_CLIENTS}-hz${FIRMWARE_BENCHMARK_RATE_HZ}-ev${RP2_BENCHMARK_EVENTS_ENABLED}-hist${RP2_BENCHMARK_HISTORY_ENABLED}")
if(NOT CMAKE_SCRIPT_MODE_FILE)
    add_compile_definitions(
        RP2_BENCHMARK_PROFILE_ENABLED=1
        RP2_BENCHMARK_CHANNEL_COUNT=${FIRMWARE_BENCHMARK_CHANNELS}
        RP2_BENCHMARK_RATE_HZ=${FIRMWARE_BENCHMARK_RATE_HZ}
        RP2_BENCHMARK_CLIENT_COUNT=${FIRMWARE_BENCHMARK_CLIENTS}
        RP2_BENCHMARK_SECURITY_NONE=${RP2_BENCHMARK_SECURITY_NONE}
        RP2_BENCHMARK_EVENTS_ENABLED=${RP2_BENCHMARK_EVENTS_ENABLED}
        RP2_BENCHMARK_HISTORY_ENABLED=${RP2_BENCHMARK_HISTORY_ENABLED}
        RP2_BENCHMARK_PROFILE_ID="${RP2_BENCHMARK_PROFILE_ID}")
endif()
message(STATUS "TEST-ONLY benchmark: ${RP2_BENCHMARK_PROFILE_ID}")
