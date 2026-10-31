#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ota/ota_verifier.h"
#include "ota_verifier_platform.h"


enum { ARENA_BYTES = 65536u };

static uint8_t g_arena[ARENA_BYTES] __attribute__((aligned(16)));
static bool g_busy;
static bool g_allocation_failure;
static size_t g_arena_bytes = sizeof(g_arena);

typedef struct {
    const uint8_t *image;
    size_t image_bytes;
    unsigned calls;
    size_t largest_call;
    bool fail_reader;
    bool reenter;
    uint8_t *mutate_manifest;
    const ota_verifier_policy_t *policy;
    const uint8_t *manifest;
} reader_context_t;

ota_verifier_platform_result_t ota_verifier_platform_acquire(uint8_t **out_arena,
                                                              size_t *out_arena_bytes) {
    *out_arena = NULL;
    *out_arena_bytes = 0u;
    if (g_busy) return OTA_VERIFIER_PLATFORM_BUSY;
    if (g_allocation_failure) return OTA_VERIFIER_PLATFORM_ALLOCATION_FAILED;
    g_busy = true;
    *out_arena = g_arena;
    *out_arena_bytes = g_arena_bytes;
    return OTA_VERIFIER_PLATFORM_READY;
}

void ota_verifier_platform_release(uint8_t *arena, size_t arena_bytes) {
    (void)arena;
    (void)arena_bytes;
    g_busy = false;
}

static void fail(const char *message) {
    fprintf(stderr, "FAIL: %s\n", message);
    exit(1);
}

static uint8_t *read_file(const char *path, size_t *out_bytes) {
    FILE *file = fopen(path, "rb");
    long length;
    uint8_t *bytes;
    if (file == NULL || fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0 ||
        fseek(file, 0, SEEK_SET) != 0) fail("cannot open fixture");
    bytes = malloc((size_t)length);
    if (bytes == NULL || fread(bytes, 1u, (size_t)length, file) != (size_t)length) fail("cannot read fixture");
    fclose(file);
    *out_bytes = (size_t)length;
    return bytes;
}

static bool all_zero(const void *source, size_t bytes) {
    const uint8_t *cursor = source;
    while (bytes-- != 0u) if (*cursor++ != 0u) return false;
    return true;
}

static bool reader(void *opaque, uint32_t offset, uint8_t *destination, size_t length) {
    reader_context_t *context = opaque;
    ++context->calls;
    if (length > context->largest_call) context->largest_call = length;
    if (context->reenter && context->calls == 1u) {
        ota_authenticated_manifest_t nested;
        if (ota_authenticate_manifest(context->policy, context->manifest, OTA_MANIFEST_BYTES, &nested) !=
            OTA_VERIFY_ALLOCATOR_BUSY || !all_zero(&nested, sizeof(nested))) return false;
    }
    if (context->mutate_manifest != NULL && context->calls == 1u) {
        context->mutate_manifest[144] ^= 1u;
    }
    if (context->fail_reader || (size_t)offset > context->image_bytes ||
        length > context->image_bytes - (size_t)offset) return false;
    memcpy(destination, context->image + offset, length);
    return true;
}

static void write_le32(uint8_t *target, uint32_t value) {
    target[0] = (uint8_t)value;
    target[1] = (uint8_t)(value >> 8u);
    target[2] = (uint8_t)(value >> 16u);
    target[3] = (uint8_t)(value >> 24u);
}

static void subtract_be(uint8_t destination[32], const uint8_t left[32],
                        const uint8_t right[32]) {
    int index;
    unsigned borrow = 0u;

    for (index = 31; index >= 0; --index) {
        const unsigned minuend = left[index];
        const unsigned subtrahend = right[index] + borrow;
        destination[index] = (uint8_t)(minuend - subtrahend);
        borrow = minuend < subtrahend ? 1u : 0u;
    }
    if (borrow != 0u) fail("invalid scalar subtraction");
}

static void expect_auth(const ota_verifier_policy_t *policy, const uint8_t *manifest,
                        size_t bytes, ota_verify_result_t expected) {
    ota_authenticated_manifest_t output;
    memset(&output, 0xa5, sizeof(output));
    const ota_verify_result_t result = ota_authenticate_manifest(policy, manifest, bytes, &output);
    if (result != expected) {
        fprintf(stderr, "authentication expected=%d got=%d\n", expected, result);
        fail("unexpected authentication result");
    }
    if (result != OTA_VERIFY_OK && !all_zero(&output, sizeof(output))) fail("authentication output leak");
}

static void expect_candidate(const ota_verifier_policy_t *policy, const uint8_t *manifest,
                             uint32_t available, reader_context_t *reader_context,
                             ota_verify_result_t expected) {
    ota_verified_candidate_snapshot_t output;
    memset(&output, 0xa5, sizeof(output));
    const ota_verify_result_t result = ota_verify_candidate(policy, manifest, OTA_MANIFEST_BYTES,
                                                             reader, reader_context, available, &output);
    if (result != expected) {
        fprintf(stderr, "candidate expected=%d got=%d\n", expected, result);
        fail("unexpected candidate result");
    }
    if (result != OTA_VERIFY_OK && !all_zero(&output, sizeof(output))) fail("candidate output leak");
}

