/**
 * @file
 * @brief Native channel driver for the two `AD5422` industrial analog
 *        output channels.
 *
 * See drivers/include/drivers/ad5422_driver.h for the full contract and
 * docs/datasheets/AD5422-register-notes.md for the register/command
 * reference this implementation follows.
 *
 * Ownership: the SPI bus binding (native_ad5422_bind_bus()), the
 * board-level binding flag and the g_chain_ready latch are process-wide
 * state -- both AO0 and AO1 share one daisy-chained bus and one
 * AO_ENABLE_REQ line, so chain startup runs once, not once per channel.
 * Nothing is ever read back (the board has no SDO path to the MCU), so no
 * per-channel DAC cache is required either.
 */
#include "drivers/ad5422_driver.h"
#include "drivers/industrial_board_pins.h"

#include "hardware/gpio.h"
#include "hardware/timer.h"

#include <stddef.h>
#include <string.h>

#define AD5422_ADDR_NOP 0x00u
#define AD5422_ADDR_DATA 0x01u
#define AD5422_ADDR_CONTROL 0x55u
#define AD5422_ADDR_RESET 0x56u

#define AD5422_RESET_DATA 0x0001u

#define AD5422_CTRL_REXT (1u << 13)
#define AD5422_CTRL_OUTEN (1u << 12)
#define AD5422_CTRL_DCEN (1u << 5)

#define AD5422_AO1_INDEX 1u /**< Far device in the chain -- transmitted first. */
#define AD5422_AO0_INDEX 0u /**< Near device in the chain -- transmitted second. */
#define AD5422_CHANNEL_COUNT 2u

#define AD5422_VREFIN 5.0

/**
 * @brief Busy-wait after every chain-startup frame, in microseconds.
 *
 * Covers the two datasheet minimums that apply there: 40 us after a
 * software reset "to ensure correct calibration and to allow the internal
 * reference to settle", and the 5 us `LATCH` high time a device still in
 * standalone mode needs between frames (t3). 100 us leaves margin over
 * both; four frames cost well under a millisecond of SensorTask time, once
 * per binding.
 */
#define AD5422_STARTUP_SETTLE_US 100u

typedef struct
{
    uint8_t code;
    bool current_mode;
    double gain;              /**< Voltage ranges only. */
    double full_scale_ma;     /**< Current ranges only. */
    double current_offset_ma; /**< Current ranges only (4-20 mA's 4 mA floor). */
} ad5422_range_entry_t;

/**
 * @brief The `R[2:0]` codes `RP2350_INDUSTRIAL_MVP_V1` qualifies
 *        (`HARDWARE_MVP.md` section 3.2): 0-10 V and 4-20 mA only.
 *
 * The silicon has five more codes (docs/datasheets/AD5422-register-notes.md)
 * that this board does not offer: the bipolar voltage ranges need a
 * negative analog rail and `AVSS` is tied to `AGND`, and 0-5 V, 0-20 mA and
 * 0-24 mA are outside the frozen board contract. They are rejected, not
 * merely undocumented, so no configuration can command them.
 */
static const ad5422_range_entry_t g_ranges[] = {
    {0x1u, false, 2.0, 0.0, 0.0},  /* 0 V to 10 V */
    {0x5u, true, 0.0, 16.0, 4.0},  /* 4 mA to 20 mA */
};

static spi_bus_t g_bus;
static bool g_bus_bound;
/** @brief Set once both DACs are reset, daisy-chained and AO_ENABLE_REQ is raised; see ad5422_start_chain(). */
static bool g_chain_ready;

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
    /* A (re)binding is a new board context: the chain must be brought up
     * again before any output is trusted. */
    g_chain_ready = false;
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
 *
 * Write-only: `AO1`'s `SDO` is not connected to `BOARD_SPI_MISO`
 * (`HARDWARE_MVP.md` section 3.2.2) and `AO0`'s feeds `AO1`, so nothing
 * clocked back during the transfer comes from either DAC.
 */
