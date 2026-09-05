/**
 * @file
 * @brief Native channel driver for the `ADS8688` industrial SPI ADC.
 *
 * See drivers/include/drivers/ads8688_driver.h for the full contract and
 * docs/datasheets/ADS8688-register-notes.md for the register/command
 * reference this implementation follows.
 *
 * Ownership: the SPI bus binding (native_ads8688_bind_bus()) is process-
 * wide file-static state -- one physical `ADS8688` chip is in scope for
 * this release; every channel using this driver shares it. Per-channel
 * state is limited to `native_channel_source_t` in `driver_context`, like
 * every other native driver.
 */
#include "drivers/ads8688_driver.h"

#include <stddef.h>

#define ADS8688_REG_RANGE_BASE 0x05u
#define ADS8688_CMD_MAN_CH_BASE 0xC000u
#define ADS8688_MAX_CHANNEL_INDEX 7u

typedef struct
{
    uint8_t code;
    bool bipolar;
    double full_scale_volts; /**< Bipolar: +/- this magnitude. Unipolar: 0 to this magnitude. */
} ads8688_range_entry_t;

/** @brief The five valid Range_CHn[3:0] codes (docs/datasheets/ADS8688-register-notes.md), VREF = 4.096 V internal reference. */
static const ads8688_range_entry_t g_ranges[] = {
    {0x0u, true, 10.24},
    {0x1u, true, 5.12},
    {0x2u, true, 2.56},
    {0x5u, false, 10.24},
    {0x6u, false, 5.12},
};

static spi_bus_t g_bus;
static bool g_bus_bound;

void native_ads8688_bind_bus(const spi_bus_t *bus)
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

static const ads8688_range_entry_t *ads8688_range_for(uint8_t code)
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

bool native_ads8688_source_is_valid(uint8_t source_index, uint8_t source_flags)
{
    return source_index <= ADS8688_MAX_CHANNEL_INDEX && ads8688_range_for(source_flags) != NULL;
}

/** @brief Reinterpret channel->driver_context as this driver's source selector, or NULL. */
static const native_channel_source_t *ads8688_source(const telemetry_channel_t *channel)
{
    return channel != NULL ? (const native_channel_source_t *)channel->driver_context : NULL;
}

static uint16_t ads8688_man_ch_command(uint8_t channel_index)
{
    return (uint16_t)(ADS8688_CMD_MAN_CH_BASE | ((uint16_t)channel_index << 10));
}

/** @brief 24-bit (3-byte) program-register write: ADDR[6:0]|WR=1, DATA, dummy (clocks out the echo). */
static bool ads8688_write_register(uint8_t address, uint8_t data, uint8_t *out_echo)
{
    uint8_t tx[3];
    uint8_t rx[3] = {0};

    tx[0] = (uint8_t)((address << 1) | 0x01u);
    tx[1] = data;
    tx[2] = 0x00u;
    if (!spi_bus_transfer_framed(&g_bus, tx, rx, sizeof(tx)))
    {
        return false;
    }
    if (out_echo != NULL)
    {
        *out_echo = rx[2];
    }
    return true;
}

/** @brief 16-bit (2-byte) full-duplex command-register transfer; @p out_result receives the previous frame's pipelined conversion result. */
static bool ads8688_command(uint16_t command, uint16_t *out_result)
{
    uint8_t tx[2];
    uint8_t rx[2] = {0};

    tx[0] = (uint8_t)(command >> 8);
    tx[1] = (uint8_t)(command & 0xFFu);
    if (!spi_bus_transfer_framed(&g_bus, tx, rx, sizeof(tx)))
    {
        return false;
    }
    if (out_result != NULL)
    {
        *out_result = (uint16_t)(((uint16_t)rx[0] << 8) | rx[1]);
    }
    return true;
}

telemetry_driver_result_t native_ads8688_start(telemetry_channel_t *channel)
{
    const native_channel_source_t *source = ads8688_source(channel);
    uint8_t echo = 0u;

    if (source == NULL || !native_ads8688_source_is_valid(source->source_index, source->source_flags))
    {
        return TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT;
    }
    if (!g_bus_bound)
    {
        return TELEMETRY_DRIVER_RESULT_INVALID_STATE;
    }
    if (!ads8688_write_register((uint8_t)(ADS8688_REG_RANGE_BASE + source->source_index), source->source_flags,
                                &echo))
    {
        return TELEMETRY_DRIVER_RESULT_TIMEOUT;
    }
    if (echo != source->source_flags)
    {
        return TELEMETRY_DRIVER_RESULT_HARDWARE_FAILURE;
    }
    /* Select this channel for conversion so the first native_ads8688_read()
     * call's priming frame already targets it. */
    if (!ads8688_command(ads8688_man_ch_command(source->source_index), NULL))
    {
        return TELEMETRY_DRIVER_RESULT_TIMEOUT;
    }
    return TELEMETRY_DRIVER_RESULT_OK;
}

