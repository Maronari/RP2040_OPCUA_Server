#include "drivers/ad5422_driver.h"
#include "drivers/ads8688_driver.h"
#include "drivers/ips2050_driver.h"
#include "drivers/max22190_driver.h"
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
bool native_ads8688_source_is_valid(uint8_t index, uint8_t flags)
{
    return index <= 7u && (flags == 0x0u || flags == 0x1u || flags == 0x2u || flags == 0x5u || flags == 0x6u);
}
telemetry_driver_result_t native_ads8688_start(telemetry_channel_t *c) { (void)c; return TELEMETRY_DRIVER_RESULT_OK; }
telemetry_driver_result_t native_ads8688_stop(telemetry_channel_t *c) { (void)c; return TELEMETRY_DRIVER_RESULT_OK; }
telemetry_driver_result_t native_ads8688_read(telemetry_channel_t *c, uint32_t n) { (void)c; (void)n; return TELEMETRY_DRIVER_RESULT_OK; }
telemetry_driver_result_t native_ads8688_self_test(telemetry_channel_t *c) { (void)c; return TELEMETRY_DRIVER_RESULT_OK; }
bool native_ad5422_source_is_valid(uint8_t index, uint8_t flags)
{
    return index <= 1u && flags != 0x4u && flags <= 0x7u;
}
telemetry_driver_result_t native_ad5422_start(telemetry_channel_t *c) { (void)c; return TELEMETRY_DRIVER_RESULT_OK; }
telemetry_driver_result_t native_ad5422_stop(telemetry_channel_t *c) { (void)c; return TELEMETRY_DRIVER_RESULT_OK; }
telemetry_driver_result_t native_ad5422_write(telemetry_channel_t *c, const telemetry_channel_value_t *v) { (void)c; (void)v; return TELEMETRY_DRIVER_RESULT_OK; }
telemetry_driver_result_t native_ad5422_self_test(telemetry_channel_t *c) { (void)c; return TELEMETRY_DRIVER_RESULT_OK; }
bool native_max22190_source_is_valid(uint8_t index, uint8_t flags)
{
    return index <= 3u && (flags & (uint8_t)~0x0Fu) == 0u;
}
telemetry_driver_result_t native_max22190_start(telemetry_channel_t *c) { (void)c; return TELEMETRY_DRIVER_RESULT_OK; }
telemetry_driver_result_t native_max22190_stop(telemetry_channel_t *c) { (void)c; return TELEMETRY_DRIVER_RESULT_OK; }
telemetry_driver_result_t native_max22190_read(telemetry_channel_t *c, uint32_t n) { (void)c; (void)n; return TELEMETRY_DRIVER_RESULT_OK; }
telemetry_driver_result_t native_max22190_self_test(telemetry_channel_t *c) { (void)c; return TELEMETRY_DRIVER_RESULT_OK; }
bool native_ips2050_source_is_valid(uint8_t index, uint8_t flags) { return index <= 1u && flags == 0u; }
telemetry_driver_result_t native_ips2050_start(telemetry_channel_t *c) { (void)c; return TELEMETRY_DRIVER_RESULT_OK; }
telemetry_driver_result_t native_ips2050_stop(telemetry_channel_t *c) { (void)c; return TELEMETRY_DRIVER_RESULT_OK; }
telemetry_driver_result_t native_ips2050_write(telemetry_channel_t *c, const telemetry_channel_value_t *v) { (void)c; (void)v; return TELEMETRY_DRIVER_RESULT_OK; }
telemetry_driver_result_t native_ips2050_self_test(telemetry_channel_t *c) { (void)c; return TELEMETRY_DRIVER_RESULT_OK; }

