/**
 * @file
 * @brief Host tests for the real drivers/src/gpio_output_channel_driver.c
 *        (v0.5.1 T1 pilot), against the stateful hardware/gpio.h stub in
 *        this directory -- proves actual pad direction/level/pull
 *        sequencing, not just that the vtable dispatches.
 */
#include "drivers/native_channel_driver.h"

#include "hardware/gpio.h"

#include <assert.h>

#define TEST_PIN 5u
#define NATIVE_GPIO_OUTPUT_SOURCE_INVERT (1u << 0u)
#define NATIVE_GPIO_OUTPUT_SOURCE_OPEN_DRAIN (1u << 1u)

static telemetry_channel_t make_channel(native_channel_source_t *source)
{
    telemetry_channel_t channel = {0};
    channel.metadata.value_type = TELEMETRY_CHANNEL_VALUE_BOOLEAN;
    channel.driver_context = source;
    return channel;
}

static void test_source_validation(void)
{
    assert(native_gpio_output_source_is_valid(0u, 0u));
    assert(native_gpio_output_source_is_valid(NUM_BANK0_GPIOS - 1u,
                                              NATIVE_GPIO_OUTPUT_SOURCE_INVERT | NATIVE_GPIO_OUTPUT_SOURCE_OPEN_DRAIN));
    assert(!native_gpio_output_source_is_valid(NUM_BANK0_GPIOS, 0u));
    assert(!native_gpio_output_source_is_valid(0u, 0x04u));
}

static void test_push_pull_write_and_self_test(void)
{
    native_channel_source_t source = {.source_index = TEST_PIN, .source_flags = 0u};
    telemetry_channel_t channel = make_channel(&source);
    telemetry_channel_value_t on = {.boolean = true};
    telemetry_channel_value_t off = {.boolean = false};

    gpio_stub_reset();
    assert(native_gpio_output_start(&channel) == TELEMETRY_DRIVER_RESULT_OK);
    assert(gpio_get_dir(TEST_PIN) == GPIO_OUT);
    assert(g_gpio_stub_pins[TEST_PIN].level == false);
    assert(native_gpio_output_self_test(&channel) == TELEMETRY_DRIVER_RESULT_OK);

    assert(native_gpio_output_write(&channel, &on) == TELEMETRY_DRIVER_RESULT_OK);
    assert(g_gpio_stub_pins[TEST_PIN].level == true);
    assert(channel.runtime.value.boolean == true);
    assert(native_gpio_output_self_test(&channel) == TELEMETRY_DRIVER_RESULT_OK);

    /* A pad state that no longer matches the last commanded value must be
     * caught -- proves self_test is a real comparison, not a tautology. */
    g_gpio_stub_pins[TEST_PIN].level = false;
    assert(native_gpio_output_self_test(&channel) == TELEMETRY_DRIVER_RESULT_HARDWARE_FAILURE);

    assert(native_gpio_output_write(&channel, &off) == TELEMETRY_DRIVER_RESULT_OK);
    assert(g_gpio_stub_pins[TEST_PIN].level == false);
    assert(native_gpio_output_stop(&channel) == TELEMETRY_DRIVER_RESULT_OK);
}

static void test_invert(void)
{
    native_channel_source_t source = {.source_index = TEST_PIN, .source_flags = NATIVE_GPIO_OUTPUT_SOURCE_INVERT};
    telemetry_channel_t channel = make_channel(&source);
    telemetry_channel_value_t on = {.boolean = true};

    gpio_stub_reset();
    assert(native_gpio_output_start(&channel) == TELEMETRY_DRIVER_RESULT_OK);
    /* Released (logical false) with invert drives the pad high. */
    assert(g_gpio_stub_pins[TEST_PIN].level == true);

    assert(native_gpio_output_write(&channel, &on) == TELEMETRY_DRIVER_RESULT_OK);
    /* Logical true with invert drives the pad low. */
    assert(g_gpio_stub_pins[TEST_PIN].level == false);
    assert(channel.runtime.value.boolean == true);
    assert(native_gpio_output_self_test(&channel) == TELEMETRY_DRIVER_RESULT_OK);
}

