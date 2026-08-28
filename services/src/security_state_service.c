/**
 * @file
 * @brief Flash-backed runtime/commissioning state snapshot for the security
 * subsystem. See header for module-level docs.
 *
 * Implementation note: every mutator here follows the same
 * read-current/copy-to-local-image/patch-field/commit pattern as
 * `security_service.c`, but always erases and reprograms the *entire*
 * `OTA_LAYOUT_SECURITY_STATE_SIZE_BYTES` region (no partial writes) via
 * `flash_operation_coordinator`.
 */
#include "services/security_state_service.h"

#include <stddef.h>
#include <string.h>

#include <FreeRTOS.h>

#include "drivers/flash_operation_coordinator.h"
#include "ota_layout.h"
#include "services/time_service.h"

#define SECURITY_STATE_SERVICE_STORE_MAGIC 0x53545331u /* "STS1" */
#define SECURITY_STATE_SERVICE_STORE_VERSION 1u
#define SECURITY_STATE_SERVICE_FLAG_COMMISSIONING_MODE (1u << 0u)

/**
 * @brief On-Flash layout of the security-state store, at
 * `OTA_LAYOUT_SECURITY_STATE_ORIGIN`. Validated by magic/version/CRC32
 * before use; `reserved` pads for future fields without breaking the
 * `_Static_assert` size bound below.
 */
typedef struct
{
    uint32_t magic;
    uint16_t version;
    uint16_t flags;
    uint32_t trust_list_version;
    uint32_t commissioning_entered_epoch_s;
    uint32_t commissioning_deadline_epoch_s;
    uint32_t active_identity_generation;
    uint32_t active_trust_generation;
    uint32_t pending_identity_generation;
    uint32_t pending_trust_generation;
    uint32_t last_security_fault_code;
    uint32_t reserved[4];
    uint32_t crc32;
} security_state_store_t;

/** True once a valid store has been loaded/seeded by `security_state_service_init()`; gates every other accessor. */
static bool g_security_state_ready = false;

_Static_assert(sizeof(security_state_store_t) <= OTA_LAYOUT_SECURITY_STATE_SIZE_BYTES,
               "Security state store does not fit into the reserved security state region");

/** @brief Standard CRC-32 (poly 0xEDB88320, reflected) over @p size bytes of @p data. */
static uint32_t security_state_service_crc32(const uint8_t *data, size_t size)
{
    uint32_t crc = 0xFFFFFFFFu;

    for (size_t i = 0u; i < size; ++i)
    {
        crc ^= data[i];
        for (uint32_t bit = 0u; bit < 8u; ++bit)
        {
            const uint32_t mask = (uint32_t)-(int32_t)(crc & 1u);
            crc = (crc >> 1u) ^ (0xEDB88320u & mask);
        }
    }

    return ~crc;
}

/** @brief Raw pointer to the XIP-mapped Flash store; does not check validity or readiness. */
static const security_state_store_t *security_state_service_flash_store(void)
{
    return (const security_state_store_t *)OTA_LAYOUT_SECURITY_STATE_ORIGIN;
}

/** @brief Returns the Flash store only if `security_state_service_init()` has succeeded; NULL otherwise. */
static const security_state_store_t *security_state_service_store(void)
{
    if (!g_security_state_ready)
    {
        return NULL;
    }

    return security_state_service_flash_store();
}

static bool security_state_service_get_commissioning_flag(const security_state_store_t *store)
{
    return store != NULL &&
           (store->flags & SECURITY_STATE_SERVICE_FLAG_COMMISSIONING_MODE) != 0u;
}

/** @brief Rounds @p image_size up to a whole number of Flash program-page units. */
static size_t security_state_service_get_program_size(size_t image_size)
{
    return ((image_size + OTA_LAYOUT_FLASH_PROGRAM_SIZE_BYTES - 1u) /
            OTA_LAYOUT_FLASH_PROGRAM_SIZE_BYTES) * OTA_LAYOUT_FLASH_PROGRAM_SIZE_BYTES;
}

