#include "ota/ota_verifier.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "mbedtls/bignum.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/ecp.h"
#include "mbedtls/sha256.h"

#include "ota_verifier_platform.h"
#if defined(OTA_VERIFIER_MEASUREMENT)
#include "ota_verifier_measurement.h"
#endif

enum {
    OTA_MANIFEST_FORMAT = 1u,
    OTA_RECOVERY_LAYOUT = 1u,
    OTA_PRODUCTION_POLICY = 0x06000001u,
    OTA_DEVELOPMENT_POLICY = 0x86000001u,
    OTA_VERIFIER_ARENA_BYTES = 65536u,
    OTA_VERIFIER_OPERATION_BYTES = OTA_VERIFIER_ARENA_BYTES,
    OTA_VERIFIER_ALIGNMENT = 16u,
};

typedef struct ota_verifier_block {
    size_t bytes;
    struct ota_verifier_block *next;
    bool free;
} ota_verifier_block_t;

typedef struct {
    uint32_t key_id;
    uint32_t image_size;
    uint32_t packed_version;
    const uint8_t *image_hash;
    const uint8_t *signature_r;
    const uint8_t *signature_s;
} ota_manifest_view_t;

static ota_verifier_block_t *g_allocator_head;
static bool g_allocator_failed;
static size_t allocator_header_bytes(void);
#if defined(OTA_VERIFIER_MEASUREMENT)
static ota_verifier_allocator_measurement_t g_allocator_measurement;

void ota_verifier_allocator_measurement_reset(void) {
    memset(&g_allocator_measurement, 0, sizeof(g_allocator_measurement));
}

void ota_verifier_allocator_measurement_get(ota_verifier_allocator_measurement_t *out) {
    if (out != NULL) *out = g_allocator_measurement;
}

static void allocator_measurement_allocate(size_t payload) {
    const size_t header = allocator_header_bytes();
    g_allocator_measurement.current_payload_bytes += payload;
    g_allocator_measurement.current_header_bytes += header;
    if (g_allocator_measurement.current_payload_bytes > g_allocator_measurement.peak_payload_bytes) {
        g_allocator_measurement.peak_payload_bytes = g_allocator_measurement.current_payload_bytes;
    }
    if (g_allocator_measurement.current_header_bytes > g_allocator_measurement.peak_header_bytes) {
        g_allocator_measurement.peak_header_bytes = g_allocator_measurement.current_header_bytes;
    }
}

static void allocator_measurement_free(size_t payload) {
    const size_t header = allocator_header_bytes();
    if (g_allocator_measurement.current_payload_bytes >= payload) {
        g_allocator_measurement.current_payload_bytes -= payload;
    }
    if (g_allocator_measurement.current_header_bytes >= header) {
        g_allocator_measurement.current_header_bytes -= header;
    }
}
#endif

static void wipe_bytes(void *pointer, size_t bytes) {
    volatile uint8_t *cursor = (volatile uint8_t *)pointer;

    while (cursor != NULL && bytes != 0u) {
        *cursor++ = 0u;
        --bytes;
    }
}

static uintptr_t align_up(uintptr_t value) {
    return (value + (OTA_VERIFIER_ALIGNMENT - 1u)) & ~(uintptr_t)(OTA_VERIFIER_ALIGNMENT - 1u);
}

static size_t allocator_header_bytes(void) {
    return (size_t)align_up(sizeof(ota_verifier_block_t));
}

// cppcheck-suppress constParameterPointer
static void allocator_begin(uint8_t *arena, size_t arena_bytes) {
    uintptr_t start;
    uintptr_t aligned;
    size_t available;

    g_allocator_head = NULL;
    g_allocator_failed = false;
    const size_t header_bytes = allocator_header_bytes();
    if (arena == NULL || arena_bytes < header_bytes) {
        g_allocator_failed = true;
        return;
    }
    start = (uintptr_t)arena;
    aligned = align_up(start);
    if (aligned < start || aligned - start > arena_bytes) {
        g_allocator_failed = true;
        return;
    }
    available = arena_bytes - (size_t)(aligned - start);
    if (available <= header_bytes) {
        g_allocator_failed = true;
        return;
    }
    g_allocator_head = (ota_verifier_block_t *)aligned;
    g_allocator_head->bytes = available - header_bytes;
    g_allocator_head->next = NULL;
    g_allocator_head->free = true;
}