static bool ad5422_transact(uint8_t target_index, uint8_t address, uint16_t data)
{
    uint8_t tx[6];
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
    return spi_bus_transfer_framed(&g_bus, tx, NULL, sizeof(tx));
}

/**
 * @brief Send one 24-bit single-word frame.
 *
 * Only for chain startup, while `AO0` may still be in standalone mode:
 * there the datasheet requires exactly 24 SCLK edges before `LATCH` rises,
 * so the 48-bit two-device frame would not be a valid write to it.
 */
static bool ad5422_transact_single(uint8_t address, uint16_t data)
{
    uint8_t tx[3];

    ad5422_pack_word(address, data, tx);
    return spi_bus_transfer_framed(&g_bus, tx, NULL, sizeof(tx));
}

/**
 * @brief One-time chain startup: reset both DACs, enable daisy chaining on
 *        both, then release the hardware CLEAR by raising `AO_ENABLE_REQ`.
 *
 * `HARDWARE_MVP.md` section "Required AO firmware contract": perform a
 * software RESET after power-up, because `DVCC` (`+3V3_DIG`) and `AVDD`
 * (`+15V_AO`) sequencing is not guaranteed and the power-on calibration
 * read may have been wrong; configure daisy-chain operation before normal
 * 48-clock updates; and keep `AO_ENABLE_REQ` low until both DACs are reset
 * and configured with safe codes loaded.
 *
 * 1. `AO_ENABLE_REQ` driven low (it is already held low by its pull-down;
 *    driving it makes a re-run after rebinding safe too). Both `CLEAR`
 *    inputs stay asserted for the whole sequence.
 * 2. 24-bit RESET to `AO0` -- valid whether `AO0` is in standalone mode
 *    (power-up) or still daisy-chained (warm MCU restart): either way its
 *    own shift register holds exactly this word.
 * 3. 24-bit `CONTROL = DCEN` to `AO0`: its `SDO` now forwards to `AO1`.
 * 4. 48-bit `[RESET][NOP]`: `AO1` resets, `AO0` sees `NOP`.
 * 5. 48-bit `[CONTROL = DCEN][NOP]`: `AO1` joins the chain, so every later
 *    48-bit frame is a valid chain write for both devices.
 * 6. `AO_ENABLE_REQ` high.
 *
 * Frames sent before step 4 may leave arbitrary data in `AO1` (its `SDIN`
 * is undriven while `AO0` is standalone, or carries `AO0`'s stale word after
 * a warm restart). That is harmless: `CLEAR` holds the outputs at their
 * clear level, and step 4 resets `AO1` afterwards. After the reset every
 * register is zero, so both outputs are disabled (`OUTEN = 0`) with a zero
 * DAC code when `CLEAR` is released; only a channel's start() enables one.
 *
 * `AO1` in steps 4-5 relies on the datasheet's "the last 24 bits to be
 * clocked in before the rising edge of LATCH is the data that is latched",
 * since a standalone device also sees the 48 clocks of that frame. No
 * sequence can avoid that for the far device of a chain, so bring-up must
 * confirm it (`docs/releases/v0.5.1-hardware-bring-up-checklists.md`).
 * @return TELEMETRY_DRIVER_RESULT_OK on success; TELEMETRY_DRIVER_RESULT_TIMEOUT
 *         if a transfer fails, with `AO_ENABLE_REQ` left low.
 */
