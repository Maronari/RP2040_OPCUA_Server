/**
 * @file
 * @brief Central security coordinator implementation: device identity,
 * trust list, local user accounts, commissioning mode. See header for
 * module-level docs.
 *
 * Implementation note: every mutator follows the same pattern —
 * `security_service_store()` for a read-only view of the current Flash
 * image, `security_service_allocate_image()` to heap-copy it, patch the
 * copy, `security_service_commit_store()` to erase+reprogram the entire
 * reserved region, then free the heap copy. There is no partial/incremental
 * Flash write and no rollback if a commit fails partway through erase vs.
 * program phases.
 */
#include "services/security_service.h"

#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <FreeRTOS.h>

#include "drivers/flash_operation_coordinator.h"
#include "mbedtls/sha1.h"
#include "mbedtls/x509_crt.h"
#include "ota_layout.h"
#include "pico/rand.h"
#include "services/login_backoff.h"
#include "services/security_state_service.h"
#include "pico/sha256.h"
#include "pico/stdio.h"
#include "pico/stdlib.h"
#include "services/time_service.h"

#define SECURITY_SERVICE_STORE_MAGIC            0x53454331u /* "SEC1" */
#define SECURITY_SERVICE_STORE_VERSION          1u
#define SECURITY_SERVICE_DEFAULT_ADMIN_USER     "admin"
#define SECURITY_SERVICE_DEFAULT_ADMIN_PASSWORD "admin"
#define SECURITY_SERVICE_FLAG_COMMISSIONING_MODE (1u << 0u)

#define fflush(_stream) stdio_flush()

/**
 * @brief On-Flash record for a single local user account.
 * `in_use` distinguishes an allocated slot from an empty one (independent
 * of `enabled`, which gates login without freeing the slot).
 */
typedef struct
{
    uint8_t in_use;
    uint8_t enabled;
    uint8_t role;
    uint8_t reserved;
    char username[SECURITY_SERVICE_USERNAME_MAX_LEN];
    uint8_t salt[SECURITY_SERVICE_SALT_LEN];
    uint8_t password_hash[SECURITY_SERVICE_PASSWORD_HASH_LEN];
} security_service_user_record_t;

/**
 * @brief On-Flash layout of the security store, at
 * `OTA_LAYOUT_SECURITY_STORE_ORIGIN`. Validated by magic/version/length
 * bounds/CRC32 before use (`security_service_store_is_valid()`).
 * `reserved0` doubles as the commissioning-mode flag bitfield (see
 * `SECURITY_SERVICE_FLAG_COMMISSIONING_MODE`).
 */
typedef struct
{
    uint32_t magic;
    uint16_t version;
    uint16_t reserved0;
    uint32_t local_certificate_length;
    uint32_t private_key_length;
    uint32_t trust_list_length;
    uint32_t trust_list_version;
    security_service_user_record_t users[SECURITY_SERVICE_MAX_USERS];
    uint8_t local_certificate[SECURITY_SERVICE_CERT_MAX_LEN];
    uint8_t private_key[SECURITY_SERVICE_KEY_MAX_LEN];
    uint8_t trust_list[SECURITY_SERVICE_TRUST_LIST_MAX_LEN];
    uint32_t crc32;
} security_service_store_t;

/** Per-user (or per-unknown-username) login-backoff bookkeeping; runtime-only, not persisted. */
typedef login_backoff_t security_service_auth_runtime_t;

/** True once `security_service_init()` has established a valid Flash store; gates every other accessor. */
static bool g_security_store_ready = false;
/** Login-backoff state indexed by user-record slot index (parallel array to `security_service_store_t::users`). */
static security_service_auth_runtime_t g_user_auth_runtime[SECURITY_SERVICE_MAX_USERS];
/** Shared login-backoff bucket for unknown usernames, so timing does not reveal whether a username exists. */
static security_service_auth_runtime_t g_unknown_auth_runtime = {0u, 0u};
/** Total authentication failures since boot; not persisted, exposed via `security_service_get_failed_login_count()`. */
static uint32_t g_failed_login_count = 0u;
/** Epoch second of the most recent authentication failure since boot; 0 means none recorded. */
static uint32_t g_last_auth_failure_epoch_s = 0u;
/** Registered audit-event sink, or NULL; see `security_service_set_audit_callback()`. */
static security_service_audit_callback_t g_audit_callback = NULL;
/** Opaque context pointer passed through to `g_audit_callback`. */
static void *g_audit_callback_context = NULL;

_Static_assert(sizeof(security_service_store_t) <= OTA_LAYOUT_SECURITY_STORE_SIZE_BYTES,
               "Security store does not fit into the reserved flash region");

/** @brief Standard CRC-32 (poly 0xEDB88320, reflected) over @p size bytes of @p data. */
static uint32_t security_service_crc32(const uint8_t *data, size_t size)
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
static const security_service_store_t *security_service_flash_store(void)
{
    return (const security_service_store_t *)OTA_LAYOUT_SECURITY_STORE_ORIGIN;
}

/** @brief Returns the Flash store only if `security_service_init()` has succeeded; NULL otherwise. */
static const security_service_store_t *security_service_store(void)
{
    if (!g_security_store_ready)
    {
        return NULL;
    }

    return security_service_flash_store();
}

/** @brief Reads the commissioning-mode bit out of the store's `reserved0` flags field. */
static bool security_service_get_commissioning_flag(const security_service_store_t *store)
{
    return store != NULL &&
           (store->reserved0 & SECURITY_SERVICE_FLAG_COMMISSIONING_MODE) != 0u;
}

/** @brief Sets/clears the commissioning-mode bit in a local store image (no Flash write performed here). */
static void security_service_set_commissioning_flag(security_service_store_t *store, bool enabled)
{
    if (store == NULL)
    {
        return;
    }

    if (enabled)
    {
        store->reserved0 = (uint16_t)(store->reserved0 | SECURITY_SERVICE_FLAG_COMMISSIONING_MODE);
    }
    else
    {
        store->reserved0 = (uint16_t)(store->reserved0 & (uint16_t)~SECURITY_SERVICE_FLAG_COMMISSIONING_MODE);
    }
}

