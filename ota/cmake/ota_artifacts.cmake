if(TARGET ota_bootloader AND TARGET device_profile_basic_bank_a)
    if(NOT DEFINED OTA_APP_FIRMWARE_VERSION OR OTA_APP_FIRMWARE_VERSION STREQUAL "")
        set(OTA_APP_FIRMWARE_VERSION unknown)
    endif()
    if(NOT DEFINED OTA_APP_GIT_HASH OR OTA_APP_GIT_HASH STREQUAL "")
        set(OTA_APP_GIT_HASH unknown)
    endif()
    if(NOT DEFINED OTA_APP_FIRMWARE_VERSION_NUMBER)
        set(OTA_APP_FIRMWARE_VERSION_NUMBER 0)
    endif()

    set(OTA_ARTIFACTS_DIR ${CMAKE_BINARY_DIR}/ota_artifacts)
    file(MAKE_DIRECTORY ${OTA_ARTIFACTS_DIR})

    set(OTA_PAYLOAD_BIN ${OTA_ARTIFACTS_DIR}/ota_payload.bin)
    set(OTA_PAYLOAD_MANIFEST ${OTA_ARTIFACTS_DIR}/ota_payload.json)
    set(FACTORY_FULL_BIN ${OTA_ARTIFACTS_DIR}/factory_full.bin)
    set(FACTORY_FULL_UF2 ${OTA_ARTIFACTS_DIR}/factory_full.uf2)

    add_custom_command(
        OUTPUT ${OTA_PAYLOAD_BIN}
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_BINARY_DIR}/app/basic_profile/device_profile_basic_bank_a.bin
                ${OTA_PAYLOAD_BIN}
        DEPENDS device_profile_basic_bank_a
        VERBATIM)

    add_custom_command(
        OUTPUT ${OTA_PAYLOAD_MANIFEST}
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File
                ${CMAKE_SOURCE_DIR}/ota/tools/write_ota_manifest.ps1
                -PayloadBin ${OTA_PAYLOAD_BIN}
                -OutJson ${OTA_PAYLOAD_MANIFEST}
                -Board ${PICO_BOARD}
                -Platform ${PICO_PLATFORM}
                -AppOrigin ${OTA_BANK_A_ORIGIN_HEX}
                -BankBOffset ${OTA_BANK_B_OFFSET_BYTES_HEX}
                -FirmwareVersion ${OTA_APP_FIRMWARE_VERSION}
                -VersionNumber ${OTA_APP_FIRMWARE_VERSION_NUMBER}
                -GitHash ${OTA_APP_GIT_HASH}
        DEPENDS ${OTA_PAYLOAD_BIN}
        VERBATIM)

    add_custom_command(
        OUTPUT ${FACTORY_FULL_BIN}
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File
                ${CMAKE_SOURCE_DIR}/ota/tools/combine_ota_factory.ps1
                -BootloaderBin ${CMAKE_BINARY_DIR}/bootloader/ota_bootloader.bin
                -AppBin ${CMAKE_BINARY_DIR}/app/basic_profile/device_profile_basic_bank_a.bin
                -AppOffsetBytes ${OTA_BANK_A_OFFSET_BYTES}
                -OutBin ${FACTORY_FULL_BIN}
        DEPENDS ota_bootloader device_profile_basic_bank_a
        VERBATIM)

    add_custom_command(
        OUTPUT ${FACTORY_FULL_UF2}
        COMMAND ${picotool_INSTALL_DIR}/picotool/picotool.exe uf2 convert --quiet
                ${FACTORY_FULL_BIN}
                ${FACTORY_FULL_UF2}
                -o ${OTA_FLASH_XIP_BASE_HEX}
                --family ${PICO_PLATFORM}
                --abs-block
        DEPENDS ${FACTORY_FULL_BIN}
        VERBATIM)

    add_custom_target(ota_artifacts
        DEPENDS ${OTA_PAYLOAD_BIN} ${OTA_PAYLOAD_MANIFEST} ${FACTORY_FULL_BIN} ${FACTORY_FULL_UF2})
endif()
