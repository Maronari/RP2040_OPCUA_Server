# CMAKE_PROJECT_INCLUDE hook for a disposable, never-deployed link-size probe.
function(v060_measure_crypto_link_cost)
    if(TARGET ota_bootloader)
        get_filename_component(probe_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../../.." ABSOLUTE)
        target_include_directories(ota_bootloader PRIVATE
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}"
            "${probe_root}/port/mbedtls/inc")
        target_compile_definitions(ota_bootloader PRIVATE
            MBEDTLS_USER_CONFIG_FILE="probe_user_config.h")
        target_link_libraries(ota_bootloader PRIVATE pico_mbedtls pico_sha256)
        # Retain primitives without changing the old CRC-only boot code.
        foreach(symbol IN ITEMS mbedtls_ecdsa_verify mbedtls_ecp_group_load
                mbedtls_ecp_point_read_binary mbedtls_ecp_check_pubkey
                mbedtls_mpi_read_binary mbedtls_sha256_starts
                mbedtls_sha256_update mbedtls_sha256_finish
                mbedtls_memory_buffer_alloc_init)
            target_link_options(ota_bootloader PRIVATE "LINKER:--undefined=${symbol}")
        endforeach()
    endif()
endfunction()
cmake_language(DEFER CALL v060_measure_crypto_link_cost)
