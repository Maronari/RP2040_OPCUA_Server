#include "drivers/native_channel_driver.h"
#include "services/driver_catalog.h"

#include <assert.h>
#include <string.h>

bool native_internal_temperature_source_is_valid(uint8_t index, uint8_t flags) { return index == 0u && flags == 0u; }
bool native_adc_voltage_source_is_valid(uint8_t index, uint8_t flags) { return index <= 3u && flags == 0u; }
bool native_gpio_input_source_is_valid(uint8_t index, uint8_t flags) { return index < 48u && flags <= 7u && (flags & 6u) != 6u; }
bool native_gpio_output_source_is_valid(uint8_t index, uint8_t flags) { return index < 48u && flags <= 3u; }

telemetry_driver_result_t native_internal_temperature_start(telemetry_channel_t *c) { (void)c; return TELEMETRY_DRIVER_RESULT_OK; }
telemetry_driver_result_t native_internal_temperature_stop(telemetry_channel_t *c) { (void)c; return TELEMETRY_DRIVER_RESULT_OK; }
telemetry_driver_result_t native_internal_temperature_read(telemetry_channel_t *c, uint32_t n) { (void)c; (void)n; return TELEMETRY_DRIVER_RESULT_OK; }
telemetry_driver_result_t native_adc_voltage_start(telemetry_channel_t *c) { (void)c; return TELEMETRY_DRIVER_RESULT_OK; }
telemetry_driver_result_t native_adc_voltage_stop(telemetry_channel_t *c) { (void)c; return TELEMETRY_DRIVER_RESULT_OK; }
telemetry_driver_result_t native_adc_voltage_read(telemetry_channel_t *c, uint32_t n) { (void)c; (void)n; return TELEMETRY_DRIVER_RESULT_OK; }
telemetry_driver_result_t native_gpio_input_start(telemetry_channel_t *c) { (void)c; return TELEMETRY_DRIVER_RESULT_OK; }
telemetry_driver_result_t native_gpio_input_stop(telemetry_channel_t *c) { (void)c; return TELEMETRY_DRIVER_RESULT_OK; }
telemetry_driver_result_t native_gpio_input_read(telemetry_channel_t *c, uint32_t n) { (void)c; (void)n; return TELEMETRY_DRIVER_RESULT_OK; }
telemetry_driver_result_t native_gpio_output_start(telemetry_channel_t *c) { (void)c; return TELEMETRY_DRIVER_RESULT_OK; }
telemetry_driver_result_t native_gpio_output_stop(telemetry_channel_t *c) { (void)c; return TELEMETRY_DRIVER_RESULT_OK; }
telemetry_driver_result_t native_gpio_output_write(telemetry_channel_t *c, const telemetry_channel_value_t *v) { (void)c; (void)v; return TELEMETRY_DRIVER_RESULT_OK; }
telemetry_driver_result_t native_gpio_output_configure(telemetry_channel_t *c, const void *cfg) { (void)c; (void)cfg; return TELEMETRY_DRIVER_RESULT_OK; }
telemetry_driver_result_t native_gpio_output_self_test(telemetry_channel_t *c) { (void)c; return TELEMETRY_DRIVER_RESULT_OK; }