static void allocator_end(void) {
    g_allocator_head = NULL;
    g_allocator_failed = false;
}

void *ota_verifier_calloc(size_t count, size_t size) {
    ota_verifier_block_t *block;
    size_t requested;

    if (count != 0u && size > SIZE_MAX / count) {
        g_allocator_failed = true;
        return NULL;
    }
    requested = count * size;
    if (requested == 0u) {
        requested = 1u;
    }
    if (requested > SIZE_MAX - (OTA_VERIFIER_ALIGNMENT - 1u)) {
        g_allocator_failed = true;
        return NULL;
    }
    requested = (size_t)align_up((uintptr_t)requested);
    for (block = g_allocator_head; block != NULL; block = block->next) {
        if (!block->free || block->bytes < requested) {
            continue;
        }
        const size_t header_bytes = allocator_header_bytes();
        if (block->bytes - requested > header_bytes + OTA_VERIFIER_ALIGNMENT) {
            ota_verifier_block_t *split =
                (ota_verifier_block_t *)((uint8_t *)block + header_bytes + requested);
            split->bytes = block->bytes - requested - header_bytes;
            split->next = block->next;
            split->free = true;
            block->next = split;
            block->bytes = requested;
        }
        block->free = false;
#if defined(OTA_VERIFIER_MEASUREMENT)
        allocator_measurement_allocate(block->bytes);
#endif
        memset((uint8_t *)block + header_bytes, 0, block->bytes);
        return (uint8_t *)block + header_bytes;
    }
    g_allocator_failed = true;
    return NULL;
}

void ota_verifier_free(void *pointer) {
    ota_verifier_block_t *block;

    if (pointer == NULL) {
        return;
    }
    block = (ota_verifier_block_t *)((uint8_t *)pointer - allocator_header_bytes());
#if defined(OTA_VERIFIER_MEASUREMENT)
    allocator_measurement_free(block->bytes);
#endif
    block->free = true;
    wipe_bytes(pointer, block->bytes);
    for (block = g_allocator_head; block != NULL && block->next != NULL;) {
        if (block->free && block->next->free) {
            block->bytes += allocator_header_bytes() + block->next->bytes;
            block->next = block->next->next;
        } else {
            block = block->next;
        }
    }
}

static uint16_t read_le16(const uint8_t *source) {
    return (uint16_t)source[0] | ((uint16_t)source[1] << 8u);
}

static uint32_t read_le32(const uint8_t *source) {
    return (uint32_t)source[0] | ((uint32_t)source[1] << 8u) |
           ((uint32_t)source[2] << 16u) | ((uint32_t)source[3] << 24u);
}

static bool is_id_character(uint8_t value) {
    return (value >= (uint8_t)'A' && value <= (uint8_t)'Z') ||
           (value >= (uint8_t)'a' && value <= (uint8_t)'z') ||
           (value >= (uint8_t)'0' && value <= (uint8_t)'9') || value == (uint8_t)'_' ||
           value == (uint8_t)'.' || value == (uint8_t)'-';
}

static bool valid_id(const uint8_t source[32]) {
    size_t cursor = 0u;

    while (cursor < 31u && source[cursor] != 0u) {
        if (!is_id_character(source[cursor])) {
            return false;
        }
        ++cursor;
    }
    if (cursor == 0u || source[cursor] != 0u) {
        return false;
    }
    ++cursor;
    while (cursor < 32u) {
        if (source[cursor++] != 0u) {
            return false;
        }
    }
    return true;
}

static bool policy_id_valid(const ota_verifier_policy_t *policy) {
    if (policy->policy_id == OTA_PRODUCTION_POLICY) {
        return !policy->test_only_allow_downgrade;
    }
    return policy->policy_id == OTA_DEVELOPMENT_POLICY;
}

static bool policy_id_string_valid(const char source[32]) {
    return valid_id((const uint8_t *)source);
}

