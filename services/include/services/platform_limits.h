#ifndef SERVICES_PLATFORM_LIMITS_H
#define SERVICES_PLATFORM_LIMITS_H

/*
 * One firmware-wide bound governs persisted configuration, runtime registry,
 * OPC UA adapter context, PubSub writers, and diagnostics.  Profiles may
 * lower the bound at compile time; the reference RP2350 profile uses 16.
 */
#ifndef PLATFORM_CHANNEL_LIMIT
#define PLATFORM_CHANNEL_LIMIT 16u
#endif

#if PLATFORM_CHANNEL_LIMIT < 1u || PLATFORM_CHANNEL_LIMIT > 16u
#error "PLATFORM_CHANNEL_LIMIT must be in the supported range 1..16"
#endif

/*
 * v0.4.3: one fixed retention capacity per channel, uniform across channels,
 * shared by both the PSRAM history ring (services/history_service) and the
 * Flash-backed history ring (drivers/history_flash_backend) so the two
 * backends stay numerically consistent by construction.
 */
#ifndef PLATFORM_HISTORY_SAMPLES_PER_CHANNEL
#define PLATFORM_HISTORY_SAMPLES_PER_CHANNEL 256u
#endif

#endif