int main(void)
{
    const driver_catalog_entry_t *adc = driver_catalog_get_by_id(CONFIG_CHANNEL_DRIVER_ADC_VOLTAGE);
    const driver_catalog_entry_t *gpio = driver_catalog_get_by_id(CONFIG_CHANNEL_DRIVER_GPIO_INPUT);
    const driver_catalog_entry_t *gpio_output = driver_catalog_get_by_id(CONFIG_CHANNEL_DRIVER_GPIO_OUTPUT);
    driver_catalog_resource_claim_t claims[4];
    driver_catalog_resource_claim_t temp_claims[DRIVER_CATALOG_MAX_RESOURCE_CLAIMS_PER_CHANNEL];
    size_t claim_count = 0u;
    size_t temp_claim_count = 0u;

    assert(adc != NULL && gpio != NULL);
    assert(driver_catalog_supports_value_type(adc, TELEMETRY_CHANNEL_VALUE_FLOAT));
    assert(driver_catalog_supports_value_type(adc, TELEMETRY_CHANNEL_VALUE_DOUBLE));
    assert(!driver_catalog_supports_value_type(adc, TELEMETRY_CHANNEL_VALUE_INT32));
    assert(driver_catalog_supports_value_type(gpio, TELEMETRY_CHANNEL_VALUE_BOOLEAN));
    assert(!driver_catalog_supports_value_type(gpio, TELEMETRY_CHANNEL_VALUE_STRING));
    assert(!driver_catalog_get_resource_claims(CONFIG_CHANNEL_DRIVER_ADC_VOLTAGE, 1u, 0u,
                                                claims, 1u, &claim_count));
    assert(claim_count == 2u);
    assert(driver_catalog_get_resource_claims(CONFIG_CHANNEL_DRIVER_ADC_VOLTAGE, 1u, 0u,
                                               claims, 2u, &claim_count));
    assert(claim_count == 2u && claims[0].type == DRIVER_CATALOG_RESOURCE_ADC_INPUT &&
           !claims[0].shared && claims[1].type == DRIVER_CATALOG_RESOURCE_ADC_PERIPHERAL &&
           claims[1].shared);
    assert(driver_catalog_get_resource_claims(CONFIG_CHANNEL_DRIVER_INTERNAL_TEMP, 0u, 0u,
                                               temp_claims, 2u, &temp_claim_count));
    assert(temp_claim_count == 1u && temp_claims[0].type == DRIVER_CATALOG_RESOURCE_ADC_PERIPHERAL &&
           temp_claims[0].shared);
    memcpy(&claims[2], temp_claims, sizeof(temp_claims[0]));
    assert(driver_catalog_claims_are_compatible(claims, 3u));
    assert(driver_catalog_get_resource_claims(CONFIG_CHANNEL_DRIVER_ADC_VOLTAGE, 1u, 0u,
                                               &claims[2], 2u, &claim_count));
    assert(!driver_catalog_claims_are_compatible(claims, 4u));
    assert(driver_catalog_get_resource_claims(CONFIG_CHANNEL_DRIVER_GPIO_INPUT, 2u,
                                               CONFIG_CHANNEL_SOURCE_FLAG_PULLUP, &claims[2], 2u, &claim_count));
    assert(claim_count == 1u && driver_catalog_claims_are_compatible(claims, 3u));
    assert(!driver_catalog_get_resource_claims(CONFIG_CHANNEL_DRIVER_ADC_VOLTAGE, 4u, 0u,
                                                claims, 2u, &claim_count));
    assert(!driver_catalog_get_resource_claims(CONFIG_CHANNEL_DRIVER_GPIO_INPUT, 1u,
                                                CONFIG_CHANNEL_SOURCE_FLAG_PULLUP | CONFIG_CHANNEL_SOURCE_FLAG_PULLDOWN,
                                                claims, 2u, &claim_count));

    /* v0.5.1 T1: gpio_output is the first writable native driver. */
    assert(gpio_output != NULL);
    assert((gpio_output->capabilities & TELEMETRY_DRIVER_CAPABILITY_WRITE) != 0u);
    assert((gpio_output->capabilities & TELEMETRY_DRIVER_CAPABILITY_CONFIGURE) != 0u);
    assert((gpio_output->capabilities & TELEMETRY_DRIVER_CAPABILITY_SELF_TEST) != 0u);
    assert((gpio_output->default_flags & TELEMETRY_CHANNEL_FLAG_WRITABLE) != 0u);
    assert(gpio_output->driver->write != NULL && gpio_output->driver->configure != NULL &&
           gpio_output->driver->self_test != NULL);
    assert(driver_catalog_supports_value_type(gpio_output, TELEMETRY_CHANNEL_VALUE_BOOLEAN));
    assert(!driver_catalog_supports_value_type(gpio_output, TELEMETRY_CHANNEL_VALUE_FLOAT));
    assert(driver_catalog_get_resource_claims(CONFIG_CHANNEL_DRIVER_GPIO_OUTPUT, 5u, 0u, claims, 2u, &claim_count));
    assert(claim_count == 1u && claims[0].type == DRIVER_CATALOG_RESOURCE_GPIO && claims[0].index == 5u &&
           !claims[0].shared);
    /* A GPIO output and a GPIO input claiming the same pin index conflict,
     * proving the shared DRIVER_CATALOG_RESOURCE_GPIO claim type catches a
     * cross-driver conflict, not just a same-driver one. */
    assert(driver_catalog_get_resource_claims(CONFIG_CHANNEL_DRIVER_GPIO_INPUT, 5u, 0u, &claims[1], 2u,
                                               &claim_count));
    assert(!driver_catalog_claims_are_compatible(claims, 2u));
    assert(!driver_catalog_get_resource_claims(CONFIG_CHANNEL_DRIVER_GPIO_OUTPUT, 1u, 4u, claims, 2u, &claim_count));
    return 0;
}
