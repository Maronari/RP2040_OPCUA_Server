#ifndef SERVICES_DRIVER_CATALOG_H
#define SERVICES_DRIVER_CATALOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "services/telemetry_channel.h"
#include "services/telemetry_driver.h"

typedef enum
{
    CONFIG_CHANNEL_DRIVER_NONE = 0,
    CONFIG_CHANNEL_DRIVER_INTERNAL_TEMP = 1,
    CONFIG_CHANNEL_DRIVER_ADC_VOLTAGE = 2,
    CONFIG_CHANNEL_DRIVER_GPIO_INPUT = 3
} config_channel_driver_id_t;

#define TELEMETRY_CHANNEL_VALUE_TYPE_BIT(type) (1u << (uint32_t)(type))

typedef enum
{
    DRIVER_CATALOG_RESOURCE_NONE = 0,
    DRIVER_CATALOG_RESOURCE_ADC_INPUT,
    DRIVER_CATALOG_RESOURCE_ADC_PERIPHERAL,
    DRIVER_CATALOG_RESOURCE_GPIO
} driver_catalog_resource_type_t;

#define DRIVER_CATALOG_MAX_RESOURCE_CLAIMS_PER_CHANNEL 2u

typedef struct
{
    driver_catalog_resource_type_t type;
    uint8_t index;
    bool shared;
} driver_catalog_resource_claim_t;

typedef enum
{
    CONFIG_CHANNEL_SOURCE_FLAG_INVERT = (1u << 0u),
    CONFIG_CHANNEL_SOURCE_FLAG_PULLUP = (1u << 1u),
    CONFIG_CHANNEL_SOURCE_FLAG_PULLDOWN = (1u << 2u)
} config_channel_source_flags_t;

typedef bool (*driver_catalog_validate_source_fn)(uint8_t source_index, uint8_t source_flags);

typedef struct
{
    uint8_t driver_id;
    const char *name;
    const char *default_unit;
    telemetry_channel_kind_t default_kind;
    telemetry_channel_value_type_t default_value_type;
    uint32_t default_flags;
    bool analog;
    uint32_t supported_value_types;
    uint32_t capabilities;
    driver_catalog_validate_source_fn validate_source;
    const telemetry_driver_vtbl_t *driver;
} driver_catalog_entry_t;

const driver_catalog_entry_t *driver_catalog_get_by_id(uint8_t driver_id);
const driver_catalog_entry_t *driver_catalog_get_by_name(const char *name);
size_t driver_catalog_count(void);
const driver_catalog_entry_t *driver_catalog_get_by_index(size_t index);
bool driver_catalog_supports_value_type(const driver_catalog_entry_t *entry,
                                        telemetry_channel_value_type_t value_type);
/* Returns false for an invalid source or insufficient output capacity.  On
 * capacity failure, out_claim_count receives the required bounded count and
 * no partial claim set is returned. */
bool driver_catalog_get_resource_claims(uint8_t driver_id,
                                        uint8_t source_index,
                                        uint8_t source_flags,
                                        driver_catalog_resource_claim_t *out_claims,
                                        size_t claims_capacity,
                                        size_t *out_claim_count);
bool driver_catalog_claims_are_compatible(const driver_catalog_resource_claim_t *claims,
                                          size_t claim_count);

#endif
