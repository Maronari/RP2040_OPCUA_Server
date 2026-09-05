/**
 * @file
 * @brief Host tests for the real drivers/src/ad5422_driver.c against a
 *        fake two-device daisy-chain model (per-device data/control
 *        registers, readback-select + NOP clock-out) -- proves the actual
 *        48-bit two-device framing (NOP for the untouched device, no
 *        cross-channel corruption) and voltage/current scaling, not just
 *        that the vtable dispatches.
 */
#include "drivers/ad5422_driver.h"

#include <assert.h>
#include <math.h>
#include <string.h>

#define AO0_INDEX 0u
#define AO1_INDEX 1u

typedef struct
{
    uint16_t data_reg[2];
    uint16_t control_reg[2];
    uint8_t readback_select[2];
    bool readback_pending[2];
    bool device_present[2];
    bool corrupt_echo[2];
    bool force_transfer_failure;
} fake_chain_t;

static fake_chain_t g_chip;

static void chip_reset(void)
{
    memset(&g_chip, 0, sizeof(g_chip));
    g_chip.device_present[0] = true;
    g_chip.device_present[1] = true;
}

static void process_device(fake_chain_t *chip, uint8_t device_index, const uint8_t word[3], uint8_t *out_rx)
{
    uint8_t address = word[0];
    uint16_t data = (uint16_t)(((uint16_t)word[1] << 8) | word[2]);

    /* Whatever clocks out this transfer reflects a readback selection made
     * by a *prior* transfer -- matches the datasheet's select-then-NOP
     * readback sequence. */
    if (chip->readback_pending[device_index] && out_rx != NULL)
    {
        uint16_t value = 0u;

        if (chip->readback_select[device_index] == 0x1u)
        {
            value = chip->data_reg[device_index];
        }
        else if (chip->readback_select[device_index] == 0x2u)
        {
            value = chip->control_reg[device_index];
        }
        if (chip->device_present[device_index])
        {
            if (chip->corrupt_echo[device_index])
            {
                value = (uint16_t)~value;
            }
            out_rx[1] = (uint8_t)(value >> 8);
            out_rx[2] = (uint8_t)(value & 0xFFu);
        }
        chip->readback_pending[device_index] = false;
    }

    if (address == 0x01u)
    {
        chip->data_reg[device_index] = data;
    }
    else if (address == 0x55u)
    {
        chip->control_reg[device_index] = data;
    }
    else if (address == 0x02u)
    {
        chip->readback_select[device_index] = (uint8_t)(data & 0x3u);
        chip->readback_pending[device_index] = true;
    }
    /* address 0x00 (NOP) and anything else: this device's registers are untouched. */
}

static bool fake_transfer(void *context, const uint8_t *tx, uint8_t *rx, size_t length)
{
    fake_chain_t *chip = (fake_chain_t *)context;

    if (chip->force_transfer_failure || length != 6u)
    {
        return false;
    }
    if (rx != NULL)
    {
        memset(rx, 0, length);
    }
    /* Far slot (bytes 0-2) targets AO1 (transmitted first); near slot
     * (bytes 3-5) targets AO0 (transmitted second) -- see
     * docs/datasheets/AD5422-register-notes.md. */
    process_device(chip, AO1_INDEX, &tx[0], rx != NULL ? &rx[0] : NULL);
    process_device(chip, AO0_INDEX, &tx[3], rx != NULL ? &rx[3] : NULL);
    return true;
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
    return channel;
}

static void assert_close(double a, double b)
{
    assert(fabs(a - b) < 0.01);
}

static void test_source_validation(void)
{
    assert(native_ad5422_source_is_valid(0u, 0x0u));
    assert(native_ad5422_source_is_valid(1u, 0x7u));
    assert(!native_ad5422_source_is_valid(2u, 0x0u));
    assert(!native_ad5422_source_is_valid(0u, 0x4u)); /* reserved range code */
}

