find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(V060_OTA_RAM_FIXTURE "${CMAKE_CURRENT_BINARY_DIR}/generated/ota_verifier_fixture.h")
add_custom_command(
    OUTPUT "${V060_OTA_RAM_FIXTURE}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/generate_fixture.py"
            --output "${V060_OTA_RAM_FIXTURE}"
    DEPENDS "${CMAKE_CURRENT_LIST_DIR}/generate_fixture.py"
    VERBATIM)
add_custom_target(v060_ota_verifier_ram_fixture DEPENDS "${V060_OTA_RAM_FIXTURE}")

add_executable(v060_ota_verifier_ram_probe
    "${CMAKE_CURRENT_LIST_DIR}/probe.c"
    "${CMAKE_CURRENT_LIST_DIR}/freertos_hooks.c"
    "${V060_OTA_RAM_FIXTURE}")
add_dependencies(v060_ota_verifier_ram_probe v060_ota_verifier_ram_fixture)
set_source_files_properties(
    "${CMAKE_CURRENT_LIST_DIR}/probe.c"
    "${CMAKE_CURRENT_LIST_DIR}/freertos_hooks.c"
    PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra;-Werror")
pico_set_binary_type(v060_ota_verifier_ram_probe no_flash)
target_include_directories(v060_ota_verifier_ram_probe PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
    "${CMAKE_SOURCE_DIR}/ota/include"
    "${CMAKE_SOURCE_DIR}/ota/src"
    "${CMAKE_SOURCE_DIR}/port/psram"
    "${FREERTOS_DIR}/include"
    "${FREERTOS_DIR}/portable/ThirdParty/GCC/RP2350_ARM_NTZ/non_secure"
    "${CMAKE_SOURCE_DIR}/port/FreeRTOS-Kernel/inc")
target_compile_definitions(v060_ota_verifier_ram_probe PRIVATE
    OTA_VERIFIER_MEASUREMENT=1
    PICO_STACK_SIZE=4096
    PICO_CORE1_STACK_SIZE=0
    PICO_STDIO_USB_CONNECT_WAIT_TIMEOUT_MS=5000
    PICO_STDIO_USB_POST_CONNECT_WAIT_DELAY_MS=0)
target_link_libraries(v060_ota_verifier_ram_probe PRIVATE
    OTA_VERIFIER
    PSRAM_ALLOCATOR
    pico_stdlib
    hardware_flash
    hardware_sync
    hardware_watchdog)
pico_enable_stdio_usb(v060_ota_verifier_ram_probe 1)
pico_enable_stdio_uart(v060_ota_verifier_ram_probe 0)
pico_set_program_name(v060_ota_verifier_ram_probe "v060_ota_verifier_ram_probe")
pico_add_extra_outputs(v060_ota_verifier_ram_probe)
