add_library(OTA_SHARED STATIC
        src/ota_flash.c
        src/ota_state.c
        )

add_library(OTA_FLASH_BACKEND_APPLICATION STATIC
        src/ota_flash_backend_application.c
        )

target_include_directories(OTA_SHARED PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        )
target_include_directories(OTA_FLASH_BACKEND_APPLICATION PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        )

ota_layout_apply_to_target(OTA_SHARED)
ota_layout_apply_to_target(OTA_FLASH_BACKEND_APPLICATION)

target_compile_definitions(OTA_SHARED PUBLIC
        PICO_FLASH_ASSERT_ON_UNSAFE=0
        PICO_FLASH_ASSUME_CORE1_SAFE=1
        )

target_link_libraries(OTA_SHARED PUBLIC
        pico_stdlib
        )

target_link_libraries(OTA_FLASH_BACKEND_APPLICATION PRIVATE
        FLASH_OPERATION_COORDINATOR
        )
