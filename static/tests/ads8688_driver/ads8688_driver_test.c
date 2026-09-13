/**
 * @file
 * @brief Host tests for the real drivers/src/ads8688_driver.c against a
 *        fake ADS8688 chip model (register echo-on-write, 32-clock manual
 *        conversion frame, one-frame pipeline) -- proves the actual
 *        register/command byte framing and voltage scaling, not just that
 *        the vtable dispatches.
 */
#include "drivers/ads8688_driver.h"

#include <assert.h>
#include <math.h>
#include <string.h>

typedef struct
{
    uint8_t registers[0x40];
    uint8_t selected_channel;
    uint16_t channel_raw[8];
    bool chip_present;
    bool corrupt_echo;
    bool force_transfer_failure;
    size_t transfer_count;
    size_t fail_at_transfer;
    size_t last_command_length;
    bool last_command_padding_zero;
} fake_chip_t;

static fake_chip_t g_chip;

static void chip_reset(void)
{
    memset(&g_chip, 0, sizeof(g_chip));
    g_chip.chip_present = true;
}

static bool fake_transfer(void *context, const uint8_t *tx, uint8_t *rx, size_t length)
{
    fake_chip_t *chip = (fake_chip_t *)context;

    ++chip->transfer_count;
    if (chip->force_transfer_failure ||
        (chip->fail_at_transfer != 0u && chip->transfer_count == chip->fail_at_transfer))
    {
        return false;
    }
    if (rx != NULL)
    {
        memset(rx, 0, length);
    }
    if (length == 3u)
    {
        /* Program register read/write: byte0 = (ADDR[6:0]<<1)|WR, byte1 =
         * write data (write) or don't-care (read), byte2 = dummy clocking
         * out the echo (write) or DOUT (read). */
        uint8_t address = (uint8_t)(tx[0] >> 1);
        bool write = (tx[0] & 0x01u) != 0u;

        if (write)
        {
            chip->registers[address] = tx[1];
            if (rx != NULL)
            {
                rx[2] = !chip->chip_present ? 0x00u : (chip->corrupt_echo ? (uint8_t)(~tx[1]) : tx[1]);
            }
        }
        else if (rx != NULL)
        {
            rx[2] = chip->chip_present ? chip->registers[address] : 0x00u;
        }
        return true;
    }
    if (length == 4u)
    {
        /* Complete 32-clock MAN_Ch_n frame: command occupies the first
         * 16 clocks; SDO carries the PREVIOUS pipelined conversion only
         * during clocks 17-32. */
        uint16_t command = (uint16_t)(((uint16_t)tx[0] << 8) | tx[1]);
        uint16_t previous_result = chip->channel_raw[chip->selected_channel];

        chip->last_command_length = length;
        chip->last_command_padding_zero = tx[2] == 0u && tx[3] == 0u;

        if ((command & 0xE000u) == 0xC000u) /* MAN_Ch_0..MAN_Ch_7 */
        {
            chip->selected_channel = (uint8_t)((command >> 10) & 0x07u);
        }
        if (rx != NULL && chip->chip_present)
        {
            rx[2] = (uint8_t)(previous_result >> 8);
            rx[3] = (uint8_t)(previous_result & 0xFFu);
        }
        return true;
    }
    return false;
}

static void cs_noop(void *context)
{
    (void)context;
}

static const spi_bus_vtbl_t g_fake_vtbl = {
    .cs_assert = cs_noop,
    .cs_deassert = cs_noop,
    .transfer = fake_transfer,
};

static telemetry_channel_t make_channel(native_channel_source_t *source, telemetry_channel_value_type_t value_type)
{
    telemetry_channel_t channel = {0};
    channel.driver_context = source;
    channel.metadata.value_type = value_type;
    channel.settings.scale = 1.0;
    channel.settings.offset = 0.0;
    return channel;
}

static void test_source_validation(void)
{
    assert(native_ads8688_source_is_valid(0u, 0x0u));
    assert(native_ads8688_source_is_valid(3u, 0x6u));
    /* CH4..CH7 are tied to AGND on RP2350_INDUSTRIAL_MVP_V1: AI0..AI3 only. */
    assert(!native_ads8688_source_is_valid(4u, 0x0u));
    assert(!native_ads8688_source_is_valid(7u, 0x6u));
    assert(!native_ads8688_source_is_valid(8u, 0x0u));
    assert(!native_ads8688_source_is_valid(0u, 0x3u)); /* not one of the five valid range codes */
    assert(!native_ads8688_source_is_valid(0u, 0x4u));
    assert(!native_ads8688_source_is_valid(0u, 0xFu));
}

