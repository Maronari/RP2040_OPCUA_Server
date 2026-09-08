/**
 * @file
 * @brief Host tests for the real drivers/src/synthetic_driver.c -- no
 *        fake bus/stub needed at all, since this driver has no hardware
 *        dependency; proves the actual counting/reset/wrap behavior, not
 *        just that the vtable dispatches.
 */
#include "drivers/synthetic_driver.h"

#include <assert.h>

static telemetry_channel_t make_channel(native_channel_source_t *source, telemetry_channel_value_type_t value_type)
{
    telemetry_channel_t channel = {0};
    channel.driver_context = source;
    channel.metadata.value_type = value_type;
    return channel;
}

static void test_source_validation(void)
{
    assert(native_synthetic_source_is_valid(0u, 0u));
    assert(native_synthetic_source_is_valid(7u, 0u));
    assert(!native_synthetic_source_is_valid(8u, 0u));
    assert(!native_synthetic_source_is_valid(0u, 1u));
}

static void test_start_resets_and_read_increments(void)
{
    native_channel_source_t source = {.source_index = 2u, .source_flags = 0u};
    telemetry_channel_t channel = make_channel(&source, TELEMETRY_CHANNEL_VALUE_DOUBLE);

    assert(native_synthetic_start(&channel) == TELEMETRY_DRIVER_RESULT_OK);
    assert(native_synthetic_read(&channel, 0u) == TELEMETRY_DRIVER_RESULT_OK);
    assert(channel.runtime.value.f64 == 1.0);
    assert(native_synthetic_read(&channel, 0u) == TELEMETRY_DRIVER_RESULT_OK);
    assert(channel.runtime.value.f64 == 2.0);
    assert(native_synthetic_read(&channel, 0u) == TELEMETRY_DRIVER_RESULT_OK);
    assert(channel.runtime.value.f64 == 3.0);

    /* start() again resets this channel's counter back to 0. */
    assert(native_synthetic_start(&channel) == TELEMETRY_DRIVER_RESULT_OK);
    assert(native_synthetic_read(&channel, 0u) == TELEMETRY_DRIVER_RESULT_OK);
    assert(channel.runtime.value.f64 == 1.0);
}

static void test_channels_are_independent(void)
{
    native_channel_source_t source_a = {.source_index = 0u, .source_flags = 0u};
    native_channel_source_t source_b = {.source_index = 0u, .source_flags = 0u};
    telemetry_channel_t channel_a = make_channel(&source_a, TELEMETRY_CHANNEL_VALUE_DOUBLE);
    telemetry_channel_t channel_b = make_channel(&source_b, TELEMETRY_CHANNEL_VALUE_DOUBLE);

    assert(native_synthetic_start(&channel_a) == TELEMETRY_DRIVER_RESULT_OK);
    assert(native_synthetic_start(&channel_b) == TELEMETRY_DRIVER_RESULT_OK);
    assert(native_synthetic_read(&channel_a, 0u) == TELEMETRY_DRIVER_RESULT_OK);
    assert(native_synthetic_read(&channel_a, 0u) == TELEMETRY_DRIVER_RESULT_OK);
    assert(native_synthetic_read(&channel_b, 0u) == TELEMETRY_DRIVER_RESULT_OK);
    assert(channel_a.runtime.value.f64 == 2.0);
    assert(channel_b.runtime.value.f64 == 1.0);
    assert(native_synthetic_start(&channel_b) == TELEMETRY_DRIVER_RESULT_OK);
    assert(native_synthetic_read(&channel_a, 0u) == TELEMETRY_DRIVER_RESULT_OK);
    assert(channel_a.runtime.value.f64 == 3.0);
    source_a.synthetic_read_count = UINT32_MAX;
    assert(native_synthetic_read(&channel_a, 0u) == TELEMETRY_DRIVER_RESULT_OK);
    assert(channel_a.runtime.value.f64 == 0.0);
    assert(source_a.synthetic_read_count == 0u);
}

static void test_float_value_type_supported(void)
{
    native_channel_source_t source = {.source_index = 3u, .source_flags = 0u};
    telemetry_channel_t channel = make_channel(&source, TELEMETRY_CHANNEL_VALUE_FLOAT);

    assert(native_synthetic_start(&channel) == TELEMETRY_DRIVER_RESULT_OK);
    assert(native_synthetic_read(&channel, 0u) == TELEMETRY_DRIVER_RESULT_OK);
    assert(channel.runtime.value.f32 == 1.0f);
}

static void test_unsupported_value_type_rejected(void)
{
    native_channel_source_t source = {.source_index = 4u, .source_flags = 0u};
    telemetry_channel_t channel = make_channel(&source, TELEMETRY_CHANNEL_VALUE_BOOLEAN);

    assert(native_synthetic_start(&channel) == TELEMETRY_DRIVER_RESULT_OK);
    assert(native_synthetic_read(&channel, 0u) == TELEMETRY_DRIVER_RESULT_UNSUPPORTED);
    assert(source.synthetic_read_count == 0u);
}

static void test_invalid_source_rejected(void)
{
    native_channel_source_t bad_source = {.source_index = 8u, .source_flags = 0u};
    telemetry_channel_t bad_channel = make_channel(&bad_source, TELEMETRY_CHANNEL_VALUE_DOUBLE);

    assert(native_synthetic_start(&bad_channel) == TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT);
    assert(native_synthetic_stop(&bad_channel) == TELEMETRY_DRIVER_RESULT_OK); /* stop() is unconditionally a no-op */
    assert(native_synthetic_read(&bad_channel, 0u) == TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT);
}

int main(void)
{
    test_source_validation();
    test_start_resets_and_read_increments();
    test_channels_are_independent();
    test_float_value_type_supported();
    test_unsupported_value_type_rejected();
    test_invalid_source_rejected();
    return 0;
}