telemetry_driver_result_t native_ads8688_stop(telemetry_channel_t *channel)
{
    (void)channel;
    return TELEMETRY_DRIVER_RESULT_OK;
}

telemetry_driver_result_t native_ads8688_read(telemetry_channel_t *channel, uint32_t now_ms)
{
    const native_channel_source_t *source = ads8688_source(channel);
    const ads8688_range_entry_t *range;
    uint16_t raw = 0u;
    uint16_t command;
    double voltage;

    (void)now_ms;
    if (source == NULL || !native_ads8688_source_is_valid(source->source_index, source->source_flags) ||
        (channel->metadata.value_type != TELEMETRY_CHANNEL_VALUE_FLOAT &&
         channel->metadata.value_type != TELEMETRY_CHANNEL_VALUE_DOUBLE))
    {
        return TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT;
    }
    if (!g_bus_bound)
    {
        return TELEMETRY_DRIVER_RESULT_INVALID_STATE;
    }
    range = ads8688_range_for(source->source_flags);
    if (range == NULL)
    {
        return TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT;
    }
    command = ads8688_man_ch_command(source->source_index);
    /* One-frame conversion pipeline delay (docs/datasheets/ADS8688-register-notes.md):
     * select this channel twice in succession so the second frame's result
     * is guaranteed to belong to this channel, regardless of which channel
     * was selected by a previous read() on a different channel. */
    if (!ads8688_command(command, NULL) || !ads8688_command(command, &raw))
    {
        return TELEMETRY_DRIVER_RESULT_TIMEOUT;
    }
    if (range->bipolar)
    {
        voltage = ((double)(int16_t)raw / 32768.0) * range->full_scale_volts;
    }
    else
    {
        voltage = ((double)raw / 65536.0) * range->full_scale_volts;
    }
    voltage = voltage * channel->settings.scale + channel->settings.offset;
    if (channel->metadata.value_type == TELEMETRY_CHANNEL_VALUE_FLOAT)
    {
        channel->runtime.value.f32 = (float)voltage;
    }
    else
    {
        channel->runtime.value.f64 = voltage;
    }
    return TELEMETRY_DRIVER_RESULT_OK;
}

telemetry_driver_result_t native_ads8688_self_test(telemetry_channel_t *channel)
{
    const native_channel_source_t *source = ads8688_source(channel);
    uint8_t address;
    uint8_t echo_a = 0u;
    uint8_t echo_b = 0u;
    uint8_t echo_restore = 0u;
    const uint8_t probe_a = 0x00u;
    const uint8_t probe_b = 0x01u;
    telemetry_driver_result_t result;

    if (source == NULL || !native_ads8688_source_is_valid(source->source_index, source->source_flags))
    {
        return TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT;
    }
    if (!g_bus_bound)
    {
        return TELEMETRY_DRIVER_RESULT_INVALID_STATE;
    }
    address = (uint8_t)(ADS8688_REG_RANGE_BASE + source->source_index);
    /* Two distinct, both-valid range codes: if the echo never tracks which
     * one was written, no live chip is responding on SDO. */
    if (!ads8688_write_register(address, probe_a, &echo_a) || !ads8688_write_register(address, probe_b, &echo_b))
    {
        result = TELEMETRY_DRIVER_RESULT_TIMEOUT;
    }
    else if (echo_a == echo_b)
    {
        /* Constant readback regardless of what was written -- the
         * characteristic signature of a floating/absent SDO line, i.e.
         * "supported but hardware unavailable/not present" per
         * ROADMAP.md's self-test requirement. */
        result = TELEMETRY_DRIVER_RESULT_TIMEOUT;
    }
    else if (echo_a != probe_a || echo_b != probe_b)
    {
        result = TELEMETRY_DRIVER_RESULT_HARDWARE_FAILURE;
    }
    else
    {
        result = TELEMETRY_DRIVER_RESULT_OK;
    }
    /* Best-effort restore of the channel's actual configured range regardless of outcome. */
    (void)ads8688_write_register(address, source->source_flags, &echo_restore);
    return result;
}