/**
 * @brief Sets/clears the commissioning-mode flag in a local store image and,
 * on transition, resets `commissioning_entered_epoch_s`
 * (to the current system time when entering, to 0 when leaving) and
 * `commissioning_deadline_epoch_s` (always cleared to 0 — no deadline
 * enforcement is currently implemented).
 */
static void security_state_service_set_commissioning_flag(security_state_store_t *store, bool enabled)
{
    if (store == NULL)
    {
        return;
    }

    if (enabled)
    {
        store->flags = (uint16_t)(store->flags | SECURITY_STATE_SERVICE_FLAG_COMMISSIONING_MODE);
        store->commissioning_entered_epoch_s = get_system_time();
        store->commissioning_deadline_epoch_s = 0u;
    }
    else
    {
        store->flags = (uint16_t)(store->flags & (uint16_t)~SECURITY_STATE_SERVICE_FLAG_COMMISSIONING_MODE);
        store->commissioning_entered_epoch_s = 0u;
        store->commissioning_deadline_epoch_s = 0u;
    }
}

/** @brief Fills @p store with a zeroed, freshly-initialized state record (magic/version/counters all reset). */
static void security_state_service_reset_defaults(security_state_store_t *store,
                                                  bool commissioning_mode,
                                                  uint32_t trust_list_version)
{
    memset(store, 0, sizeof(*store));
    store->magic = SECURITY_STATE_SERVICE_STORE_MAGIC;
    store->version = SECURITY_STATE_SERVICE_STORE_VERSION;
    store->trust_list_version = trust_list_version;
    security_state_service_set_commissioning_flag(store, commissioning_mode);
}

/** @brief Checks magic, version, and CRC32 of a candidate store; the sole gate for trusting Flash contents. */
static bool security_state_service_store_is_valid(const security_state_store_t *store)
{
    if (store == NULL)
    {
        return false;
    }

    if (store->magic != SECURITY_STATE_SERVICE_STORE_MAGIC ||
        store->version != SECURITY_STATE_SERVICE_STORE_VERSION)
    {
        return false;
    }

    return security_state_service_crc32((const uint8_t *)store,
                                        offsetof(security_state_store_t, crc32)) == store->crc32;
}

/**
 * @brief Stamps magic/version/CRC32 onto @p store, then erases and
 * reprograms the entire `OTA_LAYOUT_SECURITY_STATE_SIZE_BYTES` Flash region
 * with it via `flash_operation_coordinator`.
 *
 * @note Two-phase: erase all sectors first, then program all pages; a
 * failure partway through either phase leaves the region in an
 * indeterminate state (some sectors erased/programmed, others not) — no
 * rollback is attempted. On success, sets `g_security_state_ready = true`.
 * @return false on NULL input, allocation failure of the scratch program
 * buffer, or any individual erase/program operation failure.
 */
static bool security_state_service_commit_store(security_state_store_t *store)
{
    uint8_t *program_image = NULL;
    const size_t program_size = security_state_service_get_program_size(sizeof(*store));
    bool committed = false;

    if (store == NULL)
    {
        return false;
    }

    store->magic = SECURITY_STATE_SERVICE_STORE_MAGIC;
    store->version = SECURITY_STATE_SERVICE_STORE_VERSION;
    store->crc32 = security_state_service_crc32((const uint8_t *)store,
                                                offsetof(security_state_store_t, crc32));

    program_image = (uint8_t *)pvPortMalloc(program_size);
    if (program_image == NULL)
    {
        return false;
    }

    memset(program_image, 0xFF, program_size);
    memcpy(program_image, store, sizeof(*store));

    for (uint32_t offset = 0u; offset < OTA_LAYOUT_SECURITY_STATE_SIZE_BYTES;
         offset += OTA_LAYOUT_FLASH_ERASE_SIZE_BYTES)
    {
        if (flash_operation_erase_sector(OTA_LAYOUT_SECURITY_STATE_OFFSET_BYTES + offset) != FLASH_OPERATION_OK)
        {
            goto cleanup;
        }
    }
    for (size_t offset = 0u; offset < program_size; offset += OTA_LAYOUT_FLASH_PROGRAM_SIZE_BYTES)
    {
        if (flash_operation_program_page(OTA_LAYOUT_SECURITY_STATE_OFFSET_BYTES + (uint32_t)offset,
                                         program_image + offset) != FLASH_OPERATION_OK)
        {
            goto cleanup;
        }
    }

    g_security_state_ready = true;
    committed = true;

cleanup:
    vPortFree(program_image);
    return committed;
}