static telemetry_driver_result_t ad5422_start_chain(void)
{
    gpio_init(INDUSTRIAL_AO_ENABLE_GPIO);
    gpio_disable_pulls(INDUSTRIAL_AO_ENABLE_GPIO);
    gpio_put(INDUSTRIAL_AO_ENABLE_GPIO, false);
    gpio_set_dir(INDUSTRIAL_AO_ENABLE_GPIO, GPIO_OUT);

    if (!ad5422_transact_single(AD5422_ADDR_RESET, AD5422_RESET_DATA))
    {
        return TELEMETRY_DRIVER_RESULT_TIMEOUT;
    }
    busy_wait_us_32(AD5422_STARTUP_SETTLE_US);
    if (!ad5422_transact_single(AD5422_ADDR_CONTROL, AD5422_CTRL_DCEN))
    {
        return TELEMETRY_DRIVER_RESULT_TIMEOUT;
    }
    busy_wait_us_32(AD5422_STARTUP_SETTLE_US);
    if (!ad5422_transact(AD5422_AO1_INDEX, AD5422_ADDR_RESET, AD5422_RESET_DATA))
    {
        return TELEMETRY_DRIVER_RESULT_TIMEOUT;
    }
    busy_wait_us_32(AD5422_STARTUP_SETTLE_US);
    if (!ad5422_transact(AD5422_AO1_INDEX, AD5422_ADDR_CONTROL, AD5422_CTRL_DCEN))
    {
        return TELEMETRY_DRIVER_RESULT_TIMEOUT;
    }
    busy_wait_us_32(AD5422_STARTUP_SETTLE_US);

    gpio_put(INDUSTRIAL_AO_ENABLE_GPIO, true);
    return TELEMETRY_DRIVER_RESULT_OK;
}

/**
 * @brief Whether @p value (volts or mA, per @p range) lies inside the range
 *        the output can produce: 0..10 V, or 4..20 mA.
 *
 * Written so NaN fails too. An actuator must not quietly substitute the
 * nearest producible value and report success: the caller would believe
 * the commanded value is on the terminal.
 */
static bool ad5422_value_in_range(const ad5422_range_entry_t *range, double value)
{
    const double minimum = range->current_mode ? range->current_offset_ma : 0.0;
    const double maximum =
        range->current_mode ? range->current_offset_ma + range->full_scale_ma : range->gain * AD5422_VREFIN;

    return value >= minimum && value <= maximum;
}

/** @brief Convert an in-range engineering-unit value (see ad5422_value_in_range()) to its 16-bit DAC code. */
static uint16_t ad5422_code_from_value(const ad5422_range_entry_t *range, double value)
{
    const double normalized = range->current_mode ? (value - range->current_offset_ma) / range->full_scale_ma
                                                  : value / (range->gain * AD5422_VREFIN);

    return (uint16_t)(normalized * 65535.0 + 0.5);
}

/**
 * @brief CONTROL word with the output disabled: daisy chain on, range
 *        selected, everything else 0.
 *
 * `REXT` is deliberately 0 here and set only in the word that also sets
 * `OUTEN` (ad5422_control_enabled()): the datasheet (Table 16) recommends
 * setting `REXT` only together with `OUTEN`, and the alternative -- `REXT`
 * set ahead of `OUTEN` -- requires the range to change on the enabling
 * write, which this driver's sequence does not do. While `OUTEN = 0` both
 * outputs are tristated, so the resistor selection has no effect.
 * `CLRSEL`, `OVRRNG` and `SREN` stay 0, as `HARDWARE_MVP.md` section 3.2.3
 * requires for the hardware CLEAR safe state.
 */
static uint16_t ad5422_control_disabled(uint8_t range_code)
{
    return (uint16_t)(AD5422_CTRL_DCEN | range_code);
}

/**
 * @brief CONTROL word with the output enabled: `OUTEN | REXT | DCEN | R`.
 *
 * `REXT = 1` selects the board's external current-setting resistor
 * (`RSET` pin -> 15.0 kOhm 0.1% to `AGND`); `HARDWARE_MVP.md` section 3.2.1
 * requires it. It is set for the voltage range too: the board has no
 * internal-resistor configuration, and the bit only affects the current
 * output stage.
 */
static uint16_t ad5422_control_enabled(uint8_t range_code)
{
    return (uint16_t)(AD5422_CTRL_OUTEN | AD5422_CTRL_REXT | ad5422_control_disabled(range_code));
}

