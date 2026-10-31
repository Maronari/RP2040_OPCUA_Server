#ifndef OTA_OTA_VERIFIER_MBEDTLS_CONFIG_H
#define OTA_OTA_VERIFIER_MBEDTLS_CONFIG_H

/* Applied only while compiling the separately namespaced verifier primitives. */
#include <stddef.h>

void *ota_verifier_calloc(size_t count, size_t size);
void ota_verifier_free(void *pointer);

#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_MEMORY
#define MBEDTLS_PLATFORM_CALLOC_MACRO ota_verifier_calloc
#define MBEDTLS_PLATFORM_FREE_MACRO ota_verifier_free

/* The verifier deliberately compiles only the primitive closure it invokes. */
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_ECP_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_SHA256_C

#endif /* OTA_OTA_VERIFIER_MBEDTLS_CONFIG_H */