static bool public_key_valid(const uint8_t key[65]) {
    mbedtls_ecp_group group;
    mbedtls_ecp_point point;
    int result;

    mbedtls_ecp_group_init(&group);
    mbedtls_ecp_point_init(&point);
    result = mbedtls_ecp_group_load(&group, MBEDTLS_ECP_DP_SECP256R1);
    if (result == 0) result = mbedtls_ecp_point_read_binary(&group, &point, key, 65u);
    if (result == 0) result = mbedtls_ecp_check_pubkey(&group, &point);
    mbedtls_ecp_point_free(&point);
    mbedtls_ecp_group_free(&group);
    return result == 0;
}

static ota_verify_result_t validate_policy(const ota_verifier_policy_t *policy) {
    size_t index;

    if (!policy_id_valid(policy) || policy->trusted_key_count == 0u ||
        policy->trusted_key_count > OTA_TRUSTED_KEY_LIMIT || policy->maximum_image_size == 0u ||
        policy->maximum_image_size > 5271552u || policy->recovery_layout_id != OTA_RECOVERY_LAYOUT ||
        policy->execution_origin != 0x10040000u || policy->driver_pack_contract_version == 0u ||
        !policy_id_string_valid(policy->hardware_profile_id) ||
        !policy_id_string_valid(policy->firmware_profile_id) ||
        !policy_id_string_valid(policy->driver_pack_id)) {
        return OTA_VERIFY_MANIFEST_INVALID;
    }
    if (!policy->board_revision_present) {
        return OTA_VERIFY_INCOMPATIBLE;
    }
    for (index = 0u; index < policy->trusted_key_count; ++index) {
        size_t other;
        if (policy->key_id[index] == 0u || policy->public_key_sec1[index][0] != 0x04u ||
            !public_key_valid(policy->public_key_sec1[index])) {
            return OTA_VERIFY_MANIFEST_INVALID;
        }
        for (other = 0u; other < index; ++other) {
            if (policy->key_id[index] == policy->key_id[other]) {
                return OTA_VERIFY_MANIFEST_INVALID;
            }
        }
    }
    return OTA_VERIFY_OK;
}

static ota_verify_result_t parse_manifest(const ota_verifier_policy_t *policy,
                                          const uint8_t manifest[OTA_MANIFEST_BYTES],
                                          ota_manifest_view_t *out_view,
                                          size_t *out_key_index) {
    static const uint8_t magic[8] = {'R', 'P', '2', 'O', 'T', 'A', '0', '6'};
    const uint32_t image_size = read_le32(manifest + 24u);
    const uint32_t version = read_le32(manifest + 28u);
    const uint32_t board_minimum = read_le32(manifest + 36u);
    const uint32_t board_maximum = read_le32(manifest + 40u);
    size_t index;

    if (memcmp(manifest, magic, sizeof(magic)) != 0 || read_le16(manifest + 8u) != OTA_MANIFEST_FORMAT ||
        read_le16(manifest + 10u) != OTA_MANIFEST_SIGNED_BYTES ||
        read_le32(manifest + 12u) != OTA_MANIFEST_BYTES || image_size == 0u ||
        image_size > policy->maximum_image_size || version == 0u || board_minimum > board_maximum ||
        !valid_id(manifest + 48u) || !valid_id(manifest + 80u) || !valid_id(manifest + 112u)) {
        return OTA_VERIFY_MANIFEST_INVALID;
    }
    if (read_le32(manifest + 16u) != policy->recovery_layout_id ||
        read_le32(manifest + 32u) != policy->execution_origin ||
        read_le32(manifest + 44u) != policy->driver_pack_contract_version ||
        memcmp(manifest + 48u, policy->hardware_profile_id, 32u) != 0 ||
        memcmp(manifest + 80u, policy->firmware_profile_id, 32u) != 0 ||
        memcmp(manifest + 112u, policy->driver_pack_id, 32u) != 0) {
        return OTA_VERIFY_INCOMPATIBLE;
    }
    for (index = 176u; index < OTA_MANIFEST_SIGNED_BYTES; ++index) {
        if (manifest[index] != 0u) {
            return OTA_VERIFY_MANIFEST_INVALID;
        }
    }
    if (policy->board_revision < board_minimum || policy->board_revision > board_maximum) {
        return OTA_VERIFY_INCOMPATIBLE;
    }
    if (read_le32(manifest + 20u) == 0u) {
        return OTA_VERIFY_MANIFEST_INVALID;
    }
    for (index = 0u; index < policy->trusted_key_count; ++index) {
        if (policy->key_id[index] == read_le32(manifest + 20u)) {
            break;
        }
    }
    if (index == policy->trusted_key_count) {
        return OTA_VERIFY_UNKNOWN_KEY;
    }
    out_view->key_id = read_le32(manifest + 20u);
    out_view->image_size = image_size;
    out_view->packed_version = version;
    out_view->image_hash = manifest + 144u;
    out_view->signature_r = manifest + 256u;
    out_view->signature_s = manifest + 288u;
    *out_key_index = index;
    return OTA_VERIFY_OK;
}