static void test_no_bus_bound(void)
{
    native_channel_source_t source = {.source_index = 0u, .source_flags = 0x0u};
    telemetry_channel_t channel = make_channel(&source, TELEMETRY_CHANNEL_VALUE_FLOAT);

    native_ads8688_bind_bus(NULL);
    assert(native_ads8688_start(&channel) == TELEMETRY_DRIVER_RESULT_INVALID_STATE);
    assert(native_ads8688_read(&channel, 0u) == TELEMETRY_DRIVER_RESULT_INVALID_STATE);
    assert(native_ads8688_self_test(&channel) == TELEMETRY_DRIVER_RESULT_INVALID_STATE);
}

static void test_start_writes_range_and_selects_channel(void)
{
    spi_bus_t bus = {.vtbl = &g_fake_vtbl, .context = &g_chip};
    native_channel_source_t source = {.source_index = 3u, .source_flags = 0x1u};
    telemetry_channel_t channel = make_channel(&source, TELEMETRY_CHANNEL_VALUE_FLOAT);

    chip_reset();
    native_ads8688_bind_bus(&bus);
    assert(native_ads8688_start(&channel) == TELEMETRY_DRIVER_RESULT_OK);
    assert(g_chip.registers[0x05u + 3u] == 0x1u);
    assert(g_chip.selected_channel == 3u);
    assert(g_chip.last_command_length == 4u);
    assert(g_chip.last_command_padding_zero);
    assert(native_ads8688_stop(&channel) == TELEMETRY_DRIVER_RESULT_OK);
}

static void assert_close(double a, double b)
{
    assert(fabs(a - b) < 0.01);
}

static void test_read_scales_bipolar_and_unipolar(void)
{
    spi_bus_t bus = {.vtbl = &g_fake_vtbl, .context = &g_chip};
    native_channel_source_t bipolar_source = {.source_index = 2u, .source_flags = 0x1u}; /* +/-5.12V */
    telemetry_channel_t bipolar_channel = make_channel(&bipolar_source, TELEMETRY_CHANNEL_VALUE_FLOAT);
    native_channel_source_t unipolar_source = {.source_index = 3u, .source_flags = 0x5u}; /* 0-10.24V */
    telemetry_channel_t unipolar_channel = make_channel(&unipolar_source, TELEMETRY_CHANNEL_VALUE_DOUBLE);

    chip_reset();
    native_ads8688_bind_bus(&bus);

    /* ADS8688 bipolar data is offset binary: 0x8000 is zero volts. */
    g_chip.channel_raw[2] = 0x8000u;
    assert(native_ads8688_read(&bipolar_channel, 0u) == TELEMETRY_DRIVER_RESULT_OK);
    assert_close((double)bipolar_channel.runtime.value.f32, 0.0);

    /* One quarter below/above midscale maps to -/+ half full-scale. */
    g_chip.channel_raw[2] = 0x4000u;
    assert(native_ads8688_read(&bipolar_channel, 0u) == TELEMETRY_DRIVER_RESULT_OK);
    assert_close((double)bipolar_channel.runtime.value.f32, -2.56);
    g_chip.channel_raw[2] = 0xC000u;
    assert(native_ads8688_read(&bipolar_channel, 0u) == TELEMETRY_DRIVER_RESULT_OK);
    assert_close((double)bipolar_channel.runtime.value.f32, 2.56);
    g_chip.channel_raw[2] = 0x0000u;
    assert(native_ads8688_read(&bipolar_channel, 0u) == TELEMETRY_DRIVER_RESULT_OK);
    assert_close((double)bipolar_channel.runtime.value.f32, -5.12);
    g_chip.channel_raw[2] = 0xFFFFu;
    assert(native_ads8688_read(&bipolar_channel, 0u) == TELEMETRY_DRIVER_RESULT_OK);
    assert_close((double)bipolar_channel.runtime.value.f32, 5.12);

    /* Unipolar half-scale: 32768/65536 * 10.24 = 5.12 V. */
    g_chip.channel_raw[3] = 0x8000u;
    assert(native_ads8688_read(&unipolar_channel, 0u) == TELEMETRY_DRIVER_RESULT_OK);
    assert_close(unipolar_channel.runtime.value.f64, 5.12);

    /* scale/offset settings apply on top of the raw conversion. */
    unipolar_channel.settings.scale = 2.0;
    unipolar_channel.settings.offset = 1.0;
    assert(native_ads8688_read(&unipolar_channel, 0u) == TELEMETRY_DRIVER_RESULT_OK);
    assert_close(unipolar_channel.runtime.value.f64, 5.12 * 2.0 + 1.0);
}

