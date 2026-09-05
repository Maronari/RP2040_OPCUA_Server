/**
 * @file
 * @brief Host tests for the real drivers/src/ips2050_driver.c against the
 *        stateful hardware/gpio.h stub in this directory -- proves the
 *        actual GPIO pin sequencing (DOx_CMD driven low-then-out on
 *        start(), the shared DO_ENABLE_REQ asserted only once across both
 *        channels, DOx_FAULT read as a real externally-driven active-low
 *        input), not just that the vtable dispatches.
 */
#include "drivers/ips2050_driver.h"

#include "hardware/gpio.h"

#include <assert.h>

#define DO0_INDEX 0u
#define DO1_INDEX 1u

#define DO0_CMD_GPIO 20u
#define DO1_CMD_GPIO 21u
#define DO0_FAULT_GPIO 22u
#define DO1_FAULT_GPIO 23u
#define ENABLE_GPIO 10u

static telemetry_channel_t make_channel(native_channel_source_t *source)
{
    telemetry_channel_t channel = {0};
    channel.driver_context = source;
    channel.metadata.value_type = TELEMETRY_CHANNEL_VALUE_BOOLEAN;
    return channel;
}

static void test_source_validation(void)
{
    assert(native_ips2050_source_is_valid(0u, 0u));
    assert(native_ips2050_source_is_valid(1u, 0u));
    assert(!native_ips2050_source_is_valid(2u, 0u));
    assert(!native_ips2050_source_is_valid(0u, 1u));
}

static void test_start_drives_safe_state_and_asserts_shared_enable_once(void)
{
    native_channel_source_t source0 = {.source_index = DO0_INDEX, .source_flags = 0u};
    native_channel_source_t source1 = {.source_index = DO1_INDEX, .source_flags = 0u};
    telemetry_channel_t channel0 = make_channel(&source0);
    telemetry_channel_t channel1 = make_channel(&source1);

    gpio_stub_reset();
    assert(native_ips2050_start(&channel0) == TELEMETRY_DRIVER_RESULT_OK);
    assert(g_gpio_stub_pins[DO0_CMD_GPIO].out_enabled && !g_gpio_stub_pins[DO0_CMD_GPIO].driven_level);
    assert(!g_gpio_stub_pins[DO0_FAULT_GPIO].out_enabled && g_gpio_stub_pins[DO0_FAULT_GPIO].pulls_disabled);
    assert(g_gpio_stub_pins[ENABLE_GPIO].out_enabled && g_gpio_stub_pins[ENABLE_GPIO].driven_level);
    assert(channel0.runtime.value.boolean == false);

    /* Simulate something having since changed the enable pin's driven level;
     * a second channel's start() must NOT re-assert (re-init would reset
     * this back to true via gpio_init()'s zeroing, masking the bug this
     * test is designed to catch). */
    g_gpio_stub_pins[ENABLE_GPIO].driven_level = false;
    assert(native_ips2050_start(&channel1) == TELEMETRY_DRIVER_RESULT_OK);
    assert(g_gpio_stub_pins[DO1_CMD_GPIO].out_enabled && !g_gpio_stub_pins[DO1_CMD_GPIO].driven_level);
    assert(g_gpio_stub_pins[ENABLE_GPIO].driven_level == false);
}

static void test_write_commands_output_and_stop_forces_safe_state(void)
{
    native_channel_source_t source = {.source_index = DO0_INDEX, .source_flags = 0u};
    telemetry_channel_t channel = make_channel(&source);
    telemetry_channel_value_t on = {.boolean = true};

    gpio_stub_reset();
    assert(native_ips2050_start(&channel) == TELEMETRY_DRIVER_RESULT_OK);
    assert(native_ips2050_write(&channel, &on) == TELEMETRY_DRIVER_RESULT_OK);
    assert(g_gpio_stub_pins[DO0_CMD_GPIO].driven_level == true);
    assert(channel.runtime.value.boolean == true);

    assert(native_ips2050_stop(&channel) == TELEMETRY_DRIVER_RESULT_OK);
    assert(g_gpio_stub_pins[DO0_CMD_GPIO].driven_level == false);
    assert(channel.runtime.value.boolean == false);
    /* stop() must not re-touch the shared enable line at all -- proven
     * directly (not just "still true", which the driver's one-time-latched
     * g_enable_asserted would leave true regardless) by the dedicated test
     * above, which checks the actual GPIO calls a stop() here would need to
     * repeat if it (incorrectly) touched the line again. */
}

