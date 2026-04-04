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

typedef enum
{
    SECURITY_ROLE_VIEWER = 0,
    SECURITY_ROLE_OPERATOR = 1,
    SECURITY_ROLE_SERVICE = 2,
    SECURITY_ROLE_ADMIN = 3
} security_user_role_t;

typedef struct
{
    const uint8_t *data;
    size_t length;
} security_service_blob_view_t;

bool security_service_init(void);

bool security_service_has_device_identity(void);
bool security_service_seed_device_identity_if_missing(const uint8_t *certificate,
                                                      size_t certificate_length,
                                                      const uint8_t *private_key,
                                                      size_t private_key_length);
bool security_service_set_device_identity(const uint8_t *certificate,
                                          size_t certificate_length,
                                          const uint8_t *private_key,
                                          size_t private_key_length);
bool security_service_get_device_certificate(security_service_blob_view_t *out_view);
bool security_service_get_device_private_key(security_service_blob_view_t *out_view);
bool security_service_get_trust_list(security_service_blob_view_t *out_view);

bool security_service_verify_password(const char *username,
                                      const uint8_t *password,
                                      size_t password_length);
bool security_service_get_role(const char *username, security_user_role_t *out_role);

bool security_service_add_user(const char *username,
                               const uint8_t *password,
                               size_t password_length,
                               security_user_role_t role);
bool security_service_change_password(const char *username,
                                      const uint8_t *old_password,
                                      size_t old_password_length,
                                      const uint8_t *new_password,
                                      size_t new_password_length);
bool security_service_set_role(const char *username, security_user_role_t role);
bool security_service_disable_user(const char *username);
bool security_service_delete_user(const char *username);

bool security_service_role_from_string(const char *text, security_user_role_t *out_role);
const char *security_service_role_to_string(security_user_role_t role);

#endif
