set(RP2_FIRMWARE_VERSION "0.4.3" CACHE INTERNAL "Authoritative firmware semantic version" FORCE)

function(rp2_pack_firmware_version version_text out_version_number)
    string(REGEX MATCH "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$" _version_match "${version_text}")
    if(NOT _version_match)
        message(FATAL_ERROR "RP2_FIRMWARE_VERSION must use semantic version format MAJOR.MINOR.PATCH, got: ${version_text}")
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

rp2_pack_firmware_version("${RP2_FIRMWARE_VERSION}" RP2_FIRMWARE_VERSION_NUMBER)
set(RP2_FIRMWARE_VERSION_NUMBER ${RP2_FIRMWARE_VERSION_NUMBER} CACHE INTERNAL "Authoritative packed firmware version number" FORCE)
