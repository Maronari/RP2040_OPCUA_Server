/**
 * @file
 * @brief Compile-time static table mapping config_channel_driver_id_t values
 *        to their driver_catalog_entry_t (default metadata, supported value
 *        types, source validator, and telemetry_driver_vtbl_t).
 *
 * Responsibilities:
 * - Builds g_driver_catalog once at compile time from the
 *   FIRMWARE_ENABLE_NATIVE_*_DRIVER switches (drivers/CMakeLists.txt
 *   options) -- a disabled driver is entirely absent from the compiled ELF,
 *   not merely hidden at runtime.
 * - Wires each catalog entry's driver_catalog_validate_source_fn to the
 *   corresponding native driver's *_source_is_valid() function
 *   (drivers/include/drivers/native_channel_driver.h) and its
 *   telemetry_driver_vtbl_t to that driver's start/stop/read operations.
 * - Computes/validates physical resource claims (ADC input, shared ADC
 *   peripheral, GPIO) to catch configuration-time resource conflicts before
 *   channel registration.
 *
 * Ownership: g_driver_catalog and the per-driver vtbl globals below have
 * static storage duration for the process lifetime; entries are read-only
 * after program startup.
 *
 * Concurrency: none needed -- every symbol here is immutable after start-up
 * initialization (no runtime mutation of the catalog).
 */

#include "services/driver_catalog.h"

#include <string.h>

#include "drivers/ad5422_driver.h"
#include "drivers/ads8688_driver.h"
#include "drivers/ips2050_driver.h"
#include "drivers/max22190_driver.h"
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
#ifndef FIRMWARE_ENABLE_NATIVE_GPIO_OUTPUT_DRIVER
#define FIRMWARE_ENABLE_NATIVE_GPIO_OUTPUT_DRIVER 1
#endif
#ifndef FIRMWARE_ENABLE_NATIVE_ADS8688_DRIVER
#define FIRMWARE_ENABLE_NATIVE_ADS8688_DRIVER 1
#endif
#ifndef FIRMWARE_ENABLE_NATIVE_AD5422_DRIVER
#define FIRMWARE_ENABLE_NATIVE_AD5422_DRIVER 1
#endif
#ifndef FIRMWARE_ENABLE_NATIVE_MAX22190_DRIVER
#define FIRMWARE_ENABLE_NATIVE_MAX22190_DRIVER 1
#endif
#ifndef FIRMWARE_ENABLE_NATIVE_IPS2050_DRIVER
#define FIRMWARE_ENABLE_NATIVE_IPS2050_DRIVER 1
#endif

#if FIRMWARE_ENABLE_NATIVE_INTERNAL_TEMP_DRIVER
/** @brief driver_catalog_validate_source_fn for CONFIG_CHANNEL_DRIVER_INTERNAL_TEMP; delegates to the native driver's own source validation (drivers/native_channel_driver.h). */
static bool validate_internal_temp(uint8_t source_index, uint8_t source_flags)
{
    return native_internal_temperature_source_is_valid(source_index, source_flags);
}
#endif

#if FIRMWARE_ENABLE_NATIVE_ADC_VOLTAGE_DRIVER
/** @brief driver_catalog_validate_source_fn for CONFIG_CHANNEL_DRIVER_ADC_VOLTAGE; delegates to the native driver's own source validation. */
static bool validate_adc(uint8_t source_index, uint8_t source_flags)
{
    return native_adc_voltage_source_is_valid(source_index, source_flags);
}
#endif

#if FIRMWARE_ENABLE_NATIVE_GPIO_INPUT_DRIVER
/** @brief driver_catalog_validate_source_fn for CONFIG_CHANNEL_DRIVER_GPIO_INPUT; delegates to the native driver's own source validation. */
static bool validate_gpio(uint8_t source_index, uint8_t source_flags)
{
    return native_gpio_input_source_is_valid(source_index, source_flags);
}
#endif

#if FIRMWARE_ENABLE_NATIVE_GPIO_OUTPUT_DRIVER
/** @brief driver_catalog_validate_source_fn for CONFIG_CHANNEL_DRIVER_GPIO_OUTPUT; delegates to the native driver's own source validation. */
static bool validate_gpio_output(uint8_t source_index, uint8_t source_flags)
{
    return native_gpio_output_source_is_valid(source_index, source_flags);
}
#endif

#if FIRMWARE_ENABLE_NATIVE_ADS8688_DRIVER
/** @brief driver_catalog_validate_source_fn for CONFIG_CHANNEL_DRIVER_ADS8688; delegates to the native driver's own source validation. */
static bool validate_ads8688(uint8_t source_index, uint8_t source_flags)
{
    return native_ads8688_source_is_valid(source_index, source_flags);
}
#endif

