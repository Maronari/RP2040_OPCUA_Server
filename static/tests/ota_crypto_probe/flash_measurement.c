/*
 * Optional RAM-only destructive Flash measurement. This file is compiled only
 * when OTA_PROBE_FLASH_WRITE=1; it is deliberately outside production targets.
 */

#include "flash_measurement.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"
#include "ota/ota_state.h"
#include "pico/stdlib.h"
#include "pico/time.h"

#if !defined(PICO_NO_FLASH) || !PICO_NO_FLASH
#error "Flash measurement must remain in the RAM-only no_flash probe"
#endif
#if !defined(OTA_PROBE_FLASH_WRITE) || !OTA_PROBE_FLASH_WRITE
#error "flash_measurement.c must only be compiled for the explicit write variant"
#endif

enum {
    LEGACY_OTA_STATE_OFFSET = 0x00ff0000u,
    LEGACY_STAGING_TEST_OFFSET = 0x00f57000u,
    LEGACY_STAGING_TEST_BYTES = FLASH_SECTOR_SIZE,
    PROGRAM_PAGE_COUNT = LEGACY_STAGING_TEST_BYTES / FLASH_PAGE_SIZE,
};

#define XIP_BASE_ADDRESS ((const uint8_t *)(uintptr_t)0x10000000u)

_Static_assert(LEGACY_STAGING_TEST_OFFSET + LEGACY_STAGING_TEST_BYTES == 0x00f58000u,
               "measurement sector must end at the legacy Bank-B boundary");
_Static_assert(LEGACY_STAGING_TEST_BYTES == FLASH_SECTOR_SIZE,
               "measurement requires exactly one Flash erase sector");
_Static_assert(PROGRAM_PAGE_COUNT == 16u, "measurement requires sixteen pages");
_Static_assert(sizeof(ota_state_record_t) == 64u, "canonical legacy state must be 64 bytes");
_Static_assert(offsetof(ota_state_record_t, record_crc32) == 60u,
               "canonical legacy state CRC offset changed");

static uint8_t program_page[FLASH_PAGE_SIZE] __attribute__((aligned(4)));

static bool all_value(const uint8_t *bytes, size_t size, uint8_t value)
{
    for (size_t index = 0u; index < size; ++index) {
        if (bytes[index] != value) {
            return false;
        }
    }
    return true;
}

bool ota_probe_flash_measurement_canonical_idle(void)
{
    ota_state_record_t record;

    memcpy(&record, XIP_BASE_ADDRESS + LEGACY_OTA_STATE_OFFSET, sizeof(record));
    return ota_state_validate(&record) && record.state == OTA_IMAGE_STATE_IDLE &&
           all_value(((const uint8_t *)&record) + 8u, 52u, 0u);
}

bool ota_probe_flash_measurement_target_is_erased(void)
{
    return all_value(XIP_BASE_ADDRESS + LEGACY_STAGING_TEST_OFFSET,
                     LEGACY_STAGING_TEST_BYTES, 0xffu);
}

static void fill_program_page(unsigned int page_index)
{
    for (size_t index = 0u; index < sizeof(program_page); ++index) {
        program_page[index] = (uint8_t)(0x5au ^ (uint8_t)page_index ^ (uint8_t)index);
    }
}

static bool erase_and_verify(const char *name)
{
    const uint64_t started = time_us_64();
    const uint32_t saved_interrupts = save_and_disable_interrupts();

    flash_range_erase(LEGACY_STAGING_TEST_OFFSET, LEGACY_STAGING_TEST_BYTES);
    restore_interrupts(saved_interrupts);
    watchdog_update();
    const bool erased = ota_probe_flash_measurement_target_is_erased();
    printf("[FLASH_PROBE] test=%s elapsed_us=%llu result=%u\n", name,
           (unsigned long long)(time_us_64() - started), erased ? 0u : 1u);
    return erased;
}

static bool program_and_verify(void)
{
    const uint64_t started = time_us_64();

    for (unsigned int page = 0u; page < PROGRAM_PAGE_COUNT; ++page) {
        uint32_t saved_interrupts;

        fill_program_page(page);
        watchdog_update();
        saved_interrupts = save_and_disable_interrupts();
        flash_range_program(LEGACY_STAGING_TEST_OFFSET + page * FLASH_PAGE_SIZE,
                            program_page, sizeof(program_page));
        restore_interrupts(saved_interrupts);
        watchdog_update();
    }
    /* The Pico SDK program primitive is void; readback below is its result. */
    printf("[FLASH_PROBE] test=program_pages elapsed_us=%llu result=0\n",
           (unsigned long long)(time_us_64() - started));
    return true;
}

static bool readback_verify(void)
{
    const uint64_t started = time_us_64();
    bool matches = true;

    for (unsigned int page = 0u; page < PROGRAM_PAGE_COUNT; ++page) {
        fill_program_page(page);
        if (memcmp(XIP_BASE_ADDRESS + LEGACY_STAGING_TEST_OFFSET + page * FLASH_PAGE_SIZE,
                   program_page, sizeof(program_page)) != 0) {
            matches = false;
            break;
        }
        watchdog_update();
    }
    printf("[FLASH_PROBE] test=readback elapsed_us=%llu result=%u\n",
           (unsigned long long)(time_us_64() - started), matches ? 0u : 1u);
    return matches;
}

bool ota_probe_flash_measurement_run(void)
{
    const bool initial_erased = erase_and_verify("erase_initial");
    bool programmed = false;
    bool readback = false;

    if (initial_erased) {
        programmed = program_and_verify();
    } else {
        printf("[FLASH_PROBE] test=program_pages state=unavailable result=1\n");
    }
    if (programmed) {
        readback = readback_verify();
    } else {
        printf("[FLASH_PROBE] test=readback state=unavailable result=1\n");
    }
    const bool restored = erase_and_verify("erase_restore");

    return initial_erased && programmed && readback && restored;
}