int main(void)
{
    const driver_catalog_entry_t *adc = driver_catalog_get_by_id(CONFIG_CHANNEL_DRIVER_ADC_VOLTAGE);
    const driver_catalog_entry_t *gpio = driver_catalog_get_by_id(CONFIG_CHANNEL_DRIVER_GPIO_INPUT);
    const driver_catalog_entry_t *gpio_output = driver_catalog_get_by_id(CONFIG_CHANNEL_DRIVER_GPIO_OUTPUT);
    const driver_catalog_entry_t *ads8688 = driver_catalog_get_by_id(CONFIG_CHANNEL_DRIVER_ADS8688);
    const driver_catalog_entry_t *ad5422 = driver_catalog_get_by_id(CONFIG_CHANNEL_DRIVER_AD5422);
    const driver_catalog_entry_t *max22190 = driver_catalog_get_by_id(CONFIG_CHANNEL_DRIVER_MAX22190);
    const driver_catalog_entry_t *ips2050 = driver_catalog_get_by_id(CONFIG_CHANNEL_DRIVER_IPS2050);
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

    /* v0.5.1 T2: ads8688 is the first driver claiming SPI resources. */
    assert(ads8688 != NULL);
    assert((ads8688->capabilities & TELEMETRY_DRIVER_CAPABILITY_READ) != 0u);
    assert((ads8688->capabilities & TELEMETRY_DRIVER_CAPABILITY_SELF_TEST) != 0u);
    assert((ads8688->capabilities & TELEMETRY_DRIVER_CAPABILITY_WRITE) == 0u);
    assert(ads8688->driver->read != NULL && ads8688->driver->self_test != NULL && ads8688->driver->write == NULL);
    assert(driver_catalog_supports_value_type(ads8688, TELEMETRY_CHANNEL_VALUE_FLOAT));
    assert(driver_catalog_supports_value_type(ads8688, TELEMETRY_CHANNEL_VALUE_DOUBLE));
    assert(!driver_catalog_supports_value_type(ads8688, TELEMETRY_CHANNEL_VALUE_BOOLEAN));
    assert(!driver_catalog_get_resource_claims(CONFIG_CHANNEL_DRIVER_ADS8688, 3u, 0x1u, claims, 1u, &claim_count));
    assert(claim_count == 2u);
    assert(driver_catalog_get_resource_claims(CONFIG_CHANNEL_DRIVER_ADS8688, 3u, 0x1u, claims, 2u, &claim_count));
    assert(claim_count == 2u && claims[0].type == DRIVER_CATALOG_RESOURCE_SPI_CHIP_SELECT && claims[0].index == 3u &&
           !claims[0].shared && claims[1].type == DRIVER_CATALOG_RESOURCE_SPI_PERIPHERAL && claims[1].index == 0u &&
           claims[1].shared);
    /* Two ads8688 channels on different mux inputs share the SPI peripheral
     * claim but do not conflict; the same mux input claimed twice does. */
    assert(driver_catalog_get_resource_claims(CONFIG_CHANNEL_DRIVER_ADS8688, 5u, 0x0u, &claims[2], 2u, &claim_count));
    assert(driver_catalog_claims_are_compatible(claims, 4u));
    assert(driver_catalog_get_resource_claims(CONFIG_CHANNEL_DRIVER_ADS8688, 3u, 0x2u, &claims[2], 2u, &claim_count));
    assert(!driver_catalog_claims_are_compatible(claims, 4u));
    assert(!driver_catalog_get_resource_claims(CONFIG_CHANNEL_DRIVER_ADS8688, 8u, 0x0u, claims, 2u, &claim_count));
    assert(!driver_catalog_get_resource_claims(CONFIG_CHANNEL_DRIVER_ADS8688, 0u, 0x3u, claims, 2u, &claim_count));

    /* v0.5.1 Phase 1: ad5422 shares the same physical SPI bus as ads8688
     * (both claim DRIVER_CATALOG_RESOURCE_SPI_PERIPHERAL index 0, shared)
     * but has no chip-select pin of its own -- its exclusive claim is a
     * distinct resource type (daisy-chain position), so it must not
     * false-conflict with an ads8688 channel bound to the same numeric
     * mux index. */
    assert(ad5422 != NULL);
    assert((ad5422->capabilities & TELEMETRY_DRIVER_CAPABILITY_WRITE) != 0u);
    assert((ad5422->capabilities & TELEMETRY_DRIVER_CAPABILITY_SELF_TEST) != 0u);
    assert((ad5422->capabilities & TELEMETRY_DRIVER_CAPABILITY_READ) == 0u);
    assert(ad5422->driver->write != NULL && ad5422->driver->self_test != NULL && ad5422->driver->read == NULL);
    assert((ad5422->default_flags & TELEMETRY_CHANNEL_FLAG_WRITABLE) != 0u);
    assert(driver_catalog_get_resource_claims(CONFIG_CHANNEL_DRIVER_AD5422, 0u, 0x1u, claims, 2u, &claim_count));
    assert(claim_count == 2u && claims[0].type == DRIVER_CATALOG_RESOURCE_AD5422_CHANNEL && claims[0].index == 0u &&
           !claims[0].shared && claims[1].type == DRIVER_CATALOG_RESOURCE_SPI_PERIPHERAL && claims[1].index == 0u &&
           claims[1].shared);
    /* An ads8688 channel on mux index 0 and an ad5422 channel on AO0 (also
     * index 0) must NOT conflict -- different claim types, same shared bus. */
    assert(driver_catalog_get_resource_claims(CONFIG_CHANNEL_DRIVER_ADS8688, 0u, 0x0u, &claims[2], 2u, &claim_count));
    assert(driver_catalog_claims_are_compatible(claims, 4u));
    /* Two ad5422 channels both claiming AO0 do conflict. */
    assert(driver_catalog_get_resource_claims(CONFIG_CHANNEL_DRIVER_AD5422, 0u, 0x2u, &claims[2], 2u, &claim_count));
    assert(!driver_catalog_claims_are_compatible(claims, 4u));
    assert(!driver_catalog_get_resource_claims(CONFIG_CHANNEL_DRIVER_AD5422, 2u, 0x0u, claims, 2u, &claim_count));
    assert(!driver_catalog_get_resource_claims(CONFIG_CHANNEL_DRIVER_AD5422, 0u, 0x4u, claims, 2u, &claim_count));

    /* v0.5.1 Phase 1: max22190 (DI0-DI3) is a third chip on the same shared
     * SPI bus as ads8688/ad5422 -- read-only, no WRITE capability. */
    assert(max22190 != NULL);
    assert((max22190->capabilities & TELEMETRY_DRIVER_CAPABILITY_READ) != 0u);
    assert((max22190->capabilities & TELEMETRY_DRIVER_CAPABILITY_SELF_TEST) != 0u);
    assert((max22190->capabilities & TELEMETRY_DRIVER_CAPABILITY_WRITE) == 0u);
    assert(max22190->driver->read != NULL && max22190->driver->self_test != NULL &&
           max22190->driver->write == NULL);
    assert(driver_catalog_supports_value_type(max22190, TELEMETRY_CHANNEL_VALUE_BOOLEAN));
    assert(!driver_catalog_supports_value_type(max22190, TELEMETRY_CHANNEL_VALUE_FLOAT));
    assert(driver_catalog_get_resource_claims(CONFIG_CHANNEL_DRIVER_MAX22190, 0u, 0x03u, claims, 2u, &claim_count));
    assert(claim_count == 2u && claims[0].type == DRIVER_CATALOG_RESOURCE_MAX22190_CHANNEL && claims[0].index == 0u &&
           !claims[0].shared && claims[1].type == DRIVER_CATALOG_RESOURCE_SPI_PERIPHERAL && claims[1].index == 0u &&
           claims[1].shared);
    /* An ads8688 channel on mux index 0 and a max22190 channel on DI0 (also
     * index 0) must NOT conflict -- distinct claim types, same shared bus. */
    assert(driver_catalog_get_resource_claims(CONFIG_CHANNEL_DRIVER_ADS8688, 0u, 0x0u, &claims[2], 2u, &claim_count));
    assert(driver_catalog_claims_are_compatible(claims, 4u));
    /* An ad5422 channel on AO0 (also index 0) must likewise not conflict. */
    assert(driver_catalog_get_resource_claims(CONFIG_CHANNEL_DRIVER_AD5422, 0u, 0x1u, &claims[2], 2u, &claim_count));
    assert(driver_catalog_claims_are_compatible(claims, 4u));
    /* Two max22190 channels both claiming DI0 do conflict. */
    assert(driver_catalog_get_resource_claims(CONFIG_CHANNEL_DRIVER_MAX22190, 0u, 0x00u, &claims[2], 2u, &claim_count));
    assert(!driver_catalog_claims_are_compatible(claims, 4u));
    assert(!driver_catalog_get_resource_claims(CONFIG_CHANNEL_DRIVER_MAX22190, 4u, 0x0u, claims, 2u, &claim_count));
    assert(!driver_catalog_get_resource_claims(CONFIG_CHANNEL_DRIVER_MAX22190, 0u, 0x10u, claims, 2u, &claim_count));

    /* v0.5.1 Phase 1: ips2050 (DO0-DO1) is a plain GPIO frontend, not a
     * shared-bus chip -- writable, no READ capability, and a single
     * exclusive claim per channel (no accompanying shared claim). */
    assert(ips2050 != NULL);
    assert((ips2050->capabilities & TELEMETRY_DRIVER_CAPABILITY_WRITE) != 0u);
    assert((ips2050->capabilities & TELEMETRY_DRIVER_CAPABILITY_SELF_TEST) != 0u);
    assert((ips2050->capabilities & TELEMETRY_DRIVER_CAPABILITY_READ) == 0u);
    assert(ips2050->driver->write != NULL && ips2050->driver->self_test != NULL && ips2050->driver->read == NULL);
    assert((ips2050->default_flags & TELEMETRY_CHANNEL_FLAG_WRITABLE) != 0u);
    assert(driver_catalog_supports_value_type(ips2050, TELEMETRY_CHANNEL_VALUE_BOOLEAN));
    assert(!driver_catalog_supports_value_type(ips2050, TELEMETRY_CHANNEL_VALUE_FLOAT));
    assert(driver_catalog_get_resource_claims(CONFIG_CHANNEL_DRIVER_IPS2050, 0u, 0u, claims, 2u, &claim_count));
    assert(claim_count == 1u && claims[0].type == DRIVER_CATALOG_RESOURCE_IPS2050_CHANNEL && claims[0].index == 0u &&
           !claims[0].shared);
    /* Two ips2050 channels both claiming DO0 do conflict. */
    assert(driver_catalog_get_resource_claims(CONFIG_CHANNEL_DRIVER_IPS2050, 0u, 0u, &claims[1], 2u, &claim_count));
    assert(!driver_catalog_claims_are_compatible(claims, 2u));
    /* DO0 and DO1 do not conflict with each other. */
    assert(driver_catalog_get_resource_claims(CONFIG_CHANNEL_DRIVER_IPS2050, 1u, 0u, &claims[1], 2u, &claim_count));
    assert(driver_catalog_claims_are_compatible(claims, 2u));
    assert(!driver_catalog_get_resource_claims(CONFIG_CHANNEL_DRIVER_IPS2050, 2u, 0u, claims, 2u, &claim_count));
    assert(!driver_catalog_get_resource_claims(CONFIG_CHANNEL_DRIVER_IPS2050, 0u, 1u, claims, 2u, &claim_count));
    return 0;
}
