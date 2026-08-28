/**
 * @file
 * @brief Central OPC UA security coordinator: device identity, trust list,
 * local user accounts and commissioning mode.
 *
 * Responsibilities:
 * - Owns the single Flash-resident `security_service_store_t` record
 *   (device certificate/private key, trust list bytes, up to
 *   `SECURITY_SERVICE_MAX_USERS` user accounts) at
 *   `OTA_LAYOUT_SECURITY_STORE_ORIGIN`.
 * - Bootstraps a default `admin`/`admin` account on first boot or if no
 *   enabled admin account survives (see `security_service.c:645`).
 * - Verifies/authenticates local users with salted SHA-256 password hashes
 *   and applies exponential login backoff per username (and for unknown
 *   usernames, to avoid leaking which usernames exist).
 * - Mirrors commissioning-mode / trust-list-version / generation counters
 *   into `security_state_service` (`security_service.c:591`) so that other
 *   security modules do not need to parse this store directly.
 * - Emits an audit trail (`security_service_audit_log`) for login,
 *   certificate and trust-list mutations.
 *
 * Ownership: this module owns the on-Flash security store and all runtime
 * login-throttling state. `security_state_service` owns the smaller,
 * independently-committed runtime/commissioning snapshot that this module
 * keeps in sync after every store mutation.
 *
 * Concurrency: no internal locking. All public functions are expected to
 * run from a single task context (`InitTask` at boot via
 * `security_service_init()`, then whichever task services OPC UA security
 * callbacks/config-service commands). Flash writes go through
 * `flash_operation_coordinator`, which itself serializes concurrent
 * Flash access; this module does not itself guard against concurrent
 * callers mutating the store.
 */
#ifndef SERVICES_SECURITY_SERVICE_H
#define SERVICES_SECURITY_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SECURITY_SERVICE_MAX_USERS 8u
#define SECURITY_SERVICE_USERNAME_MAX_LEN 32u
#define SECURITY_SERVICE_SALT_LEN 16u
#define SECURITY_SERVICE_PASSWORD_HASH_LEN 32u
#define SECURITY_SERVICE_CERT_MAX_LEN 3072u
#define SECURITY_SERVICE_KEY_MAX_LEN 3072u
#define SECURITY_SERVICE_TRUST_LIST_MAX_LEN 2048u
#define SECURITY_SERVICE_CERT_THUMBPRINT_LEN 40u

/** Local-user role, persisted per user record and returned by authentication. */
typedef enum
{
    SECURITY_ROLE_VIEWER = 0,
    SECURITY_ROLE_OPERATOR = 1,
    SECURITY_ROLE_SERVICE = 2,
    SECURITY_ROLE_ADMIN = 3
} security_user_role_t;

/** Outcome of `security_service_authenticate_user()`. */
typedef enum
{
    SECURITY_AUTH_RESULT_SUCCESS = 0,
    SECURITY_AUTH_RESULT_INVALID_CREDENTIALS = 1,
    /** Username (or the unknown-username bucket) is under login backoff; see @ref out_retry_after_ms. */
    SECURITY_AUTH_RESULT_LOCKED_OUT = 2
} security_auth_result_t;

/**
 * @brief Non-owning view into a byte blob stored inside the Flash-resident
 * security store (device certificate, private key, or trust list).
 *
 * @note `data` points directly into the XIP-mapped Flash store returned by
 * `security_service_store()`; it is only valid until the next Flash commit
 * (`security_service_commit_store()`), which may relocate/replace the
 * underlying image. Do not retain the pointer across a mutating call.
 */
typedef struct
{
    const uint8_t *data;
    size_t length;
} security_service_blob_view_t;

/**
 * @brief Audit-event sink registered via `security_service_set_audit_callback()`.
 *
 * Invoked synchronously, in the caller's task context, from
 * `security_service_audit_log()` after the event has already been printed
 * to stdout. Must not block indefinitely or perform long-running Flash/OPC UA
 * work, since it runs inline with whatever security operation triggered it.
 *
 * @param event Short machine-readable event name (e.g. "login", "trust-list-update").
 * @param actor Username or "system" performing the action.
 * @param subject Object of the action (e.g. "session", "device-identity", "trust-list").
 * @param success Whether the action succeeded.
 * @param details Optional free-form detail string, may be NULL/empty.
 * @param context Opaque pointer supplied at registration time.
 */