#if FIRMWARE_ENABLE_NATIVE_AD5422_DRIVER
/** @brief driver_catalog_validate_source_fn for CONFIG_CHANNEL_DRIVER_AD5422; delegates to the native driver's own source validation. */
static bool validate_ad5422(uint8_t source_index, uint8_t source_flags)
{
    return native_ad5422_source_is_valid(source_index, source_flags);
}
#endif

#if FIRMWARE_ENABLE_NATIVE_MAX22190_DRIVER
/** @brief driver_catalog_validate_source_fn for CONFIG_CHANNEL_DRIVER_MAX22190; delegates to the native driver's own source validation. */
static bool validate_max22190(uint8_t source_index, uint8_t source_flags)
{
    return native_max22190_source_is_valid(source_index, source_flags);
}
#endif

#if FIRMWARE_ENABLE_NATIVE_IPS2050_DRIVER
/** @brief driver_catalog_validate_source_fn for CONFIG_CHANNEL_DRIVER_IPS2050; delegates to the native driver's own source validation. */
static bool validate_ips2050(uint8_t source_index, uint8_t source_flags)
{
    return native_ips2050_source_is_valid(source_index, source_flags);
}
#endif

/* Vtable dispatch tables: bind each native driver's operation functions
 * (drivers/include/drivers/native_channel_driver.h) into the
 * telemetry_driver_vtbl_t shape channel_manager.c/sensor_manager.c call
 * through. internal_temp/adc_voltage/gpio_input are read-only (no .write/
 * .configure/.self_test); gpio_output is the first writable native driver
 * (v0.5.1 T1). */
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
#if FIRMWARE_ENABLE_NATIVE_GPIO_OUTPUT_DRIVER
static const telemetry_driver_vtbl_t g_gpio_output_driver = {
    .start = native_gpio_output_start,
    .stop = native_gpio_output_stop,
    .write = native_gpio_output_write,
    .configure = native_gpio_output_configure,
    .self_test = native_gpio_output_self_test,
};
#endif
#if FIRMWARE_ENABLE_NATIVE_ADS8688_DRIVER
static const telemetry_driver_vtbl_t g_ads8688_driver = {
    .start = native_ads8688_start,
    .stop = native_ads8688_stop,
    .read = native_ads8688_read,
    .self_test = native_ads8688_self_test,
};
#endif
#if FIRMWARE_ENABLE_NATIVE_AD5422_DRIVER
static const telemetry_driver_vtbl_t g_ad5422_driver = {
    .start = native_ad5422_start,
    .stop = native_ad5422_stop,
    .write = native_ad5422_write,
    .self_test = native_ad5422_self_test,
};
#endif
#if FIRMWARE_ENABLE_NATIVE_MAX22190_DRIVER
static const telemetry_driver_vtbl_t g_max22190_driver = {
    .start = native_max22190_start,
    .stop = native_max22190_stop,
    .read = native_max22190_read,
    .self_test = native_max22190_self_test,
};
#endif
#if FIRMWARE_ENABLE_NATIVE_IPS2050_DRIVER
static const telemetry_driver_vtbl_t g_ips2050_driver = {
    .start = native_ips2050_start,
    .stop = native_ips2050_stop,
    .write = native_ips2050_write,
    .self_test = native_ips2050_self_test,
};
#endif

/* The compiled-in driver catalog. Order matches compile-time enablement of
 * FIRMWARE_ENABLE_NATIVE_*_DRIVER; driver_catalog_count()/_get_by_index()
 * index into this array directly, so it must never be reordered based on
 * runtime state. */
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
#if FIRMWARE_ENABLE_NATIVE_GPIO_OUTPUT_DRIVER
    {
        .driver_id = CONFIG_CHANNEL_DRIVER_GPIO_OUTPUT,
        .name = "gpio_output",
        .default_unit = "",
        .default_kind = TELEMETRY_CHANNEL_KIND_DISCRETE,
        .default_value_type = TELEMETRY_CHANNEL_VALUE_BOOLEAN,
        .default_flags = TELEMETRY_CHANNEL_FLAG_ENABLED | TELEMETRY_CHANNEL_FLAG_WRITABLE,
        .analog = false,
        .supported_value_types = TELEMETRY_CHANNEL_VALUE_TYPE_BIT(TELEMETRY_CHANNEL_VALUE_BOOLEAN),
        .capabilities = TELEMETRY_DRIVER_CAPABILITY_START | TELEMETRY_DRIVER_CAPABILITY_STOP |
                        TELEMETRY_DRIVER_CAPABILITY_WRITE | TELEMETRY_DRIVER_CAPABILITY_CONFIGURE |
                        TELEMETRY_DRIVER_CAPABILITY_SELF_TEST,
        .validate_source = validate_gpio_output,
        .driver = &g_gpio_output_driver,
    },
