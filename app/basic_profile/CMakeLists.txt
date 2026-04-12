set(TARGET_NAME device_profile_basic)
set(DEVICE_PROFILE_FW_REVISION "0.3.0")
string(TIMESTAMP DEVICE_PROFILE_BUILD_TIME "%Y-%m-%dT%H:%M:%SZ" UTC)
execute_process(
        COMMAND git -C ${CMAKE_SOURCE_DIR} rev-parse --short=12 --verify HEAD
        OUTPUT_VARIABLE DEVICE_PROFILE_GIT_HASH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)

if(NOT DEVICE_PROFILE_GIT_HASH)
    set(DEVICE_PROFILE_GIT_HASH "unknown")
endif()

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

add_executable(${TARGET_NAME}
        main.c
        )

target_include_directories(${TARGET_NAME} PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}/generated
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