/** @brief Current Unix epoch second, per `time_service`'s underlying `get_system_time()`. */
static uint32_t security_service_now_epoch_s(void)
{
    return get_system_time();
}

/**
 * @brief Monotonic microseconds since boot for login-backoff arithmetic.
 *
 * Deliberately `time_us_64()`, never the wall clock (`get_system_time_us()`):
 * before SNTP the RTC is invalid and that reads a constant 0, so one failed
 * login would lock the account until the network delivered a time, and an
 * SNTP correction would shorten or stretch every running lockout.
 */
static uint64_t security_service_now_us(void)
{
    return time_us_64();
}

/** @brief Clears a login-backoff record's failure count and lockout deadline; no-op if NULL. */
static void security_service_reset_auth_runtime(security_service_auth_runtime_t *runtime)
{
    login_backoff_reset(runtime);
}

/** @brief Bumps the global failed-login counter and records the current time as the last failure. */
static void security_service_note_auth_failure(void)
{
    ++g_failed_login_count;
    g_last_auth_failure_epoch_s = security_service_now_epoch_s();
}

/** @brief Clears login-backoff state for a user slot index; silently ignores an out-of-range index. */
static void security_service_reset_user_runtime_by_index(int index)
{
    if (index < 0 || index >= (int)SECURITY_SERVICE_MAX_USERS)
    {
        return;
    }

    security_service_reset_auth_runtime(&g_user_auth_runtime[index]);
}

/** @brief Resets all per-user and unknown-username login-backoff state and failure counters; called from `security_service_init()`. */
static void security_service_reset_all_runtime_state(void)
{
    memset(g_user_auth_runtime, 0, sizeof(g_user_auth_runtime));
    security_service_reset_auth_runtime(&g_unknown_auth_runtime);
    g_failed_login_count = 0u;
    g_last_auth_failure_epoch_s = 0u;
}

void security_service_audit_log(const char *event,
                                const char *actor,
                                const char *subject,
                                bool success,
                                const char *details)
{
    printf("[AUDIT]\tepoch=%" PRIu32 " event=%s actor=%s subject=%s result=%s",
           security_service_now_epoch_s(),
           event != NULL ? event : "unknown",
           (actor != NULL && actor[0] != '\0') ? actor : "system",
           (subject != NULL && subject[0] != '\0') ? subject : "-",
           success ? "success" : "failure");

    if (details != NULL && details[0] != '\0')
    {
        printf(" details=%s", details);
    }

    printf("\n");
    fflush(stdout);

    if (g_audit_callback != NULL)
    {
        g_audit_callback(event, actor, subject, success, details, g_audit_callback_context);
    }
}

void security_service_set_audit_callback(security_service_audit_callback_t callback, void *context)
{
    g_audit_callback = callback;
    g_audit_callback_context = context;
}

/**
 * @brief Days since the civil epoch (1970-01-01) for a given Gregorian
 * calendar date, using Howard Hinnant's `days_from_civil` algorithm.
 * @note Duplicated verbatim in `security_trust_expiry_service.c` and
 * `security_identity_import_service.c`.
 * @return Negative for dates before 1970-01-01 (caller treats this as invalid).
 */
static int64_t security_service_days_from_civil(int32_t year, uint32_t month, uint32_t day)
{
    const int32_t adjusted_year = year - (month <= 2u ? 1 : 0);
    const int32_t era = (adjusted_year >= 0 ? adjusted_year : adjusted_year - 399) / 400;
    const uint32_t year_of_era = (uint32_t)(adjusted_year - era * 400);
    const uint32_t shifted_month = (month > 2u) ? (month - 3u) : (month + 9u);
    const uint32_t day_of_year = (153u * shifted_month + 2u) / 5u + day - 1u;
    const uint32_t day_of_era = year_of_era * 365u + year_of_era / 4u - year_of_era / 100u + day_of_year;

    return (int64_t)era * 146097 + (int64_t)day_of_era - 719468;
}

/**
 * @brief Converts an mbedTLS X.509 time field to a Unix epoch second.
 * @return false if any field is out of range or the resulting epoch second
 * would not fit in a `uint32_t`.
 */
static bool security_service_x509_time_to_epoch(const mbedtls_x509_time *time, uint32_t *out_epoch_s)
{
    int64_t days;
    int64_t seconds;

    if (time == NULL || out_epoch_s == NULL ||
        time->year < 1970 ||
        time->mon < 1 || time->mon > 12 ||
        time->day < 1 || time->day > 31 ||
        time->hour < 0 || time->hour > 23 ||
        time->min < 0 || time->min > 59 ||
        time->sec < 0 || time->sec > 60)
    {
        return false;
    }

    days = security_service_days_from_civil(time->year, (uint32_t)time->mon, (uint32_t)time->day);
    if (days < 0)
    {
        return false;
    }

    seconds = days * 86400 +
              (int64_t)time->hour * 3600 +
              (int64_t)time->min * 60 +
              (int64_t)time->sec;
    if (seconds < 0 || (uint64_t)seconds > UINT32_MAX)
    {
        return false;
    }

    *out_epoch_s = (uint32_t)seconds;
    return true;
}

/**
 * @brief Reads the stored device certificate and parses it into @p certificate.
 * @param certificate Initialized by this function (`mbedtls_x509_crt_init`);
 * caller must `mbedtls_x509_crt_free()` it only when this returns true.
 * @return false if no device certificate is stored or it fails to parse as
 * X.509 (in which case the mbedTLS context is freed before returning).
 */
