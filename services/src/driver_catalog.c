#include "services/driver_catalog.h"

#include <string.h>

#include "drivers/native_channel_driver.h"

#ifndef FIRMWARE_ENABLE_NATIVE_INTERNAL_TEMP_DRIVER
#define FIRMWARE_ENABLE_NATIVE_INTERNAL_TEMP_DRIVER 1
#endif
#ifndef FIRMWARE_ENABLE_NATIVE_ADC_VOLTAGE_DRIVER
#define FIRMWARE_ENABLE_NATIVE_ADC_VOLTAGE_DRIVER 1
#endif
#ifndef FIRMWARE_ENABLE_NATIVE_GPIO_INPUT_DRIVER
#define FIRMWARE_ENABLE_NATIVE_GPIO_INPUT_DRIVER 1
#endif

#if FIRMWARE_ENABLE_NATIVE_INTERNAL_TEMP_DRIVER
static bool validate_internal_temp(uint8_t source_index, uint8_t source_flags)
{
    return native_internal_temperature_source_is_valid(source_index, source_flags);
}
#endif

#if FIRMWARE_ENABLE_NATIVE_ADC_VOLTAGE_DRIVER
static bool validate_adc(uint8_t source_index, uint8_t source_flags)
{
    return native_adc_voltage_source_is_valid(source_index, source_flags);
}
#endif

#if FIRMWARE_ENABLE_NATIVE_GPIO_INPUT_DRIVER
static bool validate_gpio(uint8_t source_index, uint8_t source_flags)
{
    return native_gpio_input_source_is_valid(source_index, source_flags);
}
#endif

#if FIRMWARE_ENABLE_NATIVE_INTERNAL_TEMP_DRIVER
static const telemetry_driver_vtbl_t g_internal_temp_driver = {
    .start = native_internal_temperature_start,
    .stop = native_internal_temperature_stop,
    .read = native_internal_temperature_read,
};
#endif
#if FIRMWARE_ENABLE_NATIVE_ADC_VOLTAGE_DRIVER
static const telemetry_driver_vtbl_t g_adc_voltage_driver = {
    .start = native_adc_voltage_start,
    .stop = native_adc_voltage_stop,
    .read = native_adc_voltage_read,
};
#endif
#if FIRMWARE_ENABLE_NATIVE_GPIO_INPUT_DRIVER
static const telemetry_driver_vtbl_t g_gpio_input_driver = {
    .start = native_gpio_input_start,
    .stop = native_gpio_input_stop,
    .read = native_gpio_input_read,
};
#endif

static const driver_catalog_entry_t g_driver_catalog[] = {
#if FIRMWARE_ENABLE_NATIVE_INTERNAL_TEMP_DRIVER
    {
        .driver_id = CONFIG_CHANNEL_DRIVER_INTERNAL_TEMP,
        .name = "internal_temp",
        .default_unit = "degC",
        .default_kind = TELEMETRY_CHANNEL_KIND_ANALOG,
        .default_value_type = TELEMETRY_CHANNEL_VALUE_FLOAT,
        .default_flags = TELEMETRY_CHANNEL_FLAG_ENABLED | TELEMETRY_CHANNEL_FLAG_HISTORIZED |
                         TELEMETRY_CHANNEL_FLAG_ALARM_ENABLED,
        .analog = true,
        .supported_value_types = TELEMETRY_CHANNEL_VALUE_TYPE_BIT(TELEMETRY_CHANNEL_VALUE_FLOAT) |
                                 TELEMETRY_CHANNEL_VALUE_TYPE_BIT(TELEMETRY_CHANNEL_VALUE_DOUBLE),
        .capabilities = TELEMETRY_DRIVER_CAPABILITY_START | TELEMETRY_DRIVER_CAPABILITY_STOP |
                        TELEMETRY_DRIVER_CAPABILITY_READ,
        .validate_source = validate_internal_temp,
        .driver = &g_internal_temp_driver,
    },
#endif
#if FIRMWARE_ENABLE_NATIVE_ADC_VOLTAGE_DRIVER
    {
        .driver_id = CONFIG_CHANNEL_DRIVER_ADC_VOLTAGE,
        .name = "adc_voltage",
        .default_unit = "V",
        .default_kind = TELEMETRY_CHANNEL_KIND_ANALOG,
        .default_value_type = TELEMETRY_CHANNEL_VALUE_FLOAT,
        .default_flags = TELEMETRY_CHANNEL_FLAG_ENABLED | TELEMETRY_CHANNEL_FLAG_HISTORIZED,
        .analog = true,
        .supported_value_types = TELEMETRY_CHANNEL_VALUE_TYPE_BIT(TELEMETRY_CHANNEL_VALUE_FLOAT) |
                                 TELEMETRY_CHANNEL_VALUE_TYPE_BIT(TELEMETRY_CHANNEL_VALUE_DOUBLE),
        .capabilities = TELEMETRY_DRIVER_CAPABILITY_START | TELEMETRY_DRIVER_CAPABILITY_STOP |
                        TELEMETRY_DRIVER_CAPABILITY_READ,
        .validate_source = validate_adc,
        .driver = &g_adc_voltage_driver,
    },
#endif
#if FIRMWARE_ENABLE_NATIVE_GPIO_INPUT_DRIVER
    {
        .driver_id = CONFIG_CHANNEL_DRIVER_GPIO_INPUT,
        .name = "gpio_input",
        .default_unit = "",
        .default_kind = TELEMETRY_CHANNEL_KIND_DISCRETE,
        .default_value_type = TELEMETRY_CHANNEL_VALUE_BOOLEAN,
        .default_flags = TELEMETRY_CHANNEL_FLAG_ENABLED,
        .analog = false,
        .supported_value_types = TELEMETRY_CHANNEL_VALUE_TYPE_BIT(TELEMETRY_CHANNEL_VALUE_BOOLEAN),
        .capabilities = TELEMETRY_DRIVER_CAPABILITY_START | TELEMETRY_DRIVER_CAPABILITY_STOP |
                        TELEMETRY_DRIVER_CAPABILITY_READ,
        .validate_source = validate_gpio,
        .driver = &g_gpio_input_driver,
    },
#endif
    /* Keeps the array valid when every optional native driver is excluded. */
    {0},
};