telemetry_driver_result_t native_ad5422_start(telemetry_channel_t *channel)
{
    const native_channel_source_t *source = ad5422_source(channel);

    if (source == NULL || !native_ad5422_source_is_valid(source->source_index, source->source_flags))
    {
        return TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT;
    }
    if (!g_bus_bound)
    {
        return TELEMETRY_DRIVER_RESULT_INVALID_STATE;
    }
    /* AO0 and AO1 share one chain and one AO_ENABLE_REQ, so the chain comes
     * up once, whichever channel starts first. A failure is not latched. */
    if (!g_chain_ready)
    {
        telemetry_driver_result_t chain_result = ad5422_start_chain();

        if (chain_result != TELEMETRY_DRIVER_RESULT_OK)
        {
            return chain_result;
        }
        g_chain_ready = true;
    }
    /* Select the range with the output disabled before loading the initial
     * DAC code, so a range change never glitches a live output. */
    if (!ad5422_transact(source->source_index, AD5422_ADDR_CONTROL, ad5422_control_disabled(source->source_flags)))
    {
        return TELEMETRY_DRIVER_RESULT_TIMEOUT;
    }
    /* Command zero-scale as the safe initial output. */
    if (!ad5422_transact(source->source_index, AD5422_ADDR_DATA, 0u))
    {
        return TELEMETRY_DRIVER_RESULT_TIMEOUT;
    }
    if (!ad5422_transact(source->source_index, AD5422_ADDR_CONTROL, ad5422_control_enabled(source->source_flags)))
    {
        (void)ad5422_transact(source->source_index, AD5422_ADDR_CONTROL,
                              ad5422_control_disabled(source->source_flags));
        return TELEMETRY_DRIVER_RESULT_TIMEOUT;
    }
    channel->runtime.value.f64 = 0.0;
    return TELEMETRY_DRIVER_RESULT_OK;
}

telemetry_driver_result_t native_ad5422_stop(telemetry_channel_t *channel)
{
    const native_channel_source_t *source = ad5422_source(channel);

    if (source == NULL || !native_ad5422_source_is_valid(source->source_index, source->source_flags))
    {
        return TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT;
    }
    if (!g_bus_bound)
    {
        return TELEMETRY_DRIVER_RESULT_INVALID_STATE;
    }
    if (!g_chain_ready)
    {
        /* Nothing this binding could have enabled: the chain never came up,
         * so AO_ENABLE_REQ is still low and CLEAR holds both outputs. */
        return TELEMETRY_DRIVER_RESULT_OK;
    }
    /* OUTEN=0: real safe-state action, not a no-op -- disables the output.
     * DCEN stays set so the far device (AO1) remains reachable; REXT is
     * cleared with OUTEN, see ad5422_control_disabled(). */
    if (!ad5422_transact(source->source_index, AD5422_ADDR_CONTROL, ad5422_control_disabled(source->source_flags)))
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
    if (!g_bus_bound || !g_chain_ready)
    {
        /* Before chain startup a 48-bit frame is not a valid write to a
         * device that may still be in standalone mode. */
        return TELEMETRY_DRIVER_RESULT_INVALID_STATE;
    }
    range = ad5422_range_for(source->source_flags);
    if (range == NULL)
    {
        return TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT;
    }
    engineering_value =
        channel->metadata.value_type == TELEMETRY_CHANNEL_VALUE_FLOAT ? (double)value->f32 : value->f64;
    if (!ad5422_value_in_range(range, engineering_value))
    {
        /* Nothing is sent and runtime.value keeps the last applied value. */
        return TELEMETRY_DRIVER_RESULT_OUT_OF_RANGE;
    }
    code = ad5422_code_from_value(range, engineering_value);
    if (!ad5422_transact(source->source_index, AD5422_ADDR_DATA, code))
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