typedef void (*security_service_audit_callback_t)(const char *event,
                                                  const char *actor,
                                                  const char *subject,
                                                  bool success,
                                                  const char *details,
                                                  void *context);

/**
 * @brief Loads or bootstraps the on-Flash security store and syncs
 * `security_state_service`.
 *
 * If the store at `OTA_LAYOUT_SECURITY_STORE_ORIGIN` is valid (magic/version/
 * CRC32) and has at least one enabled admin account, it is adopted as-is.
 * Otherwise a fresh store is created (or the invalid store is best-effort
 * repaired) with a bootstrap `admin`/`admin` account and committed to Flash.
 * Always re-derives the `security_state_service` snapshot from the adopted
 * store afterward.
 *
 * @return true if a valid store is in place and state sync succeeded; false
 * on Flash commit failure or out-of-memory while building the bootstrap image.
 *
 * @pre Must be called once, before any other `security_service_*` function,
 * typically from `InitTask` (see `app/basic_profile/main.c:491`).
 * @note Performs Flash erase/program via `flash_operation_coordinator` when
 * (re)seeding the store.
 */
bool security_service_init(void);

/** @return true if a non-empty device certificate and private key are stored. */
bool security_service_has_device_identity(void);
/**
 * @brief Stores a device certificate/private key pair only if no identity is
 * currently present; a no-op (success) if one already exists.
 *
 * @param certificate DER/PEM certificate bytes, `1..SECURITY_SERVICE_CERT_MAX_LEN`.
 * @param certificate_length Length of @p certificate in bytes.
 * @param private_key DER/PEM private key bytes, `1..SECURITY_SERVICE_KEY_MAX_LEN`.
 * @param private_key_length Length of @p private_key in bytes.
 * @return true if an identity is now present (either pre-existing or freshly
 * seeded); false on invalid arguments or Flash commit failure.
 * @note Commits a full store image to Flash on the seeding path.
 */
bool security_service_seed_device_identity_if_missing(const uint8_t *certificate,
                                                      size_t certificate_length,
                                                      const uint8_t *private_key,
                                                      size_t private_key_length);
/**
 * @brief Unconditionally overwrites the stored device certificate/private key.
 *
 * @param certificate DER/PEM certificate bytes, `1..SECURITY_SERVICE_CERT_MAX_LEN`.
 * @param certificate_length Length of @p certificate in bytes.
 * @param private_key DER/PEM private key bytes, `1..SECURITY_SERVICE_KEY_MAX_LEN`.
 * @param private_key_length Length of @p private_key in bytes.
 * @return true on success; false on invalid arguments or Flash commit failure.
 * @note Emits a "certificate-update" audit log entry on success. Commits a
 * full store image to Flash (erase-then-program).
 */
bool security_service_set_device_identity(const uint8_t *certificate,
                                          size_t certificate_length,
                                          const uint8_t *private_key,
                                          size_t private_key_length);
/**
 * @brief Returns a view of the stored device certificate.
 * @param out_view Receives a pointer/length into the Flash-mapped store; see
 * @ref security_service_blob_view_t lifetime note.
 * @return false if no certificate is stored or the store is not ready.
 */
bool security_service_get_device_certificate(security_service_blob_view_t *out_view);
/**
 * @brief Returns a view of the stored device private key.
 * @param out_view Receives a pointer/length into the Flash-mapped store; see
 * @ref security_service_blob_view_t lifetime note.
 * @return false if no private key is stored or the store is not ready.
 */
bool security_service_get_device_private_key(security_service_blob_view_t *out_view);
/**
 * @brief Returns a view of the stored PKI trust list (concatenated trusted
 * certificate bytes, PEM or DER).
 * @param out_view Receives a pointer/length into the Flash-mapped store.
 * @return false if the trust list is empty or the store is not ready.
 */
bool security_service_get_trust_list(security_service_blob_view_t *out_view);
/**
 * @brief Replaces the stored trust list and bumps `trust_list_version`.
 *
 * Passing `trust_list == NULL` or `trust_list_length == 0` clears the trust
 * list and re-enters commissioning mode (mirrors into
 * `security_state_service`). No-op (returns true without writing) if the
 * supplied bytes are byte-identical to the current trust list.
 *
 * @param trust_list New trust list bytes, or NULL to clear.
 * @param trust_list_length Length of @p trust_list, `<= SECURITY_SERVICE_TRUST_LIST_MAX_LEN`.
 * @return true on success; false if too large, Flash commit fails, or the
 * `security_state_service` sync fails.
 * @note Emits a "trust-list-update" audit log entry on success.
 */