static bool security_service_parse_device_certificate(mbedtls_x509_crt *certificate)
{
    security_service_blob_view_t view = {0};
    int rc;

    if (certificate == NULL || !security_service_get_device_certificate(&view))
    {
        return false;
    }

    mbedtls_x509_crt_init(certificate);
    rc = mbedtls_x509_crt_parse(certificate, view.data, view.length);
    if (rc != 0)
    {
        mbedtls_x509_crt_free(certificate);
        return false;
    }

    return true;
}

/**
 * @brief Computes `SHA-256(salt || password)` using the RP2350 hardware
 * SHA-256 peripheral (`pico_sha256_*`, blocking).
 * @param salt Fixed `SECURITY_SERVICE_SALT_LEN`-byte salt, hashed first.
 * @param password May be NULL/zero-length (hashes the salt alone).
 * @param out_hash Receives the `SECURITY_SERVICE_PASSWORD_HASH_LEN`-byte digest.
 * @return false if @p salt or @p out_hash is NULL, or the hardware engine
 * failed to start.
 * @note Blocks the calling task while the hardware SHA-256 engine runs.
 */
static bool security_service_hash_password(const uint8_t salt[SECURITY_SERVICE_SALT_LEN],
                                           const uint8_t *password,
                                           size_t password_length,
                                           uint8_t out_hash[SECURITY_SERVICE_PASSWORD_HASH_LEN])
{
    pico_sha256_state_t state;
    sha256_result_t result;

    if (salt == NULL || out_hash == NULL)
    {
        return false;
    }

    if (pico_sha256_start_blocking(&state, SHA256_BIG_ENDIAN, true) != PICO_OK)
    {
        return false;
    }

    pico_sha256_update_blocking(&state, salt, SECURITY_SERVICE_SALT_LEN);
    if (password != NULL && password_length > 0u)
    {
        pico_sha256_update_blocking(&state, password, password_length);
    }
    pico_sha256_finish(&state, &result);

    memcpy(out_hash, result.bytes, SECURITY_SERVICE_PASSWORD_HASH_LEN);
    return true;
}

/**
 * @brief Fills @p buffer with hardware TRNG output (RP2350's `get_rand_32()`),
 * used to generate a fresh per-user password salt.
 */
static void security_service_fill_random(uint8_t *buffer, size_t size)
{
    size_t offset = 0u;

    while (offset < size)
    {
        uint32_t value = get_rand_32();
        const size_t remaining = size - offset;
        const size_t chunk = remaining < sizeof(value) ? remaining : sizeof(value);

        memcpy(&buffer[offset], &value, chunk);
        offset += chunk;
    }
}

/** @brief Fills @p store with a zeroed, freshly-initialized security store (no users, no identity, commissioning mode on). */
static void security_service_reset_defaults(security_service_store_t *store)
{
    memset(store, 0, sizeof(*store));
    store->magic = SECURITY_SERVICE_STORE_MAGIC;
    store->version = SECURITY_SERVICE_STORE_VERSION;
    security_service_set_commissioning_flag(store, true);
}

/** @return Slot index of the user record matching @p username (case-sensitive, exact), or -1 if not found/invalid input. */
static int security_service_find_user_index(const security_service_store_t *store, const char *username)
{
    if (store == NULL || username == NULL || username[0] == '\0')
    {
        return -1;
    }

    for (size_t i = 0u; i < SECURITY_SERVICE_MAX_USERS; ++i)
    {
        if (store->users[i].in_use != 0u && strcmp(store->users[i].username, username) == 0)
        {
            return (int)i;
        }
    }

    return -1;
}

/** @return Index of the first unused (`in_use == 0`) user slot, or -1 if the table is full/NULL. */
static int security_service_find_free_user_index(const security_service_store_t *store)
{
    if (store == NULL)
    {
        return -1;
    }

    for (size_t i = 0u; i < SECURITY_SERVICE_MAX_USERS; ++i)
    {
        if (store->users[i].in_use == 0u)
        {
            return (int)i;
        }
    }

    return -1;
}

/**
 * @brief Counts in-use, enabled user records with role `SECURITY_ROLE_ADMIN`.
 * Used to enforce the "at least one enabled admin must remain" rule in
 * `set_role`/`disable_user`/`delete_user`.
 */
static size_t security_service_count_enabled_admins(const security_service_store_t *store)
{
    size_t count = 0u;

    if (store == NULL)
    {
        return 0u;
    }

    for (size_t i = 0u; i < SECURITY_SERVICE_MAX_USERS; ++i)
    {
        if (store->users[i].in_use != 0u &&
            store->users[i].enabled != 0u &&
            store->users[i].role == (uint8_t)SECURITY_ROLE_ADMIN)
        {
            ++count;
        }
    }

    return count;
}

/** @return false if @p username is NULL, empty, or not shorter than `SECURITY_SERVICE_USERNAME_MAX_LEN`. */
static bool security_service_username_is_valid(const char *username)
{
    size_t length;

    if (username == NULL)
    {
        return false;
    }

    length = strlen(username);
    if (length == 0u || length >= SECURITY_SERVICE_USERNAME_MAX_LEN)
    {
        return false;
    }

    return true;
}

/** @return false if @p password is NULL, empty, or longer than 128 bytes. */
static bool security_service_password_is_valid(const uint8_t *password, size_t password_length)
{
    return password != NULL && password_length > 0u && password_length <= 128u;
}

/**
 * @brief Generates a fresh random salt and stores the salted SHA-256 hash
 * of @p password into @p record, overwriting any prior salt/hash.
 * @return false if @p record is NULL or @p password is invalid (see
 * `security_service_password_is_valid()`).
 */
static bool security_service_set_password_record(security_service_user_record_t *record,
                                                 const uint8_t *password,
                                                 size_t password_length)
{
    if (record == NULL || !security_service_password_is_valid(password, password_length))
    {
        return false;
    }

    security_service_fill_random(record->salt, sizeof(record->salt));
    return security_service_hash_password(record->salt,
                                          password,
                                          password_length,
                                          record->password_hash);
}

