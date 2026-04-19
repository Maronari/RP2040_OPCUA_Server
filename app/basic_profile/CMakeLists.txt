set(TARGET_NAME device_profile_basic)
set(DEVICE_PROFILE_FW_REVISION "0.3.0")
string(TIMESTAMP DEVICE_PROFILE_BUILD_TIME "%Y-%m-%dT%H:%M:%SZ" UTC)

function(ota_pack_firmware_version version_text out_version_number)
    string(REGEX MATCH "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$" _version_match "${version_text}")
    if(NOT _version_match)
        message(FATAL_ERROR "DEVICE_PROFILE_FW_REVISION must use semantic version format MAJOR.MINOR.PATCH, got: ${version_text}")
    endif()

    set(_major ${CMAKE_MATCH_1})
    set(_minor ${CMAKE_MATCH_2})
    set(_patch ${CMAKE_MATCH_3})

    if(_major GREATER 255 OR _minor GREATER 255 OR _patch GREATER 65535)
        message(FATAL_ERROR "Firmware version components exceed OTA packing limits: ${version_text}")
    endif()

    math(EXPR _version_number "(${_major} << 24) | (${_minor} << 16) | ${_patch}")
    set(${out_version_number} ${_version_number} PARENT_SCOPE)
endfunction()

ota_pack_firmware_version("${DEVICE_PROFILE_FW_REVISION}" DEVICE_PROFILE_FW_VERSION_NUMBER)


execute_process(
        COMMAND git -C ${CMAKE_SOURCE_DIR} rev-parse --short=12 --verify HEAD
        OUTPUT_VARIABLE DEVICE_PROFILE_GIT_HASH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)

if(NOT DEVICE_PROFILE_GIT_HASH)
    set(DEVICE_PROFILE_GIT_HASH "unknown")
endif()

