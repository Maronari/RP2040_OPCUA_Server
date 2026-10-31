# Keep OTA crypto symbols and allocation policy separate from live TLS.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(OTA_CRYPTO_NAMESPACE "${CMAKE_CURRENT_BINARY_DIR}/generated/ota_crypto_namespace.h")
execute_process(
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/static/tools/generate_ota_crypto_namespace.py"
        --mbedtls-root "${PICO_MBEDTLS_PATH}"
        --output "${OTA_CRYPTO_NAMESPACE}"
    COMMAND_ERROR_IS_FATAL ANY)
file(GLOB_RECURSE OTA_CRYPTO_NAMESPACE_INPUTS CONFIGURE_DEPENDS
    "${PICO_MBEDTLS_PATH}/include/mbedtls/*.h"
    "${PICO_MBEDTLS_PATH}/library/*.h"
    "${PICO_MBEDTLS_PATH}/library/*.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/static/tools/generate_ota_crypto_namespace.py"
    ${OTA_CRYPTO_NAMESPACE_INPUTS})

# Source list/configuration is shared with the native host-vector harness.
set(OTA_CRYPTO_SOURCES
    bignum.c bignum_core.c bignum_mod.c bignum_mod_raw.c
    constant_time.c ecp.c ecp_curves.c ecp_curves_new.c ecdsa.c asn1parse.c asn1write.c
    sha256.c platform_util.c)
list(TRANSFORM OTA_CRYPTO_SOURCES PREPEND "${PICO_MBEDTLS_PATH}/library/")
add_library(OTA_VERIFIER STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ota_verifier.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ota_verifier_platform_application.c"
    ${OTA_CRYPTO_SOURCES})
target_include_directories(OTA_VERIFIER PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(OTA_VERIFIER PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${PICO_MBEDTLS_PATH}/include" "${PICO_MBEDTLS_PATH}/library")
target_compile_options(OTA_VERIFIER PRIVATE -include "${OTA_CRYPTO_NAMESPACE}" -fstack-usage)
target_compile_definitions(OTA_VERIFIER PRIVATE
    MBEDTLS_CONFIG_FILE="ota/ota_verifier_mbedtls_config.h")
target_link_libraries(OTA_VERIFIER PRIVATE pico_stdlib PSRAM_ALLOCATOR)