/**
 * @brief Verifies @p password against @p record's stored salt/hash using a
 * constant-time comparison (`diff |=` accumulator, not short-circuited).
 * @return false if @p record is NULL, not in use, disabled, or the password
 * is invalid/does not match.
 */
static bool security_service_verify_password_record(const security_service_user_record_t *record,
                                                    const uint8_t *password,
                                                    size_t password_length)
{
    uint8_t candidate_hash[SECURITY_SERVICE_PASSWORD_HASH_LEN];
    uint8_t diff = 0u;

    if (record == NULL || record->in_use == 0u || record->enabled == 0u)
    {
        return false;
    }

    if (!security_service_password_is_valid(password, password_length))
    {
        return false;
    }

    if (!security_service_hash_password(record->salt, password, password_length, candidate_hash))
    {
        return false;
    }

    for (size_t i = 0u; i < sizeof(candidate_hash); ++i)
    {
        diff |= (uint8_t)(candidate_hash[i] ^ record->password_hash[i]);
    }

    return diff == 0u;
}

/**
 * @brief Validates a candidate store: magic, version, per-field length
 * bounds (certificate/key/trust-list lengths within their `*_MAX_LEN`
 * caps), and CRC32 — the sole gate for trusting Flash contents (or a
 * heap-staged image before it is committed).
 * @note `expected_crc` is computed from @p store before the NULL check
 * below runs; this only avoids a NULL dereference because every current
 * caller passes a non-NULL pointer (the fixed XIP Flash address or a
 * heap-allocated image) — passing a genuinely NULL @p store would crash in
 * `security_service_crc32()` before reaching this function's own guard.
 */
static bool security_service_store_is_valid(const security_service_store_t *store)
{
    const uint32_t expected_crc = security_service_crc32((const uint8_t *)store,
                                                         offsetof(security_service_store_t, crc32));

    if (store == NULL)
    {
        return false;
    }

    if (store->magic != SECURITY_SERVICE_STORE_MAGIC || store->version != SECURITY_SERVICE_STORE_VERSION)
    {
        return false;
    }

    if (store->local_certificate_length > SECURITY_SERVICE_CERT_MAX_LEN ||
        store->private_key_length > SECURITY_SERVICE_KEY_MAX_LEN ||
        store->trust_list_length > SECURITY_SERVICE_TRUST_LIST_MAX_LEN)
    {
        return false;
    }

    return expected_crc == store->crc32;
}

/**
 * @brief Heap-allocates a full `security_service_store_t`-sized staging
 * image, either copied from @p source or (if NULL) freshly defaulted via
 * `security_service_reset_defaults()`.
 * @return NULL on allocation failure. Caller owns and must `vPortFree()`
 * the result (typically after `security_service_commit_store()`).
 */
static security_service_store_t *security_service_allocate_image(const security_service_store_t *source)
{
    security_service_store_t *image =
        (security_service_store_t *)pvPortMalloc(sizeof(security_service_store_t));

    if (image == NULL)
    {
        return NULL;
    }

    if (source != NULL)
    {
        memcpy(image, source, sizeof(*image));
    }
    else
    {
        security_service_reset_defaults(image);
    }

    return image;
}

/** @brief Rounds @p image_size up to a whole number of Flash program-page units. */
static size_t security_service_get_program_size(size_t image_size)
{
    return ((image_size + OTA_LAYOUT_FLASH_PROGRAM_SIZE_BYTES - 1u) /
            OTA_LAYOUT_FLASH_PROGRAM_SIZE_BYTES) * OTA_LAYOUT_FLASH_PROGRAM_SIZE_BYTES;
}

/**
 * @brief Stamps magic/version/CRC32 onto @p image, then erases and
 * reprograms the entire `OTA_LAYOUT_SECURITY_STORE_SIZE_BYTES` Flash region
 * with it via `flash_operation_coordinator`.
 *
 * @note Two-phase: erase all sectors first, then program all pages; a
 * failure partway through either phase leaves the region in an
 * indeterminate state — no rollback is attempted. On success, sets
 * `g_security_store_ready = true`.
 * @return false on NULL input, scratch-buffer allocation failure, or any
 * individual erase/program operation failure.
 */
static bool security_service_commit_store(security_service_store_t *image)
{
    uint8_t *program_image = NULL;
    const size_t program_size = security_service_get_program_size(sizeof(*image));
    bool committed = false;

    if (image == NULL)
    {
        return false;
    }

    image->magic = SECURITY_SERVICE_STORE_MAGIC;
    image->version = SECURITY_SERVICE_STORE_VERSION;
    image->crc32 = security_service_crc32((const uint8_t *)image,
                                          offsetof(security_service_store_t, crc32));

    program_image = (uint8_t *)pvPortMalloc(program_size);
    if (program_image == NULL)
    {
        return false;
    }

    memset(program_image, 0xFF, program_size);
    memcpy(program_image, image, sizeof(*image));

    for (uint32_t offset = 0u; offset < OTA_LAYOUT_SECURITY_STORE_SIZE_BYTES;
         offset += OTA_LAYOUT_FLASH_ERASE_SIZE_BYTES)
    {
        if (flash_operation_erase_sector(OTA_LAYOUT_SECURITY_STORE_OFFSET_BYTES + offset) != FLASH_OPERATION_OK)
        {
            goto cleanup;
        }
    }
    for (size_t offset = 0u; offset < program_size; offset += OTA_LAYOUT_FLASH_PROGRAM_SIZE_BYTES)
    {
        if (flash_operation_program_page(OTA_LAYOUT_SECURITY_STORE_OFFSET_BYTES + (uint32_t)offset,
                                         program_image + offset) != FLASH_OPERATION_OK)
        {
            goto cleanup;
        }
    }

    g_security_store_ready = true;
    committed = true;

cleanup:
    vPortFree(program_image);
    return committed;
}