set(DEVICE_PROFILE_CONFIG_EXPORT_PATH "${CMAKE_BINARY_DIR}/config_export.json")
set(DEVICE_PROFILE_CONFIG_EXPORT_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/generate_config_export.cmake")

add_custom_target(${TARGET_NAME}_config_export
        COMMAND ${CMAKE_COMMAND}
        -DOUTPUT_PATH=${DEVICE_PROFILE_CONFIG_EXPORT_PATH}
        -DFW_REVISION=${DEVICE_PROFILE_FW_REVISION}
        -DBUILD_TIME=${DEVICE_PROFILE_BUILD_TIME}
        -DGIT_HASH=${DEVICE_PROFILE_GIT_HASH}
        -DHW_REVISION=${PICO_BOARD}/${WIZNET_CHIP}
        -P ${DEVICE_PROFILE_CONFIG_EXPORT_SCRIPT}
        BYPRODUCTS ${DEVICE_PROFILE_CONFIG_EXPORT_PATH}
        COMMENT "Generating ${DEVICE_PROFILE_CONFIG_EXPORT_PATH}"
        VERBATIM)

set(OTA_APP_FIRMWARE_VERSION ${DEVICE_PROFILE_FW_REVISION} CACHE INTERNAL "OTA app firmware version" FORCE)
set(OTA_APP_FIRMWARE_VERSION_NUMBER ${DEVICE_PROFILE_FW_VERSION_NUMBER} CACHE INTERNAL "OTA app firmware version number" FORCE)
set(OTA_APP_GIT_HASH ${DEVICE_PROFILE_GIT_HASH} CACHE INTERNAL "OTA app git hash" FORCE)

set(DEVICE_IDENTITY_CERT_DER_FILE "" CACHE FILEPATH "DER-encoded OPC UA device certificate to provision into flash")
set(DEVICE_IDENTITY_PRIVATE_KEY_FILE "" CACHE FILEPATH "PEM or DER private key to provision into flash")
set(DEVICE_TRUST_LIST_FILE "" CACHE FILEPATH "DER-encoded trusted client certificate to provision into flash")

function(file_to_c_array input_file output_bytes_var output_length_var)
    if(NOT EXISTS "${input_file}")
        message(FATAL_ERROR "Provisioning asset not found: ${input_file}")
    endif()

    file(READ "${input_file}" _hex HEX)
    string(LENGTH "${_hex}" _hex_length)
    if(_hex_length EQUAL 0)
        message(FATAL_ERROR "Provisioning asset is empty: ${input_file}")
    endif()

    math(EXPR _byte_count "${_hex_length} / 2")
    math(EXPR _last_index "${_byte_count} - 1")
    set(_byte_list "")
    foreach(_index RANGE 0 ${_last_index})
        math(EXPR _pos "${_index} * 2")
        string(SUBSTRING "${_hex}" ${_pos} 2 _byte_hex)
        string(APPEND _byte_list "0x${_byte_hex}")
        if(NOT _index EQUAL _last_index)
            string(APPEND _byte_list ", ")
        endif()
    endforeach()

    set(${output_bytes_var} "${_byte_list}" PARENT_SCOPE)
    set(${output_length_var} "${_byte_count}" PARENT_SCOPE)
endfunction()

set(DEVICE_IDENTITY_PROVISIONING_ENABLED 0)
set(DEVICE_IDENTITY_CERTIFICATE_BYTES "0x00")
set(DEVICE_IDENTITY_CERTIFICATE_LENGTH 0)
set(DEVICE_IDENTITY_PRIVATE_KEY_BYTES "0x00")
set(DEVICE_IDENTITY_PRIVATE_KEY_LENGTH 0)
set(DEVICE_TRUST_LIST_PROVISIONING_ENABLED 0)
set(DEVICE_TRUST_LIST_BYTES "0x00")
set(DEVICE_TRUST_LIST_LENGTH 0)

if(DEVICE_IDENTITY_CERT_DER_FILE OR DEVICE_IDENTITY_PRIVATE_KEY_FILE)
    if(NOT DEVICE_IDENTITY_CERT_DER_FILE OR NOT DEVICE_IDENTITY_PRIVATE_KEY_FILE)
        message(FATAL_ERROR "Set both DEVICE_IDENTITY_CERT_DER_FILE and DEVICE_IDENTITY_PRIVATE_KEY_FILE to enable certificate provisioning")
    endif()

    set(DEVICE_IDENTITY_PROVISIONING_ENABLED 1)
    file_to_c_array("${DEVICE_IDENTITY_CERT_DER_FILE}"
                    DEVICE_IDENTITY_CERTIFICATE_BYTES
                    DEVICE_IDENTITY_CERTIFICATE_LENGTH)
    file_to_c_array("${DEVICE_IDENTITY_PRIVATE_KEY_FILE}"
                    DEVICE_IDENTITY_PRIVATE_KEY_BYTES
                    DEVICE_IDENTITY_PRIVATE_KEY_LENGTH)

    message(STATUS "Device identity provisioning enabled")
    message(STATUS "  Certificate: ${DEVICE_IDENTITY_CERT_DER_FILE}")
    message(STATUS "  Private key: ${DEVICE_IDENTITY_PRIVATE_KEY_FILE}")
endif()

if(DEVICE_TRUST_LIST_FILE)
    set(DEVICE_TRUST_LIST_PROVISIONING_ENABLED 1)
    file_to_c_array("${DEVICE_TRUST_LIST_FILE}"
                    DEVICE_TRUST_LIST_BYTES
                    DEVICE_TRUST_LIST_LENGTH)

    message(STATUS "Device trust list provisioning enabled")
    message(STATUS "  Trust list: ${DEVICE_TRUST_LIST_FILE}")
endif()

configure_file(${CMAKE_CURRENT_LIST_DIR}/device_identity_provisioning.h.in
               ${CMAKE_CURRENT_BINARY_DIR}/generated/device_identity_provisioning.h
               @ONLY)

function(add_device_profile_target target_name use_ota_linker)
    add_executable(${target_name}
            main.c
            ota_layout_sanity.c
            )

    target_include_directories(${target_name} PRIVATE
            ${CMAKE_CURRENT_BINARY_DIR}/generated
            )

    ota_layout_apply_to_target(${target_name})
    if(use_ota_linker)
        ota_layout_set_app_bank_a_linker_script(${target_name})
    endif()

    target_link_libraries(${target_name} PRIVATE
            OTA_SHARED
            FIRMWARE_DRIVERS
            FIRMWARE_SERVICES
            OPCUA_DEVICE_MODEL
            )

    add_dependencies(${target_name} ${TARGET_NAME}_config_export)

    pico_enable_stdio_usb(${target_name} 1)
    pico_enable_stdio_uart(${target_name} 0)
    pico_add_extra_outputs(${target_name})

    target_compile_definitions(${target_name} PRIVATE
            PICO_CORE1_STACK_SIZE=0
            PICO_HEAP_SIZE=0x400
            PICO_STDIO_USB_CONNECTION_WITHOUT_DTR=1
            PICO_STDIO_USB_CONNECT_WAIT_TIMEOUT_MS=0
            PICO_STDIO_USB_POST_CONNECT_WAIT_DELAY_MS=0
            PICO_STDIO_USB_STDOUT_TIMEOUT_US=0
            PICO_STDIO_USB_ENABLE_RESET_VIA_VENDOR_INTERFACE=0
            PICO_STDIO_USB_ENABLE_RESET_VIA_BAUD_RATE=0
            DEVICE_PROFILE_HW_REVISION=\"${PICO_BOARD}/${WIZNET_CHIP}\"
            DEVICE_PROFILE_FW_REVISION=\"${DEVICE_PROFILE_FW_REVISION}\"
            DEVICE_PROFILE_BUILD_TIME=\"${DEVICE_PROFILE_BUILD_TIME}\"
            DEVICE_PROFILE_GIT_HASH=\"${DEVICE_PROFILE_GIT_HASH}\"
            )
endfunction()

add_device_profile_target(${TARGET_NAME} FALSE)

if(OTA_BUILD_BOOTLOADER)
    add_device_profile_target(${TARGET_NAME}_bank_a TRUE)
endif()