#endif
#if FIRMWARE_ENABLE_NATIVE_ADS8688_DRIVER
    {
        .driver_id = CONFIG_CHANNEL_DRIVER_ADS8688,
        .name = "ads8688",
        .default_unit = "V",
        .default_kind = TELEMETRY_CHANNEL_KIND_ANALOG,
        .default_value_type = TELEMETRY_CHANNEL_VALUE_FLOAT,
        .default_flags = TELEMETRY_CHANNEL_FLAG_ENABLED | TELEMETRY_CHANNEL_FLAG_HISTORIZED,
        .analog = true,
        .supported_value_types = TELEMETRY_CHANNEL_VALUE_TYPE_BIT(TELEMETRY_CHANNEL_VALUE_FLOAT) |
                                 TELEMETRY_CHANNEL_VALUE_TYPE_BIT(TELEMETRY_CHANNEL_VALUE_DOUBLE),
        .capabilities = TELEMETRY_DRIVER_CAPABILITY_START | TELEMETRY_DRIVER_CAPABILITY_STOP |
                        TELEMETRY_DRIVER_CAPABILITY_READ | TELEMETRY_DRIVER_CAPABILITY_SELF_TEST,
        .validate_source = validate_ads8688,
        .driver = &g_ads8688_driver,
    },
#endif
#if FIRMWARE_ENABLE_NATIVE_AD5422_DRIVER
    {
        .driver_id = CONFIG_CHANNEL_DRIVER_AD5422,
        .name = "ad5422",
        .default_unit = "V",
        .default_kind = TELEMETRY_CHANNEL_KIND_ANALOG,
        .default_value_type = TELEMETRY_CHANNEL_VALUE_FLOAT,
        .default_flags = TELEMETRY_CHANNEL_FLAG_ENABLED | TELEMETRY_CHANNEL_FLAG_WRITABLE,
        .analog = true,
        .supported_value_types = TELEMETRY_CHANNEL_VALUE_TYPE_BIT(TELEMETRY_CHANNEL_VALUE_FLOAT) |
                                 TELEMETRY_CHANNEL_VALUE_TYPE_BIT(TELEMETRY_CHANNEL_VALUE_DOUBLE),
        .capabilities = TELEMETRY_DRIVER_CAPABILITY_START | TELEMETRY_DRIVER_CAPABILITY_STOP |
                        TELEMETRY_DRIVER_CAPABILITY_WRITE | TELEMETRY_DRIVER_CAPABILITY_SELF_TEST,
        .validate_source = validate_ad5422,
        .driver = &g_ad5422_driver,
    },
#endif
#if FIRMWARE_ENABLE_NATIVE_MAX22190_DRIVER
    {
        .driver_id = CONFIG_CHANNEL_DRIVER_MAX22190,
        .name = "max22190",
        .default_unit = "",
        .default_kind = TELEMETRY_CHANNEL_KIND_DISCRETE,
        .default_value_type = TELEMETRY_CHANNEL_VALUE_BOOLEAN,
        .default_flags = TELEMETRY_CHANNEL_FLAG_ENABLED,
        .analog = false,
        .supported_value_types = TELEMETRY_CHANNEL_VALUE_TYPE_BIT(TELEMETRY_CHANNEL_VALUE_BOOLEAN),
        .capabilities = TELEMETRY_DRIVER_CAPABILITY_START | TELEMETRY_DRIVER_CAPABILITY_STOP |
                        TELEMETRY_DRIVER_CAPABILITY_READ | TELEMETRY_DRIVER_CAPABILITY_SELF_TEST,
        .validate_source = validate_max22190,
        .driver = &g_max22190_driver,
    },