/**
 * @brief Re-derives and commits `security_state_service`'s snapshot from a
 * `security_service` store image, called after every store mutation
 * (init, identity seed/update, trust-list update, commissioning-mode
 * change).
 *
 * Commissioning mode is forced true if @p store is NULL or has an empty
 * trust list (mirrors `security_service_is_commissioning_mode()`'s own
 * fallback rule). The active trust-generation counter is set to the
 * trust-list version only when a trust list is actually present (0
 * otherwise), and the pending trust-generation counter is always reset to 0
 * here — this function does not know about an in-flight
 * `security_trust_import_service` session's pending generation.
 *
 * @return false if any of the underlying `security_state_service_*` calls fail.
 * @note Performs Flash writes in `security_state_service` (its own reserved
 * region, separate from this module's store).
 */
static bool security_service_sync_state_store(const security_service_store_t *store)
{
    const bool commissioning_mode = store == NULL ||
                                    store->trust_list_length == 0u ||
                                    security_service_get_commissioning_flag(store);
    const uint32_t trust_list_version = (store != NULL) ? store->trust_list_version : 0u;
    const uint32_t active_trust_generation =
        (store != NULL && store->trust_list_length > 0u) ? store->trust_list_version : 0u;

    if (!security_state_service_init(commissioning_mode, trust_list_version))
    {
        return false;
    }

    if (!security_state_service_set_trust_list_version(trust_list_version))
    {
        return false;
    }

    if (!security_state_service_set_trust_generations(active_trust_generation, 0u))
    {
        return false;
    }

    return security_state_service_set_commissioning_mode(commissioning_mode);
}

/**
 * @brief Overwrites (or creates, in the first free slot) an `admin` user
 * record in @p store with the bootstrap default password, resetting its
 * role to `SECURITY_ROLE_ADMIN` and enabling it.
 *
 * Called from `security_service_init()` when the existing store has no
 * enabled admin account (fresh store or all admins disabled/deleted) —
 * this is the mechanism that guarantees the device is never left without
 * any way to administratively log in.
 *
 * @return false only if no free slot is available (table full with a
 * differently-named non-admin user occupying every slot) or password
 * hashing fails.
 * @note Does not commit to Flash; caller is responsible for calling
 * `security_service_commit_store()`.
 */
static bool security_service_ensure_admin_account(security_service_store_t *store)
{
    security_service_user_record_t *record;
    const int existing_index = security_service_find_user_index(store, SECURITY_SERVICE_DEFAULT_ADMIN_USER);
    int index = existing_index;

    if (index < 0)
    {
        index = security_service_find_free_user_index(store);
        if (index < 0)
        {
            return false;
        }
    }

    record = &store->users[index];
    memset(record, 0, sizeof(*record));
    record->in_use = 1u;
    record->enabled = 1u;
    record->role = (uint8_t)SECURITY_ROLE_ADMIN;
    strcpy(record->username, SECURITY_SERVICE_DEFAULT_ADMIN_USER);

    return security_service_set_password_record(record,
                                                (const uint8_t *)SECURITY_SERVICE_DEFAULT_ADMIN_PASSWORD,
                                                strlen(SECURITY_SERVICE_DEFAULT_ADMIN_PASSWORD));
}

bool security_service_init(void)
{
    const security_service_store_t *flash_store = security_service_flash_store();
    security_service_store_t *image = NULL;

    security_service_reset_all_runtime_state();

    if (security_service_store_is_valid(flash_store))
    {
        g_security_store_ready = true;
        if (security_service_count_enabled_admins(flash_store) > 0u)
        {
            return security_service_sync_state_store(flash_store);
        }

        image = security_service_allocate_image(flash_store);
    }
    else
    {
        image = security_service_allocate_image(NULL);
    }

    if (image == NULL)
    {
        return false;
    }

    if (!security_service_ensure_admin_account(image))
    {
        vPortFree(image);
        return false;
    }

    printf("[SECURITY]\tInitialized default security store with bootstrap admin user\n");
    fflush(stdout);

    if (!security_service_commit_store(image))
    {
        vPortFree(image);
        return false;
    }

    if (!security_service_sync_state_store(image))
    {
        vPortFree(image);
        return false;
    }

    security_service_audit_log("certificate-update",
                               "system",
                               "device-identity",
                               true,
                               "seeded missing local certificate/private key");
    vPortFree(image);
    return true;
}

bool security_service_has_device_identity(void)
{
    const security_service_store_t *store = security_service_store();

    return store != NULL &&
           store->local_certificate_length > 0u &&
           store->private_key_length > 0u;
}

bool security_service_seed_device_identity_if_missing(const uint8_t *certificate,
                                                      size_t certificate_length,
                                                      const uint8_t *private_key,
                                                      size_t private_key_length)
{
    const security_service_store_t *store = security_service_store();
    security_service_store_t *image = NULL;

    if (security_service_has_device_identity())
    {
        return true;
    }

    if (store == NULL ||
        certificate == NULL || private_key == NULL ||
        certificate_length == 0u || certificate_length > SECURITY_SERVICE_CERT_MAX_LEN ||
        private_key_length == 0u || private_key_length > SECURITY_SERVICE_KEY_MAX_LEN)
    {
        return false;
    }

    image = security_service_allocate_image(store);
    if (image == NULL)
    {
        return false;
    }

    memset(image->local_certificate, 0, sizeof(image->local_certificate));
    memset(image->private_key, 0, sizeof(image->private_key));
    memcpy(image->local_certificate, certificate, certificate_length);
    memcpy(image->private_key, private_key, private_key_length);
    image->local_certificate_length = (uint32_t)certificate_length;
    image->private_key_length = (uint32_t)private_key_length;

    if (!security_service_commit_store(image))
    {
        vPortFree(image);
        return false;
    }

    vPortFree(image);
    return true;
}

