/**
 * @file
 * @brief Native channel driver for the two `AD5422` industrial analog
 *        output channels.
 *
 * See drivers/include/drivers/ad5422_driver.h for the full contract and
 * docs/datasheets/AD5422-register-notes.md for the register/command
 * reference this implementation follows.
 *
 * Ownership: the SPI bus binding (native_ad5422_bind_bus()) and the
 * board-level binding flag are process-wide state -- both AO0 and AO1
 * share one daisy-chained bus. Self-test only reads registers, so no
 * per-channel DAC cache is required.
 */
#include "drivers/ad5422_driver.h"

#include <stddef.h>
#include <string.h>

#define AD5422_ADDR_NOP 0x00u
#define AD5422_ADDR_DATA 0x01u
#define AD5422_ADDR_READBACK 0x02u
#define AD5422_ADDR_CONTROL 0x55u

#define AD5422_READ_ADDR_STATUS 0x0u
#define AD5422_READ_ADDR_CONTROL 0x2u

#define AD5422_CTRL_OUTEN (1u << 12)
#define AD5422_CTRL_DCEN (1u << 5)

#define AD5422_AO1_INDEX 1u /**< Far device in the chain -- transmitted first. */
#define AD5422_AO0_INDEX 0u /**< Near device in the chain -- transmitted second. */
#define AD5422_CHANNEL_COUNT 2u

#define AD5422_VREFIN 5.0

typedef struct
{
    uint8_t code;
    bool current_mode;
    bool bipolar;
    double gain;              /**< Voltage ranges only. */
    double full_scale_ma;     /**< Current ranges only. */
    double current_offset_ma; /**< Current ranges only (4-20 mA's 4 mA floor). */
} ad5422_range_entry_t;

/** @brief The seven valid R[2:0] codes (docs/datasheets/AD5422-register-notes.md); 0x4 is reserved. */
static const ad5422_range_entry_t g_ranges[] = {
    {0x0u, false, false, 1.0, 0.0, 0.0}, /* 0 V to 5 V */
    {0x1u, false, false, 2.0, 0.0, 0.0}, /* 0 V to 10 V */
    {0x2u, false, true, 2.0, 0.0, 0.0},  /* +/-5 V */
    {0x3u, false, true, 4.0, 0.0, 0.0},  /* +/-10 V */
    {0x5u, true, false, 0.0, 16.0, 4.0}, /* 4 mA to 20 mA */
    {0x6u, true, false, 0.0, 20.0, 0.0}, /* 0 mA to 20 mA */
    {0x7u, true, false, 0.0, 24.0, 0.0}, /* 0 mA to 24 mA */
};

static spi_bus_t g_bus;
static bool g_bus_bound;

void native_ad5422_bind_bus(const spi_bus_t *bus)
{
    if (bus != NULL)
    {
        g_bus = *bus;
        g_bus_bound = true;
    }
    else
    {
        g_bus_bound = false;
    }
}

static const ad5422_range_entry_t *ad5422_range_for(uint8_t code)
{
    size_t index;

    for (index = 0u; index < sizeof(g_ranges) / sizeof(g_ranges[0]); ++index)
    {
        if (g_ranges[index].code == code)
        {
            return &g_ranges[index];
        }
    }
    return NULL;
}

bool native_ad5422_source_is_valid(uint8_t source_index, uint8_t source_flags)
{
    return source_index <= AD5422_AO1_INDEX && ad5422_range_for(source_flags) != NULL;
}

/** @brief Reinterpret channel->driver_context as this driver's source selector, or NULL. */
static const native_channel_source_t *ad5422_source(const telemetry_channel_t *channel)
{
    return channel != NULL ? (const native_channel_source_t *)channel->driver_context : NULL;
}

static void ad5422_pack_word(uint8_t address, uint16_t data, uint8_t out[3])
{
    out[0] = address;
    out[1] = (uint8_t)(data >> 8);
    out[2] = (uint8_t)(data & 0xFFu);
}

