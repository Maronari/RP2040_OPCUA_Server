#ifndef SERVICES_CHANNEL_CONFIG_H
#define SERVICES_CHANNEL_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#include "services/telemetry_channel.h"

/**
 * @brief One channel's configuration-time definition: identity, settings,
 *        and the driver vtable + opaque context it will run against.
 *
 * Produced by sensor_manager while building a candidate bank from
 * device_profile_config_t and driver_catalog lookups, then consumed by
 * channel_manager_register_from_config() / channel_manager_register(),
 * which copies @c metadata's id/name/description/unit strings into owned
 * registry storage -- this struct (and any string it points at) is not
 * retained past the register call. @c driver is expected to come from
 * driver_catalog_get_by_id()/_by_name() (see driver_catalog.h); @c
 * driver_context is opaque to channel_manager and passed through unchanged
 * to every driver call.
 */
typedef struct
{
    telemetry_channel_metadata_t metadata; /* copied by channel_manager */
    telemetry_channel_settings_t settings;
    const telemetry_driver_vtbl_t *driver;
    void *driver_context;
} channel_config_t;

/**
 * @brief A whole device's channel set as a flat array + count.
 *
 * Non-owning view: @c channels is expected to point at a caller-owned array;
 * the pointer is only valid for the lifetime the caller guarantees. No
 * first-party caller of this type was found in this pass (functions such as
 * channel_manager_register_from_config() take a raw
 * `(const channel_config_t *, size_t)` pair instead) -- treat this as a
 * currently-unused convenience aggregate rather than an active call-site
 * contract; not statically confirmed to be dead code.
 */
typedef struct
{
    const channel_config_t *channels;
    size_t channel_count;
} telemetry_device_config_t;

#endif
