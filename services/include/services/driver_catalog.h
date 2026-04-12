#ifndef SERVICES_DRIVER_CATALOG_H
#define SERVICES_DRIVER_CATALOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "services/telemetry_channel.h"

typedef enum
{
    CONFIG_CHANNEL_DRIVER_NONE = 0,
    CONFIG_CHANNEL_DRIVER_INTERNAL_TEMP = 1,
    CONFIG_CHANNEL_DRIVER_ADC_VOLTAGE = 2,
    CONFIG_CHANNEL_DRIVER_GPIO_INPUT = 3
} config_channel_driver_id_t;

typedef enum
{
    CONFIG_CHANNEL_SOURCE_FLAG_INVERT = (1u << 0u),
    CONFIG_CHANNEL_SOURCE_FLAG_PULLUP = (1u << 1u),
    CONFIG_CHANNEL_SOURCE_FLAG_PULLDOWN = (1u << 2u)
} config_channel_source_flags_t;

typedef bool (*driver_catalog_validate_source_fn)(uint8_t source_index, uint8_t source_flags);
typedef bool (*driver_catalog_init_fn)(telemetry_channel_t *channel, uint8_t source_index, uint8_t source_flags);
typedef bool (*driver_catalog_poll_fn)(telemetry_channel_t *channel,
                                       uint8_t source_index,
                                       uint8_t source_flags,
                                       uint32_t now_ms);

typedef struct
{
    uint8_t driver_id;
    const char *name;
    const char *default_unit;
    telemetry_channel_kind_t default_kind;
    telemetry_channel_value_type_t default_value_type;
    uint32_t default_flags;
    bool analog;
    driver_catalog_validate_source_fn validate_source;
    driver_catalog_init_fn init;
    driver_catalog_poll_fn poll;
} driver_catalog_entry_t;

const driver_catalog_entry_t *driver_catalog_get_by_id(uint8_t driver_id);
const driver_catalog_entry_t *driver_catalog_get_by_name(const char *name);
size_t driver_catalog_count(void);
const driver_catalog_entry_t *driver_catalog_get_by_index(size_t index);

#endif