/**
 * @brief Send one 48-bit two-device frame: a real command word for
 *        @p target_index's slot, `NOP` for the other device's slot.
 * @param out_target_rx Optional; receives the 16-bit data-field bytes
 *        clocked back at @p target_index's own slot during this same
 *        transfer (meaningful only as the second step of a readback
 *        sequence -- see ad5422_read_register()).
 */
static bool ad5422_transact(uint8_t target_index, uint8_t address, uint16_t data, uint16_t *out_target_rx)
{
    uint8_t tx[6];
    uint8_t rx[6] = {0};
    uint8_t far_word[3];
    uint8_t near_word[3];

    if (target_index == AD5422_AO1_INDEX)
    {
        ad5422_pack_word(address, data, far_word);
        ad5422_pack_word(AD5422_ADDR_NOP, 0u, near_word);
    }
    else
    {
        ad5422_pack_word(AD5422_ADDR_NOP, 0u, far_word);
        ad5422_pack_word(address, data, near_word);
    }
    memcpy(tx, far_word, sizeof(far_word));
    memcpy(tx + sizeof(far_word), near_word, sizeof(near_word));
    if (!spi_bus_transfer_framed(&g_bus, tx, rx, sizeof(tx)))
    {
        return false;
    }
    if (out_target_rx != NULL)
    {
        const uint8_t *slot = (target_index == AD5422_AO1_INDEX) ? &rx[0] : &rx[3];
        *out_target_rx = (uint16_t)(((uint16_t)slot[1] << 8) | slot[2]);
    }
    return true;
}

/** @brief Non-invasive register read; never modifies DAC DATA or CONTROL. */
static bool ad5422_read_register(uint8_t target_index, uint16_t read_address, uint16_t *out_echo)
{
    if (!ad5422_transact(target_index, AD5422_ADDR_READBACK, read_address, NULL))
    {
        return false;
    }
    return ad5422_transact(target_index, AD5422_ADDR_NOP, 0u, out_echo);
}

/** @brief Convert an engineering-unit value (volts or mA, per @p range) to a clamped 16-bit DAC code. */
static uint16_t ad5422_code_from_value(const ad5422_range_entry_t *range, double value)
{
    double normalized;

    if (range->current_mode)
    {
        normalized = (value - range->current_offset_ma) / range->full_scale_ma;
    }
    else if (range->bipolar)
    {
        normalized = (value + (range->gain * AD5422_VREFIN) / 2.0) / (range->gain * AD5422_VREFIN);
    }
    else
    {
        normalized = value / (range->gain * AD5422_VREFIN);
    }
    if (normalized < 0.0)
    {
        normalized = 0.0;
    }
    else if (normalized > 1.0)
    {
        normalized = 1.0;
    }
    return (uint16_t)(normalized * 65535.0 + 0.5);
}

telemetry_driver_result_t native_ad5422_start(telemetry_channel_t *channel)
{
    const native_channel_source_t *source = ad5422_source(channel);
    uint16_t control;

    if (source == NULL || !native_ad5422_source_is_valid(source->source_index, source->source_flags))
    {
        return TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT;
    }
    if (!g_bus_bound)
    {
        return TELEMETRY_DRIVER_RESULT_INVALID_STATE;
    }
    /* Configure with output disabled before loading the initial DAC code. */
    control = (uint16_t)(AD5422_CTRL_DCEN | source->source_flags);
    if (!ad5422_transact(source->source_index, AD5422_ADDR_CONTROL, control, NULL))
    {
        return TELEMETRY_DRIVER_RESULT_TIMEOUT;
    }
    /* Command zero-scale as the safe initial output. */
    if (!ad5422_transact(source->source_index, AD5422_ADDR_DATA, 0u, NULL))
    {
        return TELEMETRY_DRIVER_RESULT_TIMEOUT;
    }
    if (!ad5422_transact(source->source_index, AD5422_ADDR_CONTROL,
                         (uint16_t)(control | AD5422_CTRL_OUTEN), NULL))
    {
        (void)ad5422_transact(source->source_index, AD5422_ADDR_CONTROL, control, NULL);
        return TELEMETRY_DRIVER_RESULT_TIMEOUT;
    }
    channel->runtime.value.f64 = 0.0;
    return TELEMETRY_DRIVER_RESULT_OK;
}