static void test_no_bus_bound(void)
{
    native_channel_source_t source = {.source_index = 0u, .source_flags = 0x1u};
    telemetry_channel_t channel = make_channel(&source, TELEMETRY_CHANNEL_VALUE_FLOAT);
    telemetry_channel_value_t value = {.f32 = 1.0f};

    native_ad5422_bind_bus(NULL);
    assert(native_ad5422_start(&channel) == TELEMETRY_DRIVER_RESULT_INVALID_STATE);
    assert(native_ad5422_write(&channel, &value) == TELEMETRY_DRIVER_RESULT_INVALID_STATE);
    assert(native_ad5422_self_test(&channel) == TELEMETRY_DRIVER_RESULT_INVALID_STATE);
}

static void test_start_enables_output_and_writing_one_channel_does_not_touch_the_other(void)
{
    spi_bus_t bus = {.vtbl = &g_fake_vtbl, .context = &g_chip};
    native_channel_source_t source_ao0 = {.source_index = AO0_INDEX, .source_flags = 0x1u}; /* 0-10V */
    native_channel_source_t source_ao1 = {.source_index = AO1_INDEX, .source_flags = 0x5u}; /* 4-20mA */
    telemetry_channel_t channel_ao0 = make_channel(&source_ao0, TELEMETRY_CHANNEL_VALUE_FLOAT);
    telemetry_channel_t channel_ao1 = make_channel(&source_ao1, TELEMETRY_CHANNEL_VALUE_DOUBLE);
    telemetry_channel_value_t value_ao0 = {.f32 = 5.0f}; /* half of 0-10V */

    chip_reset();
    native_ad5422_bind_bus(&bus);
    assert(native_ad5422_start(&channel_ao0) == TELEMETRY_DRIVER_RESULT_OK);
    assert(native_ad5422_start(&channel_ao1) == TELEMETRY_DRIVER_RESULT_OK);
    assert((g_chip.control_reg[AO0_INDEX] & (1u << 12)) != 0u); /* OUTEN */
    assert((g_chip.control_reg[AO0_INDEX] & (1u << 5)) != 0u);  /* DCEN */
    assert((g_chip.control_reg[AO0_INDEX] & 0x7u) == 0x1u);
    assert((g_chip.control_reg[AO1_INDEX] & 0x7u) == 0x5u);
    assert(g_chip.data_reg[AO0_INDEX] == 0u);
    assert(g_chip.data_reg[AO1_INDEX] == 0u);

    /* Writing AO0 must not disturb AO1's already-latched data register. */
    g_chip.data_reg[AO1_INDEX] = 0xBEEFu;
    assert(native_ad5422_write(&channel_ao0, &value_ao0) == TELEMETRY_DRIVER_RESULT_OK);
    assert(g_chip.data_reg[AO1_INDEX] == 0xBEEFu);
    assert_close((double)g_chip.data_reg[AO0_INDEX] / 65535.0 * 10.0, 5.0);
}

static void test_write_scales_voltage_and_current(void)
{
    spi_bus_t bus = {.vtbl = &g_fake_vtbl, .context = &g_chip};
    native_channel_source_t source_voltage = {.source_index = AO0_INDEX, .source_flags = 0x1u}; /* 0-10V */
    native_channel_source_t source_current = {.source_index = AO1_INDEX, .source_flags = 0x5u}; /* 4-20mA */
    telemetry_channel_t channel_voltage = make_channel(&source_voltage, TELEMETRY_CHANNEL_VALUE_FLOAT);
    telemetry_channel_t channel_current = make_channel(&source_current, TELEMETRY_CHANNEL_VALUE_DOUBLE);
    telemetry_channel_value_t ten_volts = {.f32 = 10.0f};
    telemetry_channel_value_t twelve_ma = {.f64 = 12.0};

    chip_reset();
    native_ad5422_bind_bus(&bus);
    assert(native_ad5422_write(&channel_voltage, &ten_volts) == TELEMETRY_DRIVER_RESULT_OK);
    /* Full-scale code (65535) for 10 V on the 0-10V range. */
    assert(g_chip.data_reg[AO0_INDEX] == 65535u);
    assert_close((double)channel_voltage.runtime.value.f32, 10.0);

    /* 12 mA on 4-20mA: normalized = (12-4)/16 = 0.5 -> code ~32768. */
    assert(native_ad5422_write(&channel_current, &twelve_ma) == TELEMETRY_DRIVER_RESULT_OK);
    assert(g_chip.data_reg[AO1_INDEX] >= 32760u && g_chip.data_reg[AO1_INDEX] <= 32770u);
    assert_close(channel_current.runtime.value.f64, 12.0);

    /* Out-of-range commands clamp rather than wrap. */
    telemetry_channel_value_t negative = {.f32 = -5.0f};
    assert(native_ad5422_write(&channel_voltage, &negative) == TELEMETRY_DRIVER_RESULT_OK);
    assert(g_chip.data_reg[AO0_INDEX] == 0u);
}