bool security_state_service_init(bool default_commissioning_mode,
                                 uint32_t default_trust_list_version)
{
    const security_state_store_t *flash_store = security_state_service_flash_store();
    security_state_store_t image;

    if (security_state_service_store_is_valid(flash_store))
    {
        g_security_state_ready = true;
        return true;
    }

    security_state_service_reset_defaults(&image,
                                          default_commissioning_mode,
                                          default_trust_list_version);
    return security_state_service_commit_store(&image);
}

bool security_state_service_get_snapshot(security_state_service_snapshot_t *out_snapshot)
{
    const security_state_store_t *store = security_state_service_store();

    if (out_snapshot == NULL || store == NULL)
    {
        return false;
    }

    memset(out_snapshot, 0, sizeof(*out_snapshot));
    out_snapshot->commissioning_mode = security_state_service_get_commissioning_flag(store);
    out_snapshot->trust_list_version = store->trust_list_version;
    out_snapshot->commissioning_entered_epoch_s = store->commissioning_entered_epoch_s;
    out_snapshot->commissioning_deadline_epoch_s = store->commissioning_deadline_epoch_s;
    out_snapshot->active_identity_generation = store->active_identity_generation;
    out_snapshot->active_trust_generation = store->active_trust_generation;
    out_snapshot->pending_identity_generation = store->pending_identity_generation;
    out_snapshot->pending_trust_generation = store->pending_trust_generation;
    out_snapshot->last_security_fault_code = store->last_security_fault_code;
    return true;
}

bool security_state_service_get_commissioning_mode(bool *out_enabled)
{
    const security_state_store_t *store = security_state_service_store();

    if (out_enabled == NULL || store == NULL)
    {
        return false;
    }

    *out_enabled = security_state_service_get_commissioning_flag(store);
    return true;
}

bool security_state_service_set_commissioning_mode(bool enabled)
{
    const security_state_store_t *store = security_state_service_store();
    security_state_store_t image;

    if (store == NULL)
    {
        return false;
    }

    if (security_state_service_get_commissioning_flag(store) == enabled)
    {
        return true;
    }

    image = *store;
    security_state_service_set_commissioning_flag(&image, enabled);
    return security_state_service_commit_store(&image);
}

bool security_state_service_get_trust_list_version(uint32_t *out_version)
{
    const security_state_store_t *store = security_state_service_store();

    if (out_version == NULL || store == NULL)
    {
        return false;
    }

    *out_version = store->trust_list_version;
    return true;
}

bool security_state_service_set_trust_list_version(uint32_t version)
{
    const security_state_store_t *store = security_state_service_store();
    security_state_store_t image;

    if (store == NULL)
    {
        return false;
    }

    if (store->trust_list_version == version)
    {
        return true;
    }

    image = *store;
    image.trust_list_version = version;
    return security_state_service_commit_store(&image);
}

bool security_state_service_set_trust_generations(uint32_t active_generation,
                                                  uint32_t pending_generation)
{
    const security_state_store_t *store = security_state_service_store();
    security_state_store_t image;

    if (store == NULL)
    {
        return false;
    }

    if (store->active_trust_generation == active_generation &&
        store->pending_trust_generation == pending_generation)
    {
        return true;
    }

    image = *store;
    image.active_trust_generation = active_generation;
    image.pending_trust_generation = pending_generation;
    return security_state_service_commit_store(&image);
}

bool security_state_service_set_identity_generations(uint32_t active_generation,
                                                     uint32_t pending_generation)
{
    const security_state_store_t *store = security_state_service_store();
    security_state_store_t image;

    if (store == NULL)
    {
        return false;
    }

    if (store->active_identity_generation == active_generation &&
        store->pending_identity_generation == pending_generation)
    {
        return true;
    }

    image = *store;
    image.active_identity_generation = active_generation;
    image.pending_identity_generation = pending_generation;
    return security_state_service_commit_store(&image);
}