telemetry_driver_result_t native_ad5422_stop(telemetry_channel_t *channel)
{
    const native_channel_source_t *source = ad5422_source(channel);
    uint16_t control;

    if (source == NULL || !native_ad5422_source_is_valid(source->source_index, source->source_flags))
    {
        return TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT;
    }
    if (!g_bus_bound)
    {
        return TELEMETRY_DRIVER_RESULT_INVALID_STATE;
    }
    /* OUTEN=0: real safe-state action, not a no-op -- disables the output. */
    control = (uint16_t)(AD5422_CTRL_DCEN | source->source_flags);
    if (!ad5422_transact(source->source_index, AD5422_ADDR_CONTROL, control, NULL))
    {
        return TELEMETRY_DRIVER_RESULT_TIMEOUT;
    }
    return TELEMETRY_DRIVER_RESULT_OK;
}

telemetry_driver_result_t native_ad5422_write(telemetry_channel_t *channel, const telemetry_channel_value_t *value)
{
    const native_channel_source_t *source = ad5422_source(channel);
    const ad5422_range_entry_t *range;
    double engineering_value;
    uint16_t code;

    if (source == NULL || value == NULL || !native_ad5422_source_is_valid(source->source_index, source->source_flags) ||
        (channel->metadata.value_type != TELEMETRY_CHANNEL_VALUE_FLOAT &&
         channel->metadata.value_type != TELEMETRY_CHANNEL_VALUE_DOUBLE))
    {
        return TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT;
    }
    if (!g_bus_bound)
    {
        return TELEMETRY_DRIVER_RESULT_INVALID_STATE;
    }
    range = ad5422_range_for(source->source_flags);
    if (range == NULL)
    {
        return TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT;
    }
    engineering_value =
        channel->metadata.value_type == TELEMETRY_CHANNEL_VALUE_FLOAT ? (double)value->f32 : value->f64;
    code = ad5422_code_from_value(range, engineering_value);
    if (!ad5422_transact(source->source_index, AD5422_ADDR_DATA, code, NULL))
    {
        return TELEMETRY_DRIVER_RESULT_TIMEOUT;
    }
    if (channel->metadata.value_type == TELEMETRY_CHANNEL_VALUE_FLOAT)
    {
        channel->runtime.value.f32 = (float)engineering_value;
    }
    else
    {
        channel->runtime.value.f64 = engineering_value;
    }
    return TELEMETRY_DRIVER_RESULT_OK;
}

telemetry_driver_result_t native_ad5422_self_test(telemetry_channel_t *channel)
{
    const native_channel_source_t *source = ad5422_source(channel);
    uint16_t control = 0u;
    uint16_t status = 0u;

    if (source == NULL || !native_ad5422_source_is_valid(source->source_index, source->source_flags))
    {
        return TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT;
    }
    if (!g_bus_bound)
    {
        return TELEMETRY_DRIVER_RESULT_INVALID_STATE;
    }
    if (!ad5422_read_register(source->source_index, AD5422_READ_ADDR_CONTROL, &control) ||
        !ad5422_read_register(source->source_index, AD5422_READ_ADDR_STATUS, &status))
    {
        return TELEMETRY_DRIVER_RESULT_TIMEOUT;
    }
    /* Healthy status alone may be zero. CONTROL must contain the nonzero
     * DCEN/OUTEN signature set by start, so absent SDO cannot pass. */
    if ((control == 0u && status == 0u) || (control == UINT16_MAX && status == UINT16_MAX))
    {
        return TELEMETRY_DRIVER_RESULT_TIMEOUT;
    }
    if (control != (uint16_t)(AD5422_CTRL_OUTEN | AD5422_CTRL_DCEN | source->source_flags) ||
        (status & (uint16_t)~0x0002u) != 0u)
    {
        /* Status bit 1 (slew active) is informational; faults/reserved
         * bits or a changed control register fail deterministically. */
        return TELEMETRY_DRIVER_RESULT_HARDWARE_FAILURE;
    }
    return TELEMETRY_DRIVER_RESULT_OK;
}