bool security_service_set_device_identity(const uint8_t *certificate,
                                          size_t certificate_length,
                                          const uint8_t *private_key,
                                          size_t private_key_length)
{
    const security_service_store_t *store = security_service_store();
    security_service_store_t *image = NULL;

    if (store == NULL ||
        certificate == NULL || private_key == NULL ||
        certificate_length == 0u || certificate_length > SECURITY_SERVICE_CERT_MAX_LEN ||
        private_key_length == 0u || private_key_length > SECURITY_SERVICE_KEY_MAX_LEN)
    {
        return false;
    }

    image = security_service_allocate_image(store);
    if (image == NULL)
    {
        return false;
    }

    memset(image->local_certificate, 0, sizeof(image->local_certificate));
    memset(image->private_key, 0, sizeof(image->private_key));
    memcpy(image->local_certificate, certificate, certificate_length);
    memcpy(image->private_key, private_key, private_key_length);
    image->local_certificate_length = (uint32_t)certificate_length;
    image->private_key_length = (uint32_t)private_key_length;

    if (!security_service_commit_store(image))
    {
        vPortFree(image);
        return false;
    }

    security_service_audit_log("certificate-update",
                               "system",
                               "device-identity",
                               true,
                               "local certificate/private key updated");
    vPortFree(image);
    return true;
}

bool security_service_get_device_certificate(security_service_blob_view_t *out_view)
{
    const security_service_store_t *store = security_service_store();

    if (out_view == NULL || store == NULL || store->local_certificate_length == 0u)
    {
        return false;
    }

    out_view->data = store->local_certificate;
    out_view->length = store->local_certificate_length;
    return true;
}

bool security_service_get_device_private_key(security_service_blob_view_t *out_view)
{
    const security_service_store_t *store = security_service_store();

    if (out_view == NULL || store == NULL || store->private_key_length == 0u)
    {
        return false;
    }

    out_view->data = store->private_key;
    out_view->length = store->private_key_length;
    return true;
}

bool security_service_get_trust_list(security_service_blob_view_t *out_view)
{
    const security_service_store_t *store = security_service_store();

    if (out_view == NULL || store == NULL)
    {
        return false;
    }

    out_view->data = store->trust_list;
    out_view->length = store->trust_list_length;
    return store->trust_list_length > 0u;
}

bool security_service_set_trust_list(const uint8_t *trust_list, size_t trust_list_length)
{
    const security_service_store_t *store = security_service_store();
    security_service_store_t *image = NULL;
    const bool clearing = (trust_list == NULL || trust_list_length == 0u);
    const bool unchanged =
        store != NULL &&
        store->trust_list_length == trust_list_length &&
        ((trust_list_length == 0u) ||
         (trust_list != NULL && memcmp(store->trust_list, trust_list, trust_list_length) == 0));

    if (store == NULL || trust_list_length > SECURITY_SERVICE_TRUST_LIST_MAX_LEN)
    {
        return false;
    }

    if (unchanged)
    {
        return true;
    }

    image = security_service_allocate_image(store);
    if (image == NULL)
    {
        return false;
    }

    memset(image->trust_list, 0, sizeof(image->trust_list));
    image->trust_list_length = (uint32_t)trust_list_length;
    image->trust_list_version = image->trust_list_version + 1u;

    if (!clearing)
    {
        memcpy(image->trust_list, trust_list, trust_list_length);
    }
    else
    {
        security_service_set_commissioning_flag(image, true);
    }

    if (!security_service_commit_store(image))
    {
        vPortFree(image);
        return false;
    }

    if (!security_service_sync_state_store(image))
    {
        vPortFree(image);
        return false;
    }

    security_service_audit_log("trust-list-update",
                               "system",
                               "trust-list",
                               true,
                               clearing ? "trust list cleared" : "trust list updated");
    vPortFree(image);
    return true;
}

bool security_service_get_trust_list_version(uint32_t *out_version)
{
    const security_service_store_t *store = security_service_store();

    if (out_version == NULL)
    {
        return false;
    }

    if (security_state_service_get_trust_list_version(out_version))
    {
        return true;
    }

    if (store == NULL)
    {
        return false;
    }

    *out_version = store->trust_list_version;
    return true;
}

bool security_service_is_commissioning_mode(void)
{
    const security_service_store_t *store = security_service_store();
    bool commissioning_mode = true;

    if (security_state_service_get_commissioning_mode(&commissioning_mode))
    {
        return commissioning_mode;
    }

    if (store == NULL)
    {
        return true;
    }

    if (store->trust_list_length == 0u)
    {
        return true;
    }

    return security_service_get_commissioning_flag(store);
}

bool security_service_set_commissioning_mode(bool enabled)
{
    const security_service_store_t *store = security_service_store();
    security_service_store_t *image = NULL;

    if (store == NULL)
    {
        return false;
    }

    if (!enabled && store->trust_list_length == 0u)
    {
        return false;
    }

    if (security_service_is_commissioning_mode() == enabled)
    {
        return true;
    }

    image = security_service_allocate_image(store);
    if (image == NULL)
    {
        return false;
    }

    security_service_set_commissioning_flag(image, enabled);
    if (!security_service_commit_store(image))
    {
        vPortFree(image);
        return false;
    }

    if (!security_service_sync_state_store(image))
    {
        vPortFree(image);
        return false;
    }

    vPortFree(image);
    return true;
}

bool security_service_get_last_auth_failure(uint32_t *out_epoch_s)
{
    if (out_epoch_s == NULL || g_last_auth_failure_epoch_s == 0u)
    {
        return false;
    }

    *out_epoch_s = g_last_auth_failure_epoch_s;
    return true;
}

uint32_t security_service_get_failed_login_count(void)
{
    return g_failed_login_count;
}

uint32_t security_service_get_trusted_client_count(void)
{
    security_service_blob_view_t trust_list = {0};
    mbedtls_x509_crt certificates;
    uint32_t count = 0u;

    if (!security_service_get_trust_list(&trust_list) || trust_list.length == 0u)
    {
        return 0u;
    }

    mbedtls_x509_crt_init(&certificates);
    if (mbedtls_x509_crt_parse(&certificates, trust_list.data, trust_list.length) == 0)
    {
        for (const mbedtls_x509_crt *cursor = &certificates; cursor != NULL; cursor = cursor->next)
        {
            if (cursor->raw.len > 0u)
            {
                ++count;
            }
        }
    }
    mbedtls_x509_crt_free(&certificates);

    if (count == 0u)
    {
        return 1u;
    }

    return count;
}