static int sha256_bytes(const uint8_t *input, size_t bytes, uint8_t digest[32]) {
    mbedtls_sha256_context context;
    int result;

    mbedtls_sha256_init(&context);
    result = mbedtls_sha256_starts(&context, 0);
    if (result == 0) {
        result = mbedtls_sha256_update(&context, input, bytes);
    }
    if (result == 0) {
        result = mbedtls_sha256_finish(&context, digest);
    }
    mbedtls_sha256_free(&context);
    return result;
}

static ota_verify_result_t verify_signature(const uint8_t *signed_bytes, const uint8_t *r_bytes,
                                            const uint8_t *s_bytes, const uint8_t key[65]) {
    uint8_t digest[32];
    mbedtls_ecp_group group;
    mbedtls_ecp_point point;
    mbedtls_mpi r;
    mbedtls_mpi s;
    mbedtls_mpi half_order;
    int result;

    mbedtls_ecp_group_init(&group);
    mbedtls_ecp_point_init(&point);
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);
    mbedtls_mpi_init(&half_order);
    result = sha256_bytes(signed_bytes, OTA_MANIFEST_SIGNED_BYTES, digest);
    if (result == 0) result = mbedtls_ecp_group_load(&group, MBEDTLS_ECP_DP_SECP256R1);
    if (result == 0) result = mbedtls_ecp_point_read_binary(&group, &point, key, 65u);
    if (result == 0) result = mbedtls_ecp_check_pubkey(&group, &point);
    if (result == 0) result = mbedtls_mpi_read_binary(&r, r_bytes, 32u);
    if (result == 0) result = mbedtls_mpi_read_binary(&s, s_bytes, 32u);
    if (result == 0 && (mbedtls_mpi_cmp_int(&r, 1) < 0 || mbedtls_mpi_cmp_mpi(&r, &group.N) >= 0 ||
                        mbedtls_mpi_cmp_int(&s, 1) < 0 || mbedtls_mpi_copy(&half_order, &group.N) != 0 ||
                        mbedtls_mpi_shift_r(&half_order, 1u) != 0 ||
                        mbedtls_mpi_cmp_mpi(&s, &half_order) > 0)) {
        result = -1;
    }
    if (result == 0) result = mbedtls_ecdsa_verify(&group, digest, sizeof(digest), &point, &r, &s);
    wipe_bytes(digest, sizeof(digest));
    mbedtls_mpi_free(&half_order);
    mbedtls_mpi_free(&s);
    mbedtls_mpi_free(&r);
    mbedtls_ecp_point_free(&point);
    mbedtls_ecp_group_free(&group);
    return g_allocator_failed ? OTA_VERIFY_ALLOCATION_FAILED :
                                (result == 0 ? OTA_VERIFY_OK : OTA_VERIFY_SIGNATURE_INVALID);
}