bool security_service_set_trust_list(const uint8_t *trust_list, size_t trust_list_length);
/**
 * @brief Reads the trust list version counter.
 * Prefers the `security_state_service` mirror; falls back to the Flash
 * store's own counter if the state service has not been initialized.
 * @return false if neither source is available.
 */
bool security_service_get_trust_list_version(uint32_t *out_version);
/**
 * @brief Reports whether the device is in commissioning mode.
 *
 * Prefers the `security_state_service` mirror. Falls back to: "no store"
 * or "empty trust list" both imply commissioning mode is (conceptually)
 * active, since an OPC UA server with no trust list cannot validate client
 * certificates.
 */
bool security_service_is_commissioning_mode(void);
/**
 * @brief Sets commissioning mode.
 *
 * Rejects leaving commissioning mode (`enabled == false`) while the trust
 * list is empty, since that would leave the server unable to validate any
 * client certificate. No-op (returns true) if the mode is already as
 * requested.
 * @return false if disallowed by the trust-list-empty rule, or on Flash/state
 * sync failure.
 */
bool security_service_set_commissioning_mode(bool enabled);
/**
 * @brief Returns the epoch second of the most recent authentication
 * failure recorded since boot.
 * @return false if no failure has been recorded yet.
 */
bool security_service_get_last_auth_failure(uint32_t *out_epoch_s);
/** @return Count of authentication failures recorded since boot (not persisted). */
uint32_t security_service_get_failed_login_count(void);
/**
 * @brief Counts entries in the stored trust list by parsing it as a
 * concatenated X.509 certificate chain.
 * @return 0 if the trust list is empty; 1 if the trust list is non-empty but
 * fails to parse as valid certificates (conservative non-zero fallback);
 * otherwise the number of parsed certificates.
 */
uint32_t security_service_get_trusted_client_count(void);
/**
 * @brief Computes the SHA-1 thumbprint of the device certificate as an
 * uppercase hex string.
 * @param buffer Destination buffer, must be at least
 * `SECURITY_SERVICE_CERT_THUMBPRINT_LEN + 1` bytes (40 hex chars + NUL).
 * @param buffer_size Size of @p buffer.
 * @return false if no device certificate is stored or @p buffer is too small.
 */
bool security_service_get_device_certificate_thumbprint(char *buffer, size_t buffer_size);
/**
 * @brief Returns the device certificate's `notBefore` validity time, parsed
 * from the X.509 structure.
 * @return false if no device certificate is stored or the time field could
 * not be converted to a Unix epoch (e.g. year < 1970 or out-of-range fields).
 */
bool security_service_get_device_certificate_not_before(uint32_t *out_epoch_s);
/**
 * @brief Returns the device certificate's `notAfter` validity time, parsed
 * from the X.509 structure.
 * @return false if no device certificate is stored or the time field could
 * not be converted to a Unix epoch.
 */
bool security_service_get_device_certificate_not_after(uint32_t *out_epoch_s);

/**
 * @brief Verifies a password against a stored user record without applying
 * login-lockout bookkeeping.
 *
 * @note Unlike `security_service_authenticate_user()`, this does not consult
 * or update the login-backoff runtime state and is not the entry point used
 * for interactive session login; used for lower-stakes password checks
 * (e.g. confirming the old password before a change).
 * @return false if the user does not exist, is disabled, or the password
 * does not match.
 */
bool security_service_verify_password(const char *username,
                                      const uint8_t *password,
                                      size_t password_length);
