/**
 * @file
 * @brief Firmware-wide compile-time capacity constants shared across services.
 *
 * These bounds size static/PSRAM arrays in the channel registry, config
 * store, OPC UA adapter context, PubSub writers, diagnostics, and both
 * history ring backends. Changing either value changes on-flash/PSRAM
 * layout sizing for every subsystem that includes this header, so builds
 * override them at compile time rather than at runtime.
 */
#ifndef SERVICES_PLATFORM_LIMITS_H
#define SERVICES_PLATFORM_LIMITS_H

/**
 * @brief Maximum number of channels supported by this firmware build.
 *
 * Governs persisted configuration, the runtime channel registry, OPC UA
 * adapter context, PubSub writers, and diagnostics. Profiles may lower the
 * bound at compile time via `-DPLATFORM_CHANNEL_LIMIT=N`; the reference
 * RP2350 profile uses 16. Enforced to the closed range [1, 16] by the
 * `#error` below.
 */
#ifndef PLATFORM_CHANNEL_LIMIT
#define PLATFORM_CHANNEL_LIMIT 16u
#endif

#if PLATFORM_CHANNEL_LIMIT < 1u || PLATFORM_CHANNEL_LIMIT > 16u
#error "PLATFORM_CHANNEL_LIMIT must be in the supported range 1..16"
#endif

/**
 * @brief Fixed per-channel history retention capacity (sample count).
 *
 * v0.4.3: one fixed retention capacity per channel, uniform across channels,
 * shared by both the PSRAM history ring (`services/history_service.c`) and
 * the Flash-backed history ring (`drivers/history_flash_backend.c`) so the
 * two backends stay numerically consistent by construction. Do not change
 * independently for the two backends.
 */
#ifndef PLATFORM_HISTORY_SAMPLES_PER_CHANNEL
#define PLATFORM_HISTORY_SAMPLES_PER_CHANNEL 256u
#endif

#endif