bool security_service_get_device_certificate_thumbprint(char *buffer, size_t buffer_size)
{
    security_service_blob_view_t certificate = {0};
    unsigned char digest[20];
    static const char hex[] = "0123456789ABCDEF";

    if (buffer == NULL || buffer_size < (SECURITY_SERVICE_CERT_THUMBPRINT_LEN + 1u) ||
        !security_service_get_device_certificate(&certificate) || certificate.length == 0u)
    {
        return false;
    }

    if (mbedtls_sha1(certificate.data, certificate.length, digest) != 0)
    {
        return false;
    }

    for (size_t i = 0u; i < sizeof(digest); ++i)
    {
        buffer[i * 2u] = hex[(digest[i] >> 4u) & 0x0Fu];
        buffer[i * 2u + 1u] = hex[digest[i] & 0x0Fu];
    }
    buffer[SECURITY_SERVICE_CERT_THUMBPRINT_LEN] = '\0';
    return true;
}

bool security_service_get_device_certificate_not_before(uint32_t *out_epoch_s)
{
    mbedtls_x509_crt certificate;
    bool ok = false;

    if (out_epoch_s == NULL)
    {
        return false;
    }

    if (!security_service_parse_device_certificate(&certificate))
    {
        return false;
    }

    ok = security_service_x509_time_to_epoch(&certificate.valid_from, out_epoch_s);
    mbedtls_x509_crt_free(&certificate);
    return ok;
}

bool security_service_get_device_certificate_not_after(uint32_t *out_epoch_s)
{
    mbedtls_x509_crt certificate;
    bool ok = false;

    if (out_epoch_s == NULL)
    {
        return false;
    }

    if (!security_service_parse_device_certificate(&certificate))
    {
        return false;
    }

    ok = security_service_x509_time_to_epoch(&certificate.valid_to, out_epoch_s);
    mbedtls_x509_crt_free(&certificate);
    return ok;
}

bool security_service_verify_password(const char *username,
                                      const uint8_t *password,
                                      size_t password_length)
{
    const security_service_store_t *store = security_service_store();
    int index;

    if (store == NULL)
    {
        return false;
    }

    index = security_service_find_user_index(store, username);
    if (index < 0)
    {
        return false;
    }

    return security_service_verify_password_record(&store->users[index],
                                                   password,
                                                   password_length);
}

security_auth_result_t security_service_authenticate_user(const char *username,
                                                          const uint8_t *password,
                                                          size_t password_length,
                                                          security_user_role_t *out_role,
                                                          uint32_t *out_retry_after_ms)
{
    const security_service_store_t *store = security_service_store();
    const uint64_t now_us = security_service_now_us();
    security_service_auth_runtime_t *runtime = &g_unknown_auth_runtime;
    uint32_t retry_after_ms = 0u;
    int index;

    if (out_role != NULL)
    {
        *out_role = SECURITY_ROLE_VIEWER;
    }

    if (out_retry_after_ms != NULL)
    {
        *out_retry_after_ms = 0u;
    }

    if (store == NULL || username == NULL || username[0] == '\0')
    {
        return SECURITY_AUTH_RESULT_INVALID_CREDENTIALS;
    }

    index = security_service_find_user_index(store, username);
    if (index >= 0)
    {
        runtime = &g_user_auth_runtime[index];
    }

    retry_after_ms = login_backoff_remaining_ms(runtime, now_us);
    if (retry_after_ms > 0u)
    {
        char details[64];

        if (out_retry_after_ms != NULL)
        {
            *out_retry_after_ms = retry_after_ms;
        }

        snprintf(details, sizeof(details), "locked retry_after_ms=%" PRIu32, retry_after_ms);
        security_service_note_auth_failure();
        security_service_audit_log("login", username, "session", false, details);
        return SECURITY_AUTH_RESULT_LOCKED_OUT;
    }

    if (index < 0 ||
        !security_service_verify_password_record(&store->users[index], password, password_length))
    {
        uint32_t backoff_ms;
        char details[64];

        backoff_ms = login_backoff_record_failure(runtime, now_us);

        if (out_retry_after_ms != NULL)
        {
            *out_retry_after_ms = backoff_ms;
        }

        snprintf(details, sizeof(details), "invalid-credentials retry_after_ms=%" PRIu32, backoff_ms);
        security_service_note_auth_failure();
        security_service_audit_log("login", username, "session", false, details);
        return SECURITY_AUTH_RESULT_INVALID_CREDENTIALS;
    }

    security_service_reset_auth_runtime(runtime);
    if (out_role != NULL)
    {
        *out_role = (security_user_role_t)store->users[index].role;
    }

    security_service_audit_log("login",
                               username,
                               "session",
                               true,
                               security_service_role_to_string((security_user_role_t)store->users[index].role));
    return SECURITY_AUTH_RESULT_SUCCESS;
}

bool security_service_get_role(const char *username, security_user_role_t *out_role)
{
    const security_service_store_t *store = security_service_store();
    int index;

    if (store == NULL || out_role == NULL)
    {
        return false;
    }

    index = security_service_find_user_index(store, username);
    if (index < 0 || store->users[index].enabled == 0u)
    {
        return false;
    }

    *out_role = (security_user_role_t)store->users[index].role;
    return true;
}