static ota_verify_result_t authenticate_held(const ota_verifier_policy_t *policy,
                                             const uint8_t manifest[OTA_MANIFEST_BYTES],
                                             ota_authenticated_manifest_t *out_authenticated,
                                             ota_manifest_view_t *out_view) {
    ota_manifest_view_t view;
    ota_verify_result_t result;
    size_t key_index = 0u;

    result = validate_policy(policy);
    if (result != OTA_VERIFY_OK && g_allocator_failed) result = OTA_VERIFY_ALLOCATION_FAILED;
    if (result == OTA_VERIFY_OK) result = parse_manifest(policy, manifest, &view, &key_index);
    if (result == OTA_VERIFY_OK) {
        result = verify_signature(manifest, view.signature_r, view.signature_s,
                                  policy->public_key_sec1[key_index]);
    }
    if (result == OTA_VERIFY_OK) {
        memcpy(out_authenticated->manifest, manifest, OTA_MANIFEST_BYTES);
        if (sha256_bytes(manifest, OTA_MANIFEST_SIGNED_BYTES,
                         out_authenticated->content_identity) != 0) {
            result = g_allocator_failed ? OTA_VERIFY_ALLOCATION_FAILED : OTA_VERIFY_SIGNATURE_INVALID;
        } else if ((!policy->test_only_allow_downgrade &&
                    view.packed_version < policy->confirmed_version_floor) ||
                   (view.packed_version == policy->confirmed_version_floor &&
                    memcmp(out_authenticated->content_identity,
                           policy->confirmed_content_identity, 32u) != 0)) {
            result = OTA_VERIFY_VERSION_REJECTED;
        }
        out_authenticated->image_size = view.image_size;
        out_authenticated->packed_version = view.packed_version;
        out_authenticated->trusted_key_id = view.key_id;
    }
    if (result == OTA_VERIFY_OK && out_view != NULL) *out_view = view;
    return result;
}

static ota_verify_result_t acquire_arena(uint8_t **out_arena, size_t *out_bytes) {
    const ota_verifier_platform_result_t result = ota_verifier_platform_acquire(out_arena, out_bytes);
    if (result == OTA_VERIFIER_PLATFORM_BUSY) return OTA_VERIFY_ALLOCATOR_BUSY;
    if (result != OTA_VERIFIER_PLATFORM_READY) return OTA_VERIFY_ALLOCATION_FAILED;
    if (*out_arena == NULL || *out_bytes < OTA_VERIFY_STREAM_CHUNK_BYTES ||
        *out_bytes > OTA_VERIFIER_OPERATION_BYTES) {
        if (*out_arena != NULL) ota_verifier_platform_release(*out_arena, *out_bytes);
        *out_arena = NULL;
        *out_bytes = 0u;
        return OTA_VERIFY_ALLOCATION_FAILED;
    }
    allocator_begin(*out_arena + OTA_VERIFY_STREAM_CHUNK_BYTES,
                    *out_bytes - OTA_VERIFY_STREAM_CHUNK_BYTES);
    if (!g_allocator_failed) return OTA_VERIFY_OK;
    allocator_end();
    ota_verifier_platform_release(*out_arena, *out_bytes);
    *out_arena = NULL;
    *out_bytes = 0u;
    return OTA_VERIFY_ALLOCATION_FAILED;
}

ota_verify_result_t ota_authenticate_manifest(const ota_verifier_policy_t *immutable_policy,
                                              const uint8_t *manifest, size_t manifest_size,
                                              ota_authenticated_manifest_t *out_authenticated) {
    ota_verify_result_t result;
    uint8_t *arena = NULL;
    size_t arena_bytes = 0u;
    bool arena_held = false;

    if (out_authenticated != NULL) memset(out_authenticated, 0, sizeof(*out_authenticated));
    if (immutable_policy == NULL || manifest == NULL || out_authenticated == NULL ||
        manifest_size != OTA_MANIFEST_BYTES) return OTA_VERIFY_MANIFEST_INVALID;
    result = acquire_arena(&arena, &arena_bytes);
    if (result == OTA_VERIFY_OK) {
        arena_held = true;
        result = authenticate_held(immutable_policy, manifest, out_authenticated, NULL);
    }
    if (result != OTA_VERIFY_OK) memset(out_authenticated, 0, sizeof(*out_authenticated));
    if (arena_held) {
        allocator_end();
        ota_verifier_platform_release(arena, arena_bytes);
    }
    return result;
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *bytes, size_t count) {
    size_t index;
    for (index = 0u; index < count; ++index) {
        unsigned bit;
        crc ^= bytes[index];
        for (bit = 0u; bit < 8u; ++bit) crc = (crc >> 1u) ^ (0xedb88320u & (uint32_t)-(int32_t)(crc & 1u));
    }
    return crc;
}