static void test_read_reselects_after_other_channel(void)
{
    spi_bus_t bus = {.vtbl = &g_fake_vtbl, .context = &g_chip};
    native_channel_source_t source_a = {.source_index = 0u, .source_flags = 0x0u};
    native_channel_source_t source_b = {.source_index = 1u, .source_flags = 0x0u};
    telemetry_channel_t channel_a = make_channel(&source_a, TELEMETRY_CHANNEL_VALUE_FLOAT);
    telemetry_channel_t channel_b = make_channel(&source_b, TELEMETRY_CHANNEL_VALUE_FLOAT);

    chip_reset();
    native_ads8688_bind_bus(&bus);
    g_chip.channel_raw[0] = 0x1000u;
    g_chip.channel_raw[1] = 0x2000u;

    /* Reading channel 0 then channel 1 must not return channel 0's stale
     * pipelined result for channel 1 -- proves the double-select priming. */
    assert(native_ads8688_read(&channel_a, 0u) == TELEMETRY_DRIVER_RESULT_OK);
    assert(native_ads8688_read(&channel_b, 0u) == TELEMETRY_DRIVER_RESULT_OK);
    assert_close((double)channel_a.runtime.value.f32, -8.96);
    assert_close((double)channel_b.runtime.value.f32, -7.68);
}

static void test_self_test_passed_unavailable_failed(void)
{
    spi_bus_t bus = {.vtbl = &g_fake_vtbl, .context = &g_chip};
    native_channel_source_t source = {.source_index = 1u, .source_flags = 0x2u};
    telemetry_channel_t channel = make_channel(&source, TELEMETRY_CHANNEL_VALUE_FLOAT);

    chip_reset();
    native_ads8688_bind_bus(&bus);
    g_chip.chip_present = true;
    g_chip.corrupt_echo = false;
    assert(native_ads8688_self_test(&channel) == TELEMETRY_DRIVER_RESULT_OK);
    /* The channel's actual configured range must be restored afterward. */
    assert(g_chip.registers[0x05u + 1u] == 0x2u);

    /* Absent/floating chip: echo never tracks what was written (constant readback). */
    g_chip.chip_present = false;
    assert(native_ads8688_self_test(&channel) == TELEMETRY_DRIVER_RESULT_TIMEOUT);

    /* Present but malfunctioning: echo varies but never matches what was written. */
    g_chip.chip_present = true;
    g_chip.corrupt_echo = true;
    assert(native_ads8688_self_test(&channel) == TELEMETRY_DRIVER_RESULT_HARDWARE_FAILURE);
}

static void test_transfer_failure_maps_to_timeout(void)
{
    spi_bus_t bus = {.vtbl = &g_fake_vtbl, .context = &g_chip};
    native_channel_source_t source = {.source_index = 0u, .source_flags = 0x0u};
    telemetry_channel_t channel = make_channel(&source, TELEMETRY_CHANNEL_VALUE_FLOAT);

    chip_reset();
    native_ads8688_bind_bus(&bus);
    g_chip.force_transfer_failure = true;
    assert(native_ads8688_start(&channel) == TELEMETRY_DRIVER_RESULT_TIMEOUT);
    assert(native_ads8688_read(&channel, 0u) == TELEMETRY_DRIVER_RESULT_TIMEOUT);
    assert(native_ads8688_self_test(&channel) == TELEMETRY_DRIVER_RESULT_TIMEOUT);
}

static void test_self_test_restore_failure_is_not_ok(void)
{
    spi_bus_t bus = {.vtbl = &g_fake_vtbl, .context = &g_chip};
    native_channel_source_t source = {.source_index = 1u, .source_flags = 0x1u};
    telemetry_channel_t channel = make_channel(&source, TELEMETRY_CHANNEL_VALUE_FLOAT);

    chip_reset();
    native_ads8688_bind_bus(&bus);
    /* probe A, probe B, then restoration of the configured range */
    g_chip.fail_at_transfer = 3u;
    assert(native_ads8688_self_test(&channel) == TELEMETRY_DRIVER_RESULT_TIMEOUT);
}

static void test_invalid_arguments(void)
{
    spi_bus_t bus = {.vtbl = &g_fake_vtbl, .context = &g_chip};
    native_channel_source_t bad_source = {.source_index = 9u, .source_flags = 0x0u};
    telemetry_channel_t bad_channel = make_channel(&bad_source, TELEMETRY_CHANNEL_VALUE_FLOAT);
    native_channel_source_t good_source = {.source_index = 0u, .source_flags = 0x0u};
    telemetry_channel_t bad_type_channel = make_channel(&good_source, TELEMETRY_CHANNEL_VALUE_INT32);

    chip_reset();
    native_ads8688_bind_bus(&bus);
    assert(native_ads8688_start(&bad_channel) == TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT);
    assert(native_ads8688_read(&bad_channel, 0u) == TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT);
    assert(native_ads8688_self_test(&bad_channel) == TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT);
    assert(native_ads8688_read(&bad_type_channel, 0u) == TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT);
}

int main(void)
{
    test_source_validation();
    test_no_bus_bound();
    test_start_writes_range_and_selects_channel();
    test_read_scales_bipolar_and_unipolar();
    test_read_reselects_after_other_channel();
    test_self_test_passed_unavailable_failed();
    test_transfer_failure_maps_to_timeout();
    test_self_test_restore_failure_is_not_ok();
    test_invalid_arguments();
    return 0;
}