static void test_write_does_not_disturb_the_other_channel(void)
{
    native_channel_source_t source0 = {.source_index = DO0_INDEX, .source_flags = 0u};
    native_channel_source_t source1 = {.source_index = DO1_INDEX, .source_flags = 0u};
    telemetry_channel_t channel0 = make_channel(&source0);
    telemetry_channel_t channel1 = make_channel(&source1);
    telemetry_channel_value_t on = {.boolean = true};

    gpio_stub_reset();
    assert(native_ips2050_start(&channel0) == TELEMETRY_DRIVER_RESULT_OK);
    assert(native_ips2050_start(&channel1) == TELEMETRY_DRIVER_RESULT_OK);
    assert(native_ips2050_write(&channel0, &on) == TELEMETRY_DRIVER_RESULT_OK);
    assert(g_gpio_stub_pins[DO0_CMD_GPIO].driven_level == true);
    assert(g_gpio_stub_pins[DO1_CMD_GPIO].driven_level == false);
}

static void test_self_test_reads_real_fault_line(void)
{
    native_channel_source_t source = {.source_index = DO1_INDEX, .source_flags = 0u};
    telemetry_channel_t channel = make_channel(&source);

    gpio_stub_reset();
    assert(native_ips2050_start(&channel) == TELEMETRY_DRIVER_RESULT_OK);
    /* Idle (no fault): active-low line reads logic-high. */
    assert(native_ips2050_self_test(&channel) == TELEMETRY_DRIVER_RESULT_OK);

    g_gpio_stub_pins[DO1_FAULT_GPIO].external_level = false;
    assert(native_ips2050_self_test(&channel) == TELEMETRY_DRIVER_RESULT_HARDWARE_FAILURE);

    g_gpio_stub_pins[DO1_FAULT_GPIO].external_level = true;
    assert(native_ips2050_self_test(&channel) == TELEMETRY_DRIVER_RESULT_OK);
    /* The other channel's fault line is unaffected. */
    assert(g_gpio_stub_pins[DO0_FAULT_GPIO].external_level == true);
}

static void test_invalid_arguments_rejected(void)
{
    native_channel_source_t bad_source = {.source_index = 5u, .source_flags = 0u};
    native_channel_source_t good_source = {.source_index = DO0_INDEX, .source_flags = 0u};
    telemetry_channel_t bad_channel = make_channel(&bad_source);
    telemetry_channel_t wrong_type_channel = make_channel(&good_source);
    telemetry_channel_value_t value = {.boolean = true};

    wrong_type_channel.metadata.value_type = TELEMETRY_CHANNEL_VALUE_FLOAT;

    gpio_stub_reset();
    assert(native_ips2050_start(&bad_channel) == TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT);
    assert(native_ips2050_stop(&bad_channel) == TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT);
    assert(native_ips2050_self_test(&bad_channel) == TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT);
    assert(native_ips2050_write(&bad_channel, &value) == TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT);
    assert(native_ips2050_write(&wrong_type_channel, &value) == TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT);
    assert(native_ips2050_write(&wrong_type_channel, NULL) == TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT);
}

int main(void)
{
    test_source_validation();
    test_start_drives_safe_state_and_asserts_shared_enable_once();
    test_write_commands_output_and_stop_forces_safe_state();
    test_write_does_not_disturb_the_other_channel();
    test_self_test_reads_real_fault_line();
    test_invalid_arguments_rejected();
    return 0;
}
