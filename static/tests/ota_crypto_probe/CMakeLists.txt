cmake_minimum_required(VERSION 3.20)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
get_filename_component(PROBE_REPOSITORY_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(PICO_SDK_PATH "${PROBE_REPOSITORY_ROOT}/libraries/pico-sdk" CACHE PATH "Vendored Pico SDK")
set(PICO_BOARD waveshare_core2350b_16mb_psram8mb CACHE STRING "Reference board")
set(PICO_BOARD_HEADER_DIRS "${PROBE_REPOSITORY_ROOT}/boards/include/boards" CACHE STRING "Board definitions")
include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)
project(v060_crypto_measurement C CXX ASM)
pico_sdk_init()
option(OTA_PROBE_FLASH_WRITE
       "Build the guarded destructive legacy-staging-sector microbenchmark" OFF)

set(V060_CRYPTO_PROBE_SOURCES probe.c)
if(OTA_PROBE_FLASH_WRITE)
    list(APPEND V060_CRYPTO_PROBE_SOURCES
         flash_measurement.c
         "${PROBE_REPOSITORY_ROOT}/ota/src/ota_state.c")
endif()

add_executable(v060_crypto_probe ${V060_CRYPTO_PROBE_SOURCES})
set_source_files_properties(${V060_CRYPTO_PROBE_SOURCES} PROPERTIES
    COMPILE_OPTIONS "-Wall;-Wextra;-Werror")
pico_set_binary_type(v060_crypto_probe no_flash)
target_include_directories(v060_crypto_probe PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    "${PROBE_REPOSITORY_ROOT}/port/mbedtls/inc"
    "${PROBE_REPOSITORY_ROOT}/ota/include")
target_compile_definitions(v060_crypto_probe PRIVATE
    MBEDTLS_USER_CONFIG_FILE="probe_user_config.h"
    OTA_PROBE_FLASH_WRITE=$<BOOL:${OTA_PROBE_FLASH_WRITE}>
    PICO_STACK_SIZE=4096
    PICO_CORE1_STACK_SIZE=0
    PICO_STDIO_USB_CONNECT_WAIT_TIMEOUT_MS=5000
    PICO_STDIO_USB_POST_CONNECT_WAIT_DELAY_MS=0)
target_link_libraries(v060_crypto_probe PRIVATE
    pico_stdlib pico_mbedtls pico_sha256 hardware_watchdog hardware_clocks hardware_flash hardware_sync)
pico_enable_stdio_usb(v060_crypto_probe 1)
pico_enable_stdio_uart(v060_crypto_probe 0)
pico_set_program_name(v060_crypto_probe "v060_crypto_probe_RAM_ONLY")
pico_add_extra_outputs(v060_crypto_probe)
