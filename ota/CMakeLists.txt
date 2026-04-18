add_library(OTA_SHARED STATIC
        src/ota_flash.c
        src/ota_state.c
        )

target_include_directories(OTA_SHARED PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        )

ota_layout_apply_to_target(OTA_SHARED)

target_compile_definitions(OTA_SHARED PRIVATE
        PICO_FLASH_ASSERT_ON_UNSAFE=0
        PICO_FLASH_ASSUME_CORE1_SAFE=1
        )

target_link_libraries(OTA_SHARED PUBLIC
        pico_stdlib
        pico_flash
        hardware_flash
        )