#endif
#if FIRMWARE_ENABLE_NATIVE_IPS2050_DRIVER
    {
        .driver_id = CONFIG_CHANNEL_DRIVER_IPS2050,
        .name = "ips2050",
        .default_unit = "",
        .default_kind = TELEMETRY_CHANNEL_KIND_DISCRETE,
        .default_value_type = TELEMETRY_CHANNEL_VALUE_BOOLEAN,
        .default_flags = TELEMETRY_CHANNEL_FLAG_ENABLED | TELEMETRY_CHANNEL_FLAG_WRITABLE,
        .analog = false,
        .supported_value_types = TELEMETRY_CHANNEL_VALUE_TYPE_BIT(TELEMETRY_CHANNEL_VALUE_BOOLEAN),
        .capabilities = TELEMETRY_DRIVER_CAPABILITY_START | TELEMETRY_DRIVER_CAPABILITY_STOP |
                        TELEMETRY_DRIVER_CAPABILITY_WRITE | TELEMETRY_DRIVER_CAPABILITY_SELF_TEST,
        .validate_source = validate_ips2050,
        .driver = &g_ips2050_driver,
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
    if (driver_id == CONFIG_CHANNEL_DRIVER_ADC_VOLTAGE || driver_id == CONFIG_CHANNEL_DRIVER_ADS8688 ||
        driver_id == CONFIG_CHANNEL_DRIVER_AD5422 || driver_id == CONFIG_CHANNEL_DRIVER_MAX22190)
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
    else if (driver_id == CONFIG_CHANNEL_DRIVER_GPIO_INPUT || driver_id == CONFIG_CHANNEL_DRIVER_GPIO_OUTPUT)
    {
        out_claims[0].type = DRIVER_CATALOG_RESOURCE_GPIO;
        out_claims[0].index = source_index;
        out_claims[0].shared = false;
    }
    else if (driver_id == CONFIG_CHANNEL_DRIVER_ADS8688)
    {
        /* source_index selects one of the chip's 8 mux inputs -- exclusive,
         * so two configured channels cannot both bind AI-n. Only one
         * physical ADS8688 chip is in scope for this release, so a single
         * shared SPI-peripheral claim at index 0 is sufficient; see
         * docs/releases/v0.5.1.md T2 for the multi-chip caveat this leaves
         * unaddressed. */
        out_claims[0].type = DRIVER_CATALOG_RESOURCE_SPI_CHIP_SELECT;
        out_claims[0].index = source_index;
        out_claims[0].shared = false;
        out_claims[1] = (driver_catalog_resource_claim_t){
            .type = DRIVER_CATALOG_RESOURCE_SPI_PERIPHERAL,
            .index = 0u,
            .shared = true,
        };
    }
    else if (driver_id == CONFIG_CHANNEL_DRIVER_AD5422)
    {
        /* source_index selects AO0/AO1 (a daisy-chain position, not a
         * chip-select pin -- both AD5422 devices share one LATCH line).
         * Shares the same SPI_PERIPHERAL index as CONFIG_CHANNEL_DRIVER_ADS8688:
         * both chips sit on the industrial board's one shared physical SPI
         * bus (HARDWARE_MVP.md), so the shared claim correctly does not
         * conflict between them. */
        out_claims[0].type = DRIVER_CATALOG_RESOURCE_AD5422_CHANNEL;
        out_claims[0].index = source_index;
        out_claims[0].shared = false;
        out_claims[1] = (driver_catalog_resource_claim_t){
            .type = DRIVER_CATALOG_RESOURCE_SPI_PERIPHERAL,
            .index = 0u,
            .shared = true,
        };
    }
    else if (driver_id == CONFIG_CHANNEL_DRIVER_MAX22190)
    {
        /* source_index selects DI0-DI3, one of this board's four wired
         * physical input lines on the single MAX22190 chip in scope for this
         * release -- exclusive, so two configured channels cannot both bind
         * the same DI line. Shares the same SPI_PERIPHERAL index as ADS8688/
         * AD5422: all three chips sit on the industrial board's one shared
         * physical SPI bus (HARDWARE_MVP.md), so the shared claim correctly
         * does not conflict between them. */
        out_claims[0].type = DRIVER_CATALOG_RESOURCE_MAX22190_CHANNEL;
        out_claims[0].index = source_index;
        out_claims[0].shared = false;
        out_claims[1] = (driver_catalog_resource_claim_t){
            .type = DRIVER_CATALOG_RESOURCE_SPI_PERIPHERAL,
            .index = 0u,
            .shared = true,
        };
    }
    else if (driver_id == CONFIG_CHANNEL_DRIVER_IPS2050)
    {
        /* source_index selects DO0/DO1, one of this board's two protected
         * high-side output channels -- exclusive, so two configured
         * channels cannot both bind the same DO. A plain GPIO frontend
         * (HARDWARE_MVP.md 3.4.4), not a shared-bus chip like ADS8688/
         * AD5422/MAX22190, so no second shared claim is needed. */
        out_claims[0].type = DRIVER_CATALOG_RESOURCE_IPS2050_CHANNEL;
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