const driver_catalog_entry_t *driver_catalog_get_by_id(uint8_t driver_id)
{
    for (size_t index = 0u; index < driver_catalog_count(); ++index)
    {
        if (g_driver_catalog[index].driver_id == driver_id)
        {
            return &g_driver_catalog[index];
        }
    }
    return NULL;
}

const driver_catalog_entry_t *driver_catalog_get_by_name(const char *name)
{
    if (name == NULL)
    {
        return NULL;
    }
    for (size_t index = 0u; index < driver_catalog_count(); ++index)
    {
        if (strcmp(g_driver_catalog[index].name, name) == 0)
        {
            return &g_driver_catalog[index];
        }
    }
    return NULL;
}

size_t driver_catalog_count(void)
{
    return (sizeof(g_driver_catalog) / sizeof(g_driver_catalog[0])) - 1u;
}

const driver_catalog_entry_t *driver_catalog_get_by_index(size_t index)
{
    return index < driver_catalog_count() ? &g_driver_catalog[index] : NULL;
}

bool driver_catalog_supports_value_type(const driver_catalog_entry_t *entry,
                                        telemetry_channel_value_type_t value_type)
{
    return entry != NULL && value_type <= TELEMETRY_CHANNEL_VALUE_STRING &&
           (entry->supported_value_types & TELEMETRY_CHANNEL_VALUE_TYPE_BIT(value_type)) != 0u;
}

bool driver_catalog_get_resource_claims(uint8_t driver_id,
                                        uint8_t source_index,
                                        uint8_t source_flags,
                                        driver_catalog_resource_claim_t *out_claims,
                                        size_t claims_capacity,
                                        size_t *out_claim_count)
{
    const driver_catalog_entry_t *entry = driver_catalog_get_by_id(driver_id);
    size_t required = 1u;

    if (out_claim_count == NULL || entry == NULL || entry->validate_source == NULL ||
        !entry->validate_source(source_index, source_flags))
    {
        return false;
    }
    if (driver_id == CONFIG_CHANNEL_DRIVER_ADC_VOLTAGE)
    {
        required = 2u;
    }
    *out_claim_count = required;
    if (out_claims == NULL || claims_capacity < required)
    {
        return false;
    }
    out_claims[0] = (driver_catalog_resource_claim_t){
        .type = DRIVER_CATALOG_RESOURCE_NONE,
        .index = 0u,
        .shared = true,
    };
    if (driver_id == CONFIG_CHANNEL_DRIVER_INTERNAL_TEMP)
    {
        out_claims[0].type = DRIVER_CATALOG_RESOURCE_ADC_PERIPHERAL;
    }
    else if (driver_id == CONFIG_CHANNEL_DRIVER_ADC_VOLTAGE)
    {
        out_claims[0].type = DRIVER_CATALOG_RESOURCE_ADC_INPUT;
        out_claims[0].index = source_index;
        out_claims[0].shared = false;
        out_claims[1] = (driver_catalog_resource_claim_t){
            .type = DRIVER_CATALOG_RESOURCE_ADC_PERIPHERAL,
            .index = 0u,
            .shared = true,
        };
    }
    else if (driver_id == CONFIG_CHANNEL_DRIVER_GPIO_INPUT)
    {
        out_claims[0].type = DRIVER_CATALOG_RESOURCE_GPIO;
        out_claims[0].index = source_index;
        out_claims[0].shared = false;
    }
    return true;
}

bool driver_catalog_claims_are_compatible(const driver_catalog_resource_claim_t *claims,
                                          size_t claim_count)
{
    if (claims == NULL && claim_count != 0u)
    {
        return false;
    }
    for (size_t index = 0u; index < claim_count; ++index)
    {
        for (size_t other = index + 1u; other < claim_count; ++other)
        {
            if (claims[index].type != DRIVER_CATALOG_RESOURCE_NONE &&
                claims[index].type == claims[other].type && claims[index].index == claims[other].index &&
                (!claims[index].shared || !claims[other].shared))
            {
                return false;
            }
        }
    }
    return true;
}