bool security_service_add_user(const char *username,
                               const uint8_t *password,
                               size_t password_length,
                               security_user_role_t role)
{
    const security_service_store_t *store = security_service_store();
    security_service_store_t *image = NULL;
    security_service_user_record_t *record = NULL;
    int index;

    if (store == NULL ||
        !security_service_username_is_valid(username) ||
        !security_service_password_is_valid(password, password_length) ||
        role < SECURITY_ROLE_VIEWER || role > SECURITY_ROLE_ADMIN)
    {
        return false;
    }

    index = security_service_find_free_user_index(store);
    if (security_service_find_user_index(store, username) >= 0 || index < 0)
    {
        return false;
    }

    image = security_service_allocate_image(store);
    if (image == NULL)
    {
        return false;
    }

    record = &image->users[index];
    memset(record, 0, sizeof(*record));
    record->in_use = 1u;
    record->enabled = 1u;
    record->role = (uint8_t)role;
    strcpy(record->username, username);

    if (!security_service_set_password_record(record, password, password_length) ||
        !security_service_commit_store(image))
    {
        vPortFree(image);
        return false;
    }

    security_service_reset_user_runtime_by_index(index);
    vPortFree(image);
    return true;
}

bool security_service_change_password(const char *username,
                                      const uint8_t *old_password,
                                      size_t old_password_length,
                                      const uint8_t *new_password,
                                      size_t new_password_length)
{
    const security_service_store_t *store = security_service_store();
    security_service_store_t *image = NULL;
    int index;

    if (store == NULL || !security_service_password_is_valid(new_password, new_password_length))
    {
        return false;
    }

    index = security_service_find_user_index(store, username);
    if (index < 0)
    {
        return false;
    }

    if (old_password != NULL && old_password_length > 0u &&
        !security_service_verify_password_record(&store->users[index],
                                                 old_password,
                                                 old_password_length))
    {
        return false;
    }

    image = security_service_allocate_image(store);
    if (image == NULL)
    {
        return false;
    }

    if (!security_service_set_password_record(&image->users[index], new_password, new_password_length) ||
        !security_service_commit_store(image))
    {
        vPortFree(image);
        return false;
    }

    security_service_reset_user_runtime_by_index(index);
    vPortFree(image);
    return true;
}

bool security_service_set_role(const char *username, security_user_role_t role)
{
    const security_service_store_t *store = security_service_store();
    security_service_store_t *image = NULL;
    int index;
    const security_service_user_record_t *current_record;

    if (store == NULL || role < SECURITY_ROLE_VIEWER || role > SECURITY_ROLE_ADMIN)
    {
        return false;
    }

    index = security_service_find_user_index(store, username);
    if (index < 0)
    {
        return false;
    }

    current_record = &store->users[index];
    if (current_record->enabled == 0u)
    {
        return false;
    }

    if (current_record->role == (uint8_t)SECURITY_ROLE_ADMIN &&
        role != SECURITY_ROLE_ADMIN &&
        security_service_count_enabled_admins(store) <= 1u)
    {
        return false;
    }

    image = security_service_allocate_image(store);
    if (image == NULL)
    {
        return false;
    }

    image->users[index].role = (uint8_t)role;
    if (!security_service_commit_store(image))
    {
        vPortFree(image);
        return false;
    }

    security_service_reset_user_runtime_by_index(index);
    vPortFree(image);
    return true;
}

bool security_service_disable_user(const char *username)
{
    const security_service_store_t *store = security_service_store();
    security_service_store_t *image = NULL;
    int index;
    const security_service_user_record_t *current_record;

    if (store == NULL)
    {
        return false;
    }

    index = security_service_find_user_index(store, username);
    if (index < 0)
    {
        return false;
    }

    current_record = &store->users[index];
    if (current_record->enabled == 0u)
    {
        return true;
    }

    if (current_record->role == (uint8_t)SECURITY_ROLE_ADMIN &&
        security_service_count_enabled_admins(store) <= 1u)
    {
        return false;
    }

    image = security_service_allocate_image(store);
    if (image == NULL)
    {
        return false;
    }

    image->users[index].enabled = 0u;
    if (!security_service_commit_store(image))
    {
        vPortFree(image);
        return false;
    }

    security_service_reset_user_runtime_by_index(index);
    vPortFree(image);
    return true;
}

bool security_service_delete_user(const char *username)
{
    const security_service_store_t *store = security_service_store();
    security_service_store_t *image = NULL;
    int index;
    const security_service_user_record_t *current_record;

    if (store == NULL)
    {
        return false;
    }

    index = security_service_find_user_index(store, username);
    if (index < 0)
    {
        return false;
    }

    current_record = &store->users[index];
    if (current_record->role == (uint8_t)SECURITY_ROLE_ADMIN &&
        current_record->enabled != 0u &&
        security_service_count_enabled_admins(store) <= 1u)
    {
        return false;
    }

    image = security_service_allocate_image(store);
    if (image == NULL)
    {
        return false;
    }

    memset(&image->users[index], 0, sizeof(image->users[index]));
    if (!security_service_commit_store(image))
    {
        vPortFree(image);
        return false;
    }

    security_service_reset_user_runtime_by_index(index);
    vPortFree(image);
    return true;
}

bool security_service_role_from_string(const char *text, security_user_role_t *out_role)
{
    if (text == NULL || out_role == NULL)
    {
        return false;
    }

    if (strcmp(text, "Viewer") == 0)
    {
        *out_role = SECURITY_ROLE_VIEWER;
        return true;
    }

    if (strcmp(text, "Operator") == 0)
    {
        *out_role = SECURITY_ROLE_OPERATOR;
        return true;
    }

    if (strcmp(text, "Service") == 0)
    {
        *out_role = SECURITY_ROLE_SERVICE;
        return true;
    }

    if (strcmp(text, "Admin") == 0)
    {
        *out_role = SECURITY_ROLE_ADMIN;
        return true;
    }

    return false;
}

const char *security_service_role_to_string(security_user_role_t role)
{
    switch (role)
    {
    case SECURITY_ROLE_VIEWER:
        return "Viewer";
    case SECURITY_ROLE_OPERATOR:
        return "Operator";
    case SECURITY_ROLE_SERVICE:
        return "Service";
    case SECURITY_ROLE_ADMIN:
        return "Admin";
    default:
        return "Unknown";
    }
}