int main(int argc, char **argv) {
    size_t manifest_bytes;
    size_t image_bytes;
    size_t public_key_bytes;
    uint8_t *manifest;
    uint8_t *image;
    uint8_t *public_key;
    uint8_t changed[OTA_MANIFEST_BYTES];
    ota_verifier_policy_t policy = {0};
    reader_context_t context = {0};
    ota_verified_candidate_snapshot_t verified;

    if (argc != 4) fail("usage: ota_verifier_test manifest image public-key");
    manifest = read_file(argv[1], &manifest_bytes);
    image = read_file(argv[2], &image_bytes);
    public_key = read_file(argv[3], &public_key_bytes);
    if (manifest_bytes != OTA_MANIFEST_BYTES || public_key_bytes != 65u || image_bytes > UINT32_MAX) fail("bad fixture");
    policy.key_id[0] = 7u;
    memcpy(policy.public_key_sec1[0], public_key, public_key_bytes);
    policy.trusted_key_count = 1u;
    memcpy(policy.hardware_profile_id, "waveshare_core2350b_16m8m", 25u);
    memcpy(policy.firmware_profile_id, "device_profile_basic", 21u);
    memcpy(policy.driver_pack_id, "base", 5u);
    policy.driver_pack_contract_version = 1u;
    policy.board_revision = 7u;
    policy.board_revision_present = true;
    policy.recovery_layout_id = 1u;
    policy.execution_origin = 0x10040000u;
    policy.policy_id = 0x06000001u;
    policy.maximum_image_size = 5271552u;
    context.image = image;
    context.image_bytes = image_bytes;
    context.policy = &policy;
    context.manifest = manifest;

    expect_auth(&policy, manifest, OTA_MANIFEST_BYTES, OTA_VERIFY_OK);
    if (ota_verify_candidate(&policy, manifest, OTA_MANIFEST_BYTES, reader, &context,
                             (uint32_t)image_bytes, &verified) != OTA_VERIFY_OK ||
        verified.authenticated.image_size != image_bytes || context.largest_call > OTA_VERIFY_STREAM_CHUNK_BYTES ||
        context.calls == 0u) fail("valid candidate");
    if (ota_authenticate_manifest(NULL, manifest, OTA_MANIFEST_BYTES, NULL) != OTA_VERIFY_MANIFEST_INVALID ||
        ota_verify_candidate(&policy, manifest, OTA_MANIFEST_BYTES, NULL, &context,
                             (uint32_t)image_bytes, &verified) != OTA_VERIFY_MANIFEST_INVALID) fail("null arguments");
    expect_auth(&policy, manifest, OTA_MANIFEST_BYTES - 1u, OTA_VERIFY_MANIFEST_INVALID);
    memcpy(changed, manifest, sizeof(changed));
    changed[0] ^= 1u;
    expect_auth(&policy, changed, sizeof(changed), OTA_VERIFY_MANIFEST_INVALID);
    memcpy(changed, manifest, sizeof(changed));
    write_le32(changed + 20u, 8u);
    expect_auth(&policy, changed, sizeof(changed), OTA_VERIFY_UNKNOWN_KEY);
    memcpy(changed, manifest, sizeof(changed));
    write_le32(changed + 20u, 0u);
    expect_auth(&policy, changed, sizeof(changed), OTA_VERIFY_MANIFEST_INVALID);
    memcpy(changed, manifest, sizeof(changed));
    changed[176] = 1u;
    expect_auth(&policy, changed, sizeof(changed), OTA_VERIFY_MANIFEST_INVALID);
    memcpy(changed, manifest, sizeof(changed));
    changed[48] = (uint8_t)'X';
    expect_auth(&policy, changed, sizeof(changed), OTA_VERIFY_INCOMPATIBLE);
    memcpy(changed, manifest, sizeof(changed));
    write_le32(changed + 36u, 8u);
    expect_auth(&policy, changed, sizeof(changed), OTA_VERIFY_INCOMPATIBLE);
    memcpy(changed, manifest, sizeof(changed));
    changed[256] = 0u;
    memset(changed + 257u, 0, 31u);
    expect_auth(&policy, changed, sizeof(changed), OTA_VERIFY_SIGNATURE_INVALID);
    memcpy(changed, manifest, sizeof(changed));
    memset(changed + 256u, 0xff, 32u);
    expect_auth(&policy, changed, sizeof(changed), OTA_VERIFY_SIGNATURE_INVALID);
    memcpy(changed, manifest, sizeof(changed));
    memset(changed + 288u, 0, 32u);
    expect_auth(&policy, changed, sizeof(changed), OTA_VERIFY_SIGNATURE_INVALID);
    memcpy(changed, manifest, sizeof(changed));
    memset(changed + 288u, 0xff, 32u);
    expect_auth(&policy, changed, sizeof(changed), OTA_VERIFY_SIGNATURE_INVALID);
    memcpy(changed, manifest, sizeof(changed));
    {
        static const uint8_t p256_order[32] = {
            0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00,
            0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
            0xbc, 0xe6, 0xfa, 0xad, 0xa7, 0x17, 0x9e, 0x84,
            0xf3, 0xb9, 0xca, 0xc2, 0xfc, 0x63, 0x25, 0x51,
        };
        subtract_be(changed + 288u, p256_order, manifest + 288u);
    }
    expect_auth(&policy, changed, sizeof(changed), OTA_VERIFY_SIGNATURE_INVALID);
    policy.public_key_sec1[0][0] = 0u;
    expect_auth(&policy, manifest, sizeof(changed), OTA_VERIFY_MANIFEST_INVALID);
    policy.public_key_sec1[0][0] = 0x04u;
    policy.trusted_key_count = 2u;
    memcpy(policy.public_key_sec1[1], policy.public_key_sec1[0], 65u);
    policy.key_id[1] = policy.key_id[0];
    expect_auth(&policy, manifest, sizeof(changed), OTA_VERIFY_MANIFEST_INVALID);
    policy.key_id[1] = 8u;
    expect_auth(&policy, manifest, sizeof(changed), OTA_VERIFY_OK);
    policy.trusted_key_count = 1u;
    memcpy(changed, manifest, sizeof(changed));
    memset(changed + 48u, (uint8_t)'a', 31u);
    changed[79] = 0u;
    memset(policy.hardware_profile_id, 'a', 31u);
    policy.hardware_profile_id[31] = '\0';
    expect_auth(&policy, changed, sizeof(changed), OTA_VERIFY_SIGNATURE_INVALID);
    memset(policy.hardware_profile_id, 0, sizeof(policy.hardware_profile_id));
    memcpy(policy.hardware_profile_id, "waveshare_core2350b_16m8m", 25u);
    policy.confirmed_version_floor = 2u;
    expect_auth(&policy, manifest, sizeof(changed), OTA_VERIFY_VERSION_REJECTED);
    policy.confirmed_version_floor = 1u;
    expect_auth(&policy, manifest, sizeof(changed), OTA_VERIFY_VERSION_REJECTED);
    policy.policy_id = 0x86000001u;
    policy.test_only_allow_downgrade = true;
    policy.confirmed_version_floor = 2u;
    expect_auth(&policy, manifest, sizeof(changed), OTA_VERIFY_OK);
    policy.policy_id = 0x06000001u;
    policy.test_only_allow_downgrade = true;
    expect_auth(&policy, manifest, sizeof(changed), OTA_VERIFY_MANIFEST_INVALID);
    policy.test_only_allow_downgrade = false;
    policy.board_revision_present = false;
    expect_auth(&policy, manifest, sizeof(changed), OTA_VERIFY_INCOMPATIBLE);
    policy.board_revision_present = true;
    policy.confirmed_version_floor = 0u;
    context.calls = 0u;
    context.largest_call = 0u;
    context.fail_reader = true;
    expect_candidate(&policy, manifest, (uint32_t)image_bytes, &context, OTA_VERIFY_READER_FAILED);
    context.fail_reader = false;
    expect_candidate(&policy, manifest, (uint32_t)image_bytes - 1u, &context, OTA_VERIFY_IMAGE_HASH_MISMATCH);
    image[0] ^= 1u;
    expect_candidate(&policy, manifest, (uint32_t)image_bytes, &context, OTA_VERIFY_IMAGE_HASH_MISMATCH);
    image[0] ^= 1u;
    g_busy = true;
    expect_auth(&policy, manifest, sizeof(changed), OTA_VERIFY_ALLOCATOR_BUSY);
    g_busy = false;
    g_arena_bytes = OTA_VERIFY_STREAM_CHUNK_BYTES;
    expect_auth(&policy, manifest, sizeof(changed), OTA_VERIFY_ALLOCATION_FAILED);
    g_arena_bytes = OTA_VERIFY_STREAM_CHUNK_BYTES + 4096u;
    expect_auth(&policy, manifest, sizeof(changed), OTA_VERIFY_ALLOCATION_FAILED);
    g_arena_bytes = sizeof(g_arena);
    context.calls = 0u;
    context.reenter = true;
    context.mutate_manifest = manifest;
    expect_candidate(&policy, manifest, (uint32_t)image_bytes, &context, OTA_VERIFY_OK);
    manifest[144] ^= 1u;
    context.mutate_manifest = NULL;
    context.reenter = false;
    free(public_key);
    free(image);
    free(manifest);
    puts("OTA verifier native suite: PASS");
    return 0;
}
