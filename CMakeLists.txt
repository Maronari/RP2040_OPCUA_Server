# CMake minimum required version
cmake_minimum_required(VERSION 3.13)

if(POLICY CMP0077)
    cmake_policy(SET CMP0077 NEW)
endif()

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type")
endif()
set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)

# Pull in SDK (must be before project)
if(NOT DEFINED PICO_SDK_PATH)
    set(PICO_SDK_PATH ${CMAKE_SOURCE_DIR}/libraries/pico-sdk)
    message(STATUS "PICO_SDK_PATH = ${PICO_SDK_PATH}")
endif()

if(NOT DEFINED PICO_BOARD_HEADER_DIRS)
    set(PICO_BOARD_HEADER_DIRS ${CMAKE_SOURCE_DIR}/boards/include/boards CACHE STRING "Additional Pico board header directories")
else()
    list(APPEND PICO_BOARD_HEADER_DIRS ${CMAKE_SOURCE_DIR}/boards/include/boards)
    list(REMOVE_DUPLICATES PICO_BOARD_HEADER_DIRS)
    set(PICO_BOARD_HEADER_DIRS ${PICO_BOARD_HEADER_DIRS} CACHE STRING "Additional Pico board header directories" FORCE)
endif()

if(NOT DEFINED PICO_BOARD)
    set(PICO_BOARD waveshare_core2350b_16mb_psram8mb CACHE STRING "Pico board")
endif()

if(NOT DEFINED WIZNET_CHIP)
    set(WIZNET_CHIP W5500 CACHE STRING "WIZnet Ethernet chip")
endif()
set_property(CACHE WIZNET_CHIP PROPERTY STRINGS W5100S W5500)

if(NOT DEFINED PICO_DEOPTIMIZED_DEBUG AND CMAKE_BUILD_TYPE STREQUAL "Debug")
    # Fully de-optimized Debug is too slow for OPC UA Basic256Sha256
    # handshakes on RP2350: UaExpert times out during OpenSecureChannel.
    set(PICO_DEOPTIMIZED_DEBUG OFF)
endif()

option(BUILD_LEGACY_EXAMPLES "Build legacy example targets" OFF)
option(OTA_BUILD_BOOTLOADER "Build the OTA bootloader target" OFF)

include(${CMAKE_SOURCE_DIR}/patches/apply_submodule_patches.cmake)
rp2_apply_submodule_patch(
    libraries/pico-sdk
    patches/submodules/pico-sdk-lwip-freertos-init.patch)
rp2_apply_submodule_patch(
    libraries/FreeRTOS-Kernel
    patches/submodules/freertos-malloc-diagnostics.patch)

include(pico_sdk_import.cmake)
include(rp2040_hat_lwip_c_sdk_version.cmake)
include(rp2040_hat_freertos_c_sdk_version.cmake)
include(${CMAKE_SOURCE_DIR}/cmake/firmware_version.cmake)

# Set project name
set(PROJECT_NAME RP2-HAT-LWIP-C)

# Set project informations
project(${PROJECT_NAME} VERSION ${RP2_FIRMWARE_VERSION} LANGUAGES C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Initialize the SDK
pico_sdk_init()

include(${CMAKE_SOURCE_DIR}/ota/cmake/ota_layout.cmake)

set(_device_identity_cert_default "")
set(_device_identity_key_default "")
if(EXISTS "${CMAKE_SOURCE_DIR}/static/certs/server.crt.der" AND
   EXISTS "${CMAKE_SOURCE_DIR}/static/certs/server.key.pem")
    set(_device_identity_cert_default "${CMAKE_SOURCE_DIR}/static/certs/server.crt.der")
    set(_device_identity_key_default "${CMAKE_SOURCE_DIR}/static/certs/server.key.pem")
endif()

set(DEVICE_IDENTITY_CERT_DER_FILE
    "${_device_identity_cert_default}"
    CACHE FILEPATH "DER-encoded OPC UA device certificate to provision into flash")
set(DEVICE_IDENTITY_PRIVATE_KEY_FILE
    "${_device_identity_key_default}"
    CACHE FILEPATH "PEM or DER OPC UA private key to provision into flash")

if(${WIZNET_CHIP} STREQUAL W5100S)
    add_definitions(-D_WIZCHIP_=W5100S)
elseif(${WIZNET_CHIP} STREQUAL W5500)
    add_definitions(-D_WIZCHIP_=W5500)
else()
    message(FATAL_ERROR "WIZNET_CHIP is wrong = ${WIZNET_CHIP}")
endif()

# Set the project root directory if it's not already defined, as may happen if
# the tests folder is included directly by a parent project, without including
# the top level CMakeLists.txt.
if(NOT DEFINED FREERTOS_DIR)
    set(FREERTOS_DIR ${CMAKE_SOURCE_DIR}/libraries/FreeRTOS-Kernel)
    message(STATUS "FREERTOS_DIR = ${FREERTOS_DIR}")
endif()

if(NOT DEFINED WIZNET_DIR)
    set(WIZNET_DIR ${CMAKE_SOURCE_DIR}/libraries/ioLibrary_Driver)
    message(STATUS "WIZNET_DIR = ${WIZNET_DIR}")
endif()

if(NOT DEFINED PORT_DIR)
    set(PORT_DIR ${CMAKE_SOURCE_DIR}/port)
    message(STATUS "PORT_DIR = ${PORT_DIR}")
endif()

if(NOT DEFINED OPEN62541_DIR)
    set(OPEN62541_DIR ${PORT_DIR}/open62541)
    message(STATUS "OPEN62541_DIR = ${OPEN62541_DIR}")
endif()

add_definitions(-DUA_ARCHITECTURE_FREERTOSLWIP)
add_definitions(-DSET_TRUSTED_CERT_IN_SAMPLES)

# Add libraries in subdirectories
add_subdirectory(${CMAKE_SOURCE_DIR}/libraries)
add_subdirectory(${PORT_DIR})
add_subdirectory(drivers)
add_subdirectory(ota)
add_subdirectory(services)
add_subdirectory(opcua_model)
add_subdirectory(app)

if(OTA_BUILD_BOOTLOADER AND EXISTS ${CMAKE_SOURCE_DIR}/bootloader/CMakeLists.txt)
    add_subdirectory(bootloader)
    include(${CMAKE_SOURCE_DIR}/ota/cmake/ota_artifacts.cmake)
endif()

if(BUILD_LEGACY_EXAMPLES)
    add_subdirectory(examples)
endif()

# Set compile options
add_compile_options(
        -Wall
        -Wno-format          # int != int32_t as far as the compiler is concerned because gcc has int32_t as long int
        -Wno-unused-function # we have some for the docs that aren't called
        -Wno-maybe-uninitialized
)