ota_verify_result_t ota_verify_candidate(const ota_verifier_policy_t *immutable_policy,
                                         const uint8_t *manifest, size_t manifest_size,
                                         ota_image_reader_fn reader, void *reader_context,
                                         uint32_t available_image_bytes,
                                         ota_verified_candidate_snapshot_t *out_verified) {
    ota_verify_result_t result;
    ota_manifest_view_t view;
    uint8_t digest[32];
    uint8_t *arena = NULL;
    size_t arena_bytes = 0u;
    uint8_t *stream = NULL;
    uint32_t offset = 0u;
    uint32_t crc = 0xffffffffu;
    mbedtls_sha256_context hash;
    int hash_result;
    bool arena_held = false;

    if (out_verified != NULL) memset(out_verified, 0, sizeof(*out_verified));
    if (immutable_policy == NULL || manifest == NULL || out_verified == NULL || reader == NULL ||
        manifest_size != OTA_MANIFEST_BYTES) return OTA_VERIFY_MANIFEST_INVALID;
    result = acquire_arena(&arena, &arena_bytes);
    if (result != OTA_VERIFY_OK) return result;
    arena_held = true;
    result = authenticate_held(immutable_policy, manifest, &out_verified->authenticated, &view);
    if (result != OTA_VERIFY_OK) goto finish;
    /* authenticate_held() has checked and consumed any allocator failure. */
    g_allocator_failed = false;
    /* Reader callbacks may alter their caller-owned manifest buffer. */
    view.image_hash = out_verified->authenticated.manifest + 144u;
    if (available_image_bytes != view.image_size) {
        result = OTA_VERIFY_IMAGE_HASH_MISMATCH;
        goto finish;
    }
    stream = arena;
    mbedtls_sha256_init(&hash);
    hash_result = mbedtls_sha256_starts(&hash, 0);
    while (hash_result == 0 && offset < view.image_size) {
        const size_t remaining = (size_t)(view.image_size - offset);
        const size_t chunk = remaining > OTA_VERIFY_STREAM_CHUNK_BYTES ? OTA_VERIFY_STREAM_CHUNK_BYTES : remaining;
        if (chunk == 0u || chunk > OTA_VERIFY_STREAM_CHUNK_BYTES || offset > available_image_bytes ||
            chunk > (size_t)(available_image_bytes - offset) || !reader(reader_context, offset, stream, chunk)) {
            result = OTA_VERIFY_READER_FAILED;
            break;
        }
        hash_result = mbedtls_sha256_update(&hash, stream, chunk);
        crc = crc32_update(crc, stream, chunk);
        offset += (uint32_t)chunk;
    }
    if (result == OTA_VERIFY_OK && hash_result == 0) hash_result = mbedtls_sha256_finish(&hash, digest);
    mbedtls_sha256_free(&hash);
    if (result == OTA_VERIFY_OK && (hash_result != 0 || g_allocator_failed)) {
        result = g_allocator_failed ? OTA_VERIFY_ALLOCATION_FAILED : OTA_VERIFY_IMAGE_HASH_MISMATCH;
    }
    if (result == OTA_VERIFY_OK && memcmp(digest, view.image_hash, sizeof(digest)) != 0) result = OTA_VERIFY_IMAGE_HASH_MISMATCH;
    if (result == OTA_VERIFY_OK) out_verified->image_crc32 = crc ^ 0xffffffffu;
finish:
    wipe_bytes(digest, sizeof(digest));
    if (result != OTA_VERIFY_OK) memset(out_verified, 0, sizeof(*out_verified));
    if (arena_held) {
        allocator_end();
        ota_verifier_platform_release(arena, arena_bytes);
    }
    return result;
}
