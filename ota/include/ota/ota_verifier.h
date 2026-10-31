#ifndef OTA_OTA_VERIFIER_H
#define OTA_OTA_VERIFIER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The v0.6.0 signed-package wire format is deliberately fixed width.  These
 * declarations contain no Flash, FreeRTOS, or OPC UA types so the verifier is
 * usable both by the application and the later boot transaction owner.
 */
#define OTA_MANIFEST_BYTES             320u
#define OTA_MANIFEST_SIGNED_BYTES      256u
#define OTA_VERIFY_STREAM_CHUNK_BYTES 4096u
#define OTA_TRUSTED_KEY_LIMIT          2u

typedef bool (*ota_image_reader_fn)(void *context, uint32_t offset,
                                    uint8_t *destination, size_t length);

typedef struct {
    uint32_t key_id[OTA_TRUSTED_KEY_LIMIT];
    uint8_t public_key_sec1[OTA_TRUSTED_KEY_LIMIT][65];
    size_t trusted_key_count;
    char hardware_profile_id[32];
    char firmware_profile_id[32];
    char driver_pack_id[32];
    uint32_t driver_pack_contract_version;
    uint32_t board_revision;
    bool board_revision_present;
    uint32_t recovery_layout_id;
    uint32_t execution_origin;
    uint32_t policy_id;
    uint32_t maximum_image_size;
    uint32_t confirmed_version_floor;
    uint8_t confirmed_content_identity[32];
    bool test_only_allow_downgrade;
} ota_verifier_policy_t;

typedef enum {
    OTA_VERIFY_OK = 0,
    OTA_VERIFY_MANIFEST_INVALID,
    OTA_VERIFY_UNKNOWN_KEY,
    OTA_VERIFY_SIGNATURE_INVALID,
    OTA_VERIFY_IMAGE_HASH_MISMATCH,
    OTA_VERIFY_INCOMPATIBLE,
    OTA_VERIFY_VERSION_REJECTED,
    OTA_VERIFY_READER_FAILED,
    OTA_VERIFY_ALLOCATION_FAILED,
    OTA_VERIFY_ALLOCATOR_BUSY,
} ota_verify_result_t;

typedef struct {
    uint8_t manifest[OTA_MANIFEST_BYTES];
    uint8_t content_identity[32];
    uint32_t image_size;
    uint32_t packed_version;
    uint32_t trusted_key_id;
} ota_authenticated_manifest_t;

typedef struct {
    ota_authenticated_manifest_t authenticated;
    uint32_t image_crc32;
} ota_verified_candidate_snapshot_t;

ota_verify_result_t ota_authenticate_manifest(
    const ota_verifier_policy_t *immutable_policy,
    const uint8_t *manifest, size_t manifest_size,
    ota_authenticated_manifest_t *out_authenticated);

ota_verify_result_t ota_verify_candidate(
    const ota_verifier_policy_t *immutable_policy,
    const uint8_t *manifest, size_t manifest_size,
    ota_image_reader_fn reader, void *reader_context,
    uint32_t available_image_bytes,
    ota_verified_candidate_snapshot_t *out_verified);

/*
 * Private entry points selected by the separately namespaced T1 mbedTLS
 * library.  They are intentionally not general-purpose allocation APIs.
 */
void *ota_verifier_calloc(size_t count, size_t size);
void ota_verifier_free(void *pointer);

#ifdef __cplusplus
}
#endif

#endif /* OTA_OTA_VERIFIER_H */