/**
 * @brief Authenticates a user for session/login purposes, applying
 * per-username exponential backoff on repeated failures.
 *
 * Backoff starts at `SECURITY_SERVICE_AUTH_BASE_BACKOFF_MS` (1s) and doubles
 * per consecutive failure up to `SECURITY_SERVICE_AUTH_MAX_BACKOFF_MS` (32s).
 * Unknown usernames share a single backoff bucket (`g_unknown_auth_runtime`)
 * to avoid revealing whether a username exists via timing. Every attempt
 * (locked-out, invalid, or successful) is written to the audit log.
 *
 * @param out_role Set to `SECURITY_ROLE_VIEWER` on entry; overwritten with
 * the user's role only on success.
 * @param out_retry_after_ms Set to 0 on entry; on `SECURITY_AUTH_RESULT_LOCKED_OUT`
 * or `SECURITY_AUTH_RESULT_INVALID_CREDENTIALS`, set to the remaining/new
 * backoff duration in milliseconds the caller should wait before retrying.
 * @return `SECURITY_AUTH_RESULT_SUCCESS`, `SECURITY_AUTH_RESULT_INVALID_CREDENTIALS`,
 * or `SECURITY_AUTH_RESULT_LOCKED_OUT`.
 */
security_auth_result_t security_service_authenticate_user(const char *username,
                                                          const uint8_t *password,
                                                          size_t password_length,
                                                          security_user_role_t *out_role,
                                                          uint32_t *out_retry_after_ms);
/**
 * @brief Looks up a user's role.
 * @return false if the user does not exist or is disabled.
 */
bool security_service_get_role(const char *username, security_user_role_t *out_role);

/**
 * @brief Creates a new local user account.
 * @param username Must be non-empty and shorter than `SECURITY_SERVICE_USERNAME_MAX_LEN`.
 * @param password 1..128 bytes.
 * @param role Must be a valid `security_user_role_t` value.
 * @return false if arguments are invalid, the username already exists, the
 * `SECURITY_SERVICE_MAX_USERS`-slot table is full, or the Flash commit fails.
 * @note Resets any existing login-backoff state for the (possibly reused)
 * slot index.
 */
bool security_service_add_user(const char *username,
                               const uint8_t *password,
                               size_t password_length,
                               security_user_role_t role);
/**
 * @brief Changes a user's password.
 *
 * @param old_password If non-NULL/non-empty, must verify against the current
 * password before the change is applied; pass NULL/0-length to skip this
 * check (used for administrative resets).
 * @param new_password 1..128 bytes.
 * @return false if the user does not exist, the old-password check fails,
 * @p new_password is invalid, or the Flash commit fails.
 */
bool security_service_change_password(const char *username,
                                      const uint8_t *old_password,
                                      size_t old_password_length,
                                      const uint8_t *new_password,
                                      size_t new_password_length);
/**
 * @brief Changes a user's role.
 *
 * Refuses to demote the last enabled admin account (would leave the device
 * with no admin).
 * @return false if the user does not exist, is disabled, @p role is invalid,
 * the last-admin rule blocks the change, or the Flash commit fails.
 */
bool security_service_set_role(const char *username, security_user_role_t role);
/**
 * @brief Disables (soft-deletes login capability for) a user account.
 *
 * Refuses to disable the last enabled admin account. Already-disabled users
 * return true without writing.
 * @return false if the user does not exist, the last-admin rule blocks it, or
 * the Flash commit fails.
 */
bool security_service_disable_user(const char *username);
/**
 * @brief Permanently removes a user account record.
 *
 * Refuses to delete the last enabled admin account.
 * @return false if the user does not exist, the last-admin rule blocks it, or
 * the Flash commit fails.
 */
bool security_service_delete_user(const char *username);

/** @return false if @p text does not match a known role name ("Viewer"/"Operator"/"Service"/"Admin"). */
bool security_service_role_from_string(const char *text, security_user_role_t *out_role);
/** @return Static role name string, or "Unknown" for an out-of-range value. */
const char *security_service_role_to_string(security_user_role_t role);
/**
 * @brief Registers (or clears, with `callback == NULL`) the audit-event sink
 * invoked by `security_service_audit_log()`.
 * @note Not synchronized; intended to be called once during startup wiring.
 */
void security_service_set_audit_callback(security_service_audit_callback_t callback, void *context);
/**
 * @brief Writes a structured audit-log line to stdout and forwards it to the
 * registered audit callback, if any.
 *
 * @param event Short machine-readable event name.
 * @param actor Username or NULL/"" (logged as "system").
 * @param subject Object of the action, or NULL/"" (logged as "-").
 * @param success Whether the action succeeded.
 * @param details Optional free-form detail string.
 * @note Flushes stdout after printing; called from whatever task context
 * triggered the security event (login, certificate/trust-list mutation).
 */
void security_service_audit_log(const char *event,
                                const char *actor,
                                const char *subject,
                                bool success,
                                const char *details);

#endif
