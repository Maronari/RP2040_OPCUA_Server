#include "ota/ota_state.h"

#include <assert.h>
#include <string.h>

static void expect_invalid(const ota_state_record_t *record)
{
    assert(!ota_state_validate(record));
}

static void test_reset_finalize_and_validation(void)
{
    ota_state_record_t record;
    ota_state_record_t original;

    ota_state_reset_record(NULL);
    ota_state_finalize(NULL);
    assert(!ota_state_validate(NULL));

    memset(&record, 0xa5, sizeof(record));
    ota_state_reset_record(&record);
    assert(record.magic == OTA_STATE_MAGIC);
    assert(record.format_version == OTA_STATE_FORMAT_VERSION);
    assert(record.state == OTA_IMAGE_STATE_IDLE);
    assert(record.last_error == OTA_IMAGE_ERROR_NONE);
    assert(ota_state_validate(&record));

    record.state = OTA_IMAGE_STATE_STAGED_IN_BANK_B;
    record.image_size = 4096u;
    record.image_crc32 = 0x12345678u;
    record.version_number = 17u;
    ota_state_finalize(&record);
    assert(ota_state_validate(&record));
    original = record;

    record.magic ^= 1u;
    expect_invalid(&record);
    record = original;
    record.format_version++;
    expect_invalid(&record);
    record = original;
    record.record_crc32 ^= 1u;
    expect_invalid(&record);
    record = original;
    record.image_size ^= 1u;
    expect_invalid(&record);
    record = original;
    record.reserved[5] ^= 1u;
    expect_invalid(&record);
}

static void test_crc_and_string_mappings(void)
{
    assert(ota_crc32_compute(NULL, 1u) == 0u);
    assert(ota_crc32_compute("123456789", 9u) == 0xcbf43926u);
    assert(strcmp(ota_state_to_string(OTA_IMAGE_STATE_IDLE), "IDLE") == 0);
    assert(strcmp(ota_state_to_string(99u), "UNKNOWN") == 0);
    assert(strcmp(ota_error_to_string(OTA_IMAGE_ERROR_VERIFY_FAILED), "VERIFY_FAILED") == 0);
    assert(strcmp(ota_error_to_string(99u), "UNKNOWN") == 0);
}

int main(void)
{
    test_reset_finalize_and_validation();
    test_crc_and_string_mappings();
    return 0;
}