static void test_open_drain_assert_release(void)
{
    native_channel_source_t source = {.source_index = TEST_PIN, .source_flags = NATIVE_GPIO_OUTPUT_SOURCE_OPEN_DRAIN};
    telemetry_channel_t channel = make_channel(&source);
    telemetry_channel_value_t asserted = {.boolean = true};
    telemetry_channel_value_t released = {.boolean = false};

    gpio_stub_reset();
    assert(native_gpio_output_start(&channel) == TELEMETRY_DRIVER_RESULT_OK);
    /* Released default: high-impedance, weak pull-up enabled. */
    assert(gpio_get_dir(TEST_PIN) == GPIO_IN);
    assert(g_gpio_stub_pins[TEST_PIN].pull_up == true);
    assert(native_gpio_output_self_test(&channel) == TELEMETRY_DRIVER_RESULT_OK);

    assert(native_gpio_output_write(&channel, &asserted) == TELEMETRY_DRIVER_RESULT_OK);
    assert(gpio_get_dir(TEST_PIN) == GPIO_OUT);
    assert(g_gpio_stub_pins[TEST_PIN].level == false);
    assert(native_gpio_output_self_test(&channel) == TELEMETRY_DRIVER_RESULT_OK);

    assert(native_gpio_output_write(&channel, &released) == TELEMETRY_DRIVER_RESULT_OK);
    assert(gpio_get_dir(TEST_PIN) == GPIO_IN);
    assert(native_gpio_output_self_test(&channel) == TELEMETRY_DRIVER_RESULT_OK);

    /* Direction mismatch (e.g. a fault forcing the pad back to output)
     * must be caught for open-drain too. */
    gpio_set_dir(TEST_PIN, true);
    assert(native_gpio_output_self_test(&channel) == TELEMETRY_DRIVER_RESULT_HARDWARE_FAILURE);
}

static void test_configure(void)
{
    native_channel_source_t source = {.source_index = TEST_PIN, .source_flags = 0u};
    telemetry_channel_t channel = make_channel(&source);
    native_gpio_output_config_t config = {.safe_level = true};

    gpio_stub_reset();
    assert(native_gpio_output_start(&channel) == TELEMETRY_DRIVER_RESULT_OK);
    /* Every real caller (sensor_manager.c activation/restart) invokes
     * configure() with a NULL payload -- that must apply the released
     * default, not fail. This is the exact bug found during T1's live
     * hardware verification: an earlier version of this driver required a
     * non-NULL config and returned INVALID_ARGUMENT here, which made every
     * real channel activation fail. */
    assert(native_gpio_output_write(&channel, &(telemetry_channel_value_t){.boolean = true}) ==
           TELEMETRY_DRIVER_RESULT_OK);
    assert(native_gpio_output_configure(&channel, NULL) == TELEMETRY_DRIVER_RESULT_OK);
    assert(g_gpio_stub_pins[TEST_PIN].level == false);
    assert(channel.runtime.value.boolean == false);

    assert(native_gpio_output_configure(&channel, &config) == TELEMETRY_DRIVER_RESULT_OK);
    assert(g_gpio_stub_pins[TEST_PIN].level == true);
    assert(channel.runtime.value.boolean == true);
}

static void test_invalid_arguments(void)
{
    native_channel_source_t bad_source = {.source_index = (uint8_t)NUM_BANK0_GPIOS, .source_flags = 0u};
    telemetry_channel_t bad_channel = make_channel(&bad_source);
    native_channel_source_t good_source = {.source_index = TEST_PIN, .source_flags = 0u};
    telemetry_channel_t channel = make_channel(&good_source);
    telemetry_channel_value_t on = {.boolean = true};

    gpio_stub_reset();
    assert(native_gpio_output_start(&bad_channel) == TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT);
    assert(native_gpio_output_write(&bad_channel, &on) == TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT);
    assert(native_gpio_output_self_test(&bad_channel) == TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT);
    assert(native_gpio_output_write(NULL, &on) == TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT);

    assert(native_gpio_output_start(&channel) == TELEMETRY_DRIVER_RESULT_OK);
    assert(native_gpio_output_write(&channel, NULL) == TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT);
    channel.metadata.value_type = TELEMETRY_CHANNEL_VALUE_FLOAT;
    assert(native_gpio_output_write(&channel, &on) == TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT);
}

int main(void)
{
    test_source_validation();
    test_push_pull_write_and_self_test();
    test_invert();
    test_open_drain_assert_release();
    test_configure();
    test_invalid_arguments();
    return 0;
}
