/*
 * T0 RAM-only crypto probe configuration overlay.
 *
 * This is intentionally separate from port/mbedtls/inc/ssl_config.h.  The
 * primary probe build applies it through MBEDTLS_USER_CONFIG_FILE, so the
 * production configuration is neither included nor modified.
 */
#ifndef V060_T0_CRYPTO_PROBE_USER_CONFIG_H
#define V060_T0_CRYPTO_PROBE_USER_CONFIG_H

/* Keep the non-allocator policy aligned with the application configuration. */
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_MEMORY
#define MBEDTLS_HAVE_TIME
#define MBEDTLS_PLATFORM_MS_TIME_ALT
#undef MBEDTLS_HAVE_TIME_DATE
#undef MBEDTLS_NET_C
#undef MBEDTLS_TIMING_C
#undef MBEDTLS_FS_IO
#undef MBEDTLS_PSA_ITS_FILE_C
#undef MBEDTLS_PSA_CRYPTO_STORAGE_C
#undef MBEDTLS_PSA_CRYPTO_C
#undef MBEDTLS_LMS_C
#undef MBEDTLS_LMS_PRIVATE
#undef MBEDTLS_SSL_PROTO_TLS1_3
#undef MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_EPHEMERAL_ENABLED
#undef MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_PSK_EPHEMERAL_ENABLED
#undef MBEDTLS_SSL_TLS1_3_COMPATIBILITY_MODE
#undef MBEDTLS_AESNI_C
#undef MBEDTLS_SHA3_C
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT

/* Probe-only allocator instrumentation. No FreeRTOS allocator is involved. */
#define MBEDTLS_MEMORY_BUFFER_ALLOC_C
#define MBEDTLS_MEMORY_DEBUG

/* Preserve the RP2350 SHA-256 alternate implementation used by the app. */
#if defined(PICO_RP2350)
#define MBEDTLS_SHA256_ALT
#define PICO_MBEDTLS_SHA256_ALT_USE_DMA 1
#endif

#endif /* V060_T0_CRYPTO_PROBE_USER_CONFIG_H */