static void test_stop_disables_output(void)
{
    spi_bus_t bus = {.vtbl = &g_fake_vtbl, .context = &g_chip};
    native_channel_source_t source = {.source_index = AO0_INDEX, .source_flags = 0x1u};
    telemetry_channel_t channel = make_channel(&source, TELEMETRY_CHANNEL_VALUE_FLOAT);

    chip_reset();
    native_ad5422_bind_bus(&bus);
    assert(native_ad5422_start(&channel) == TELEMETRY_DRIVER_RESULT_OK);
    assert((g_chip.control_reg[AO0_INDEX] & (1u << 12)) != 0u);
    assert(native_ad5422_stop(&channel) == TELEMETRY_DRIVER_RESULT_OK);
    assert((g_chip.control_reg[AO0_INDEX] & (1u << 12)) == 0u); /* OUTEN cleared */
    assert((g_chip.control_reg[AO0_INDEX] & (1u << 5)) != 0u);  /* DCEN preserved */
}

static void test_self_test_passed_unavailable_failed(void)
{
    spi_bus_t bus = {.vtbl = &g_fake_vtbl, .context = &g_chip};
    native_channel_source_t source = {.source_index = AO1_INDEX, .source_flags = 0x2u};
    telemetry_channel_t channel = make_channel(&source, TELEMETRY_CHANNEL_VALUE_FLOAT);
    telemetry_channel_value_t value = {.f32 = 1.0f};

    chip_reset();
    native_ad5422_bind_bus(&bus);
    assert(native_ad5422_write(&channel, &value) == TELEMETRY_DRIVER_RESULT_OK);
    assert(native_ad5422_self_test(&channel) == TELEMETRY_DRIVER_RESULT_OK);
    /* The channel's actual last-commanded code must be restored afterward. */
    assert(g_chip.data_reg[AO1_INDEX] != 0x1234u && g_chip.data_reg[AO1_INDEX] != 0xABCDu);

    g_chip.device_present[AO1_INDEX] = false;
    assert(native_ad5422_self_test(&channel) == TELEMETRY_DRIVER_RESULT_TIMEOUT);

    g_chip.device_present[AO1_INDEX] = true;
    g_chip.corrupt_echo[AO1_INDEX] = true;
    assert(native_ad5422_self_test(&channel) == TELEMETRY_DRIVER_RESULT_HARDWARE_FAILURE);
}

static void test_transfer_failure_maps_to_timeout(void)
{
    spi_bus_t bus = {.vtbl = &g_fake_vtbl, .context = &g_chip};
    native_channel_source_t source = {.source_index = AO0_INDEX, .source_flags = 0x1u};
    telemetry_channel_t channel = make_channel(&source, TELEMETRY_CHANNEL_VALUE_FLOAT);
    telemetry_channel_value_t value = {.f32 = 1.0f};

    chip_reset();
    native_ad5422_bind_bus(&bus);
    g_chip.force_transfer_failure = true;
    assert(native_ad5422_start(&channel) == TELEMETRY_DRIVER_RESULT_TIMEOUT);
    assert(native_ad5422_write(&channel, &value) == TELEMETRY_DRIVER_RESULT_TIMEOUT);
    assert(native_ad5422_self_test(&channel) == TELEMETRY_DRIVER_RESULT_TIMEOUT);
}

int main(void)
{
    test_source_validation();
    test_no_bus_bound();
    test_start_enables_output_and_writing_one_channel_does_not_touch_the_other();
    test_write_scales_voltage_and_current();
    test_stop_disables_output();
    test_self_test_passed_unavailable_failed();
    test_transfer_failure_maps_to_timeout();
    return 0;
}
