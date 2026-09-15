/**
 * @file
 * @brief Host tests for the real drivers/src/ad5422_driver.c against a
 *        fake two-device daisy chain modeled at the shift-register level:
 *        each DAC has a 24-bit input shift register, `AO0`'s `SDO` feeds
 *        `AO1` only while `AO0.DCEN` is set (otherwise `AO1` sees an
 *        undriven line), and every `LATCH` rising edge makes each device
 *        latch the last 24 bits it holds. A standalone `AO0` that receives
 *        anything but exactly 24 clocks rejects the frame, as the datasheet
 *        requires. That proves the chain startup (resets, daisy-chain
 *        enable, `AO_ENABLE_REQ`), the 48-bit two-device framing (NOP for
 *        the untouched device), the CONTROL sequence the board requires
 *        (`REXT` only together with `OUTEN`, safe state on stop), the board's
 *        range contract and voltage/current scaling -- not just that the
 *        vtable dispatches.
 */
#include "drivers/ad5422_driver.h"
#include "drivers/industrial_board_pins.h"

#include "hardware/gpio.h"
#include "hardware/timer.h"

#include <assert.h>
#include <math.h>
#include <string.h>

#define AO0_INDEX 0u
#define AO1_INDEX 1u

#define CTRL_REXT (1u << 13)
#define CTRL_OUTEN (1u << 12)
#define CTRL_DCEN (1u << 5)

#define CONTROL_HISTORY 8u
#define EVENT_LOG 64u

/** Frames one chain startup costs: 24-bit RESET, 24-bit DCEN to AO0, 48-bit RESET, 48-bit DCEN to AO1. */
#define CHAIN_STARTUP_TRANSFERS 4u

typedef enum
{
    EVENT_TRANSFER,
    EVENT_WAIT,
    EVENT_ENABLE_LEVEL,
} event_kind_t;

typedef struct
{
    event_kind_t kind;
    unsigned value; /**< Frame length in bytes, wait in us, or AO_ENABLE_REQ level. */
} event_t;

typedef struct
{
    uint8_t shift[3];
    uint16_t data_reg;
    uint16_t control_reg;
    uint16_t control_history[CONTROL_HISTORY];
    unsigned control_writes;       /**< Since the last reset. */
    unsigned data_writes_before_enable;
    unsigned resets;
} fake_dac_t;

typedef struct
{
    fake_dac_t dac[2];
    uint8_t undriven_bytes[3]; /**< What AO1 clocks in while AO0's SDO is not driving. */
    bool force_transfer_failure;
    unsigned transfers;
    unsigned fail_transfer;
    unsigned ao0_rejected_frames;
    bool rx_requested;
    event_t events[EVENT_LOG];
    unsigned event_count;
    bool enable_is_output;
    bool enable_level;
} fake_chain_t;

static fake_chain_t g_chip;

static void log_event(event_kind_t kind, unsigned value)
{
    if (g_chip.event_count < EVENT_LOG)
    {
        g_chip.events[g_chip.event_count++] = (event_t){kind, value};
    }
}

void gpio_init(unsigned int gpio)
{
    if (gpio == INDUSTRIAL_AO_ENABLE_GPIO)
    {
        g_chip.enable_is_output = false;
    }
}

void gpio_set_dir(unsigned int gpio, bool out)
{
    if (gpio == INDUSTRIAL_AO_ENABLE_GPIO)
    {
        g_chip.enable_is_output = out;
    }
}

void gpio_put(unsigned int gpio, bool value)
{
    assert(gpio == INDUSTRIAL_AO_ENABLE_GPIO);
    g_chip.enable_level = value;
    log_event(EVENT_ENABLE_LEVEL, value ? 1u : 0u);
}

void gpio_disable_pulls(unsigned int gpio)
{
    (void)gpio;
}

void busy_wait_us_32(uint32_t delay_us)
{
    log_event(EVENT_WAIT, delay_us);
}

/** @brief AO_ENABLE_REQ as the board sees it: the 100k pull-down wins unless the pin drives high. */
static bool ao_enable_requested(void)
{
    return g_chip.enable_is_output && g_chip.enable_level;
}

static void chip_reset(void)
{
    memset(&g_chip, 0, sizeof(g_chip));
}

static void dac_reset(fake_dac_t *dac)
{
    unsigned resets = dac->resets + 1u;

    memset(dac, 0, sizeof(*dac));
    dac->resets = resets;
}

static void dac_latch(fake_dac_t *dac)
{
    uint8_t address = dac->shift[0];
    uint16_t data = (uint16_t)(((uint16_t)dac->shift[1] << 8) | dac->shift[2]);

    if (address == 0x01u)
    {
        dac->data_reg = data;
        if ((dac->control_reg & CTRL_OUTEN) == 0u)
        {
            ++dac->data_writes_before_enable;
        }
    }
    else if (address == 0x55u)
    {
        dac->control_reg = data;
        if (dac->control_writes < CONTROL_HISTORY)
        {
            dac->control_history[dac->control_writes] = data;
        }
        ++dac->control_writes;
    }
    else if (address == 0x56u && (data & 0x1u) != 0u)
    {
        dac_reset(dac);
    }
    /* address 0x00 (NOP) and anything else: registers untouched. */
}

/** @brief Clock one byte into @p dac; returns the byte that left its shift register. */
static uint8_t dac_clock_byte(fake_dac_t *dac, uint8_t in)
{
    uint8_t out = dac->shift[0];

    dac->shift[0] = dac->shift[1];
    dac->shift[1] = dac->shift[2];
    dac->shift[2] = in;
    return out;
}

static bool fake_transfer(void *context, const uint8_t *tx, uint8_t *rx, size_t length)
{
    fake_chain_t *chip = (fake_chain_t *)context;
    fake_dac_t *ao0 = &chip->dac[AO0_INDEX];
    fake_dac_t *ao1 = &chip->dac[AO1_INDEX];
    bool ao0_chained = (ao0->control_reg & CTRL_DCEN) != 0u;
    size_t index;

    log_event(EVENT_TRANSFER, (unsigned)length);
    if (rx != NULL)
    {
        chip->rx_requested = true;
    }
    if (++chip->transfers == chip->fail_transfer || chip->force_transfer_failure)
    {
        return false;
    }
    /* CLEAR is never released onto DACs that have not both been reset. */
    assert(!ao_enable_requested() || (ao0->resets > 0u && ao1->resets > 0u));
    for (index = 0u; index < length; ++index)
    {
        uint8_t forwarded = dac_clock_byte(ao0, tx[index]);

        (void)dac_clock_byte(ao1, ao0_chained ? forwarded : chip->undriven_bytes[index % 3u]);
    }
    /* LATCH rising edge. */
    if (!ao0_chained && length != 3u)
    {
        ++chip->ao0_rejected_frames; /* standalone: exactly 24 clocks or the data is invalid */
    }
    else
    {
        dac_latch(ao0);
    }
    dac_latch(ao1);
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

static spi_bus_t g_bus = {.vtbl = &g_fake_vtbl, .context = &g_chip};

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

/** @brief Every CONTROL word either device has latched since its reset sets REXT exactly when it sets OUTEN. */
static void assert_rext_only_with_outen(void)
{
    unsigned device;
    unsigned index;

    for (device = 0u; device < 2u; ++device)
    {
        for (index = 0u; index < g_chip.dac[device].control_writes && index < CONTROL_HISTORY; ++index)
        {
            const uint16_t word = g_chip.dac[device].control_history[index];

            assert(((word & CTRL_REXT) != 0u) == ((word & CTRL_OUTEN) != 0u));
        }
    }
}

static void test_source_validation_is_the_board_range_contract(void)
{
    unsigned code;

    /* HARDWARE_MVP.md section 3.2: 0-10 V and 4-20 mA only. */
    for (code = 0u; code <= 0xFFu; ++code)
    {
        bool expected = code == 0x1u || code == 0x5u;

        assert(native_ad5422_source_is_valid(AO0_INDEX, (uint8_t)code) == expected);
        assert(native_ad5422_source_is_valid(AO1_INDEX, (uint8_t)code) == expected);
    }
    assert(!native_ad5422_source_is_valid(2u, 0x1u));
}

static void test_unsupported_ranges_are_never_sent(void)
{
    /* 0-5 V, +/-5 V, +/-10 V, reserved, 0-20 mA, 0-24 mA. */
    static const uint8_t rejected[] = {0x0u, 0x2u, 0x3u, 0x4u, 0x6u, 0x7u};
    telemetry_channel_value_t value = {.f32 = 1.0f};
    size_t index;

    chip_reset();
    native_ad5422_bind_bus(&g_bus);
    for (index = 0u; index < sizeof(rejected) / sizeof(rejected[0]); ++index)
    {
        native_channel_source_t source = {.source_index = AO0_INDEX, .source_flags = rejected[index]};
        telemetry_channel_t channel = make_channel(&source, TELEMETRY_CHANNEL_VALUE_FLOAT);

        assert(native_ad5422_start(&channel) == TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT);
        assert(native_ad5422_stop(&channel) == TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT);
        assert(native_ad5422_write(&channel, &value) == TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT);
    }
    assert(g_chip.transfers == 0u && g_chip.event_count == 0u);
}

static void test_no_bus_bound(void)
{
    native_channel_source_t source = {.source_index = 0u, .source_flags = 0x1u};
    telemetry_channel_t channel = make_channel(&source, TELEMETRY_CHANNEL_VALUE_FLOAT);
    telemetry_channel_value_t value = {.f32 = 1.0f};

    native_ad5422_bind_bus(NULL);
    assert(native_ad5422_start(&channel) == TELEMETRY_DRIVER_RESULT_INVALID_STATE);
    assert(native_ad5422_stop(&channel) == TELEMETRY_DRIVER_RESULT_INVALID_STATE);
    assert(native_ad5422_write(&channel, &value) == TELEMETRY_DRIVER_RESULT_INVALID_STATE);
}

/** @brief HARDWARE_MVP.md's AO firmware contract, event by event, from power-up. */
static void test_chain_startup_from_power_up(void)
{
    static const event_t expected[] = {
        {EVENT_ENABLE_LEVEL, 0u}, /* AO_ENABLE_REQ driven low: CLEAR held */
        {EVENT_TRANSFER, 3u},     /* RESET -> AO0 (standalone: exactly 24 clocks) */
        {EVENT_WAIT, 0u},
        {EVENT_TRANSFER, 3u},     /* CONTROL = DCEN -> AO0 */
        {EVENT_WAIT, 0u},
        {EVENT_TRANSFER, 6u},     /* [RESET][NOP] -> AO1 */
        {EVENT_WAIT, 0u},
        {EVENT_TRANSFER, 6u},     /* [CONTROL = DCEN][NOP] -> AO1 */
        {EVENT_WAIT, 0u},
        {EVENT_ENABLE_LEVEL, 1u}, /* CLEAR released */
        {EVENT_TRANSFER, 6u},     /* the channel's own CONTROL (disabled) */
        {EVENT_TRANSFER, 6u},     /* DATA = 0 */
        {EVENT_TRANSFER, 6u},     /* CONTROL = OUTEN | REXT | DCEN | R */
    };
    native_channel_source_t source = {.source_index = AO1_INDEX, .source_flags = 0x5u};
    telemetry_channel_t channel = make_channel(&source, TELEMETRY_CHANNEL_VALUE_DOUBLE);
    unsigned index;

    chip_reset();
    /* AO1's SDIN is undriven until AO0 chains: let it float to a harmful
     * word (CONTROL with OUTEN on the 0-24 mA range). */
    g_chip.undriven_bytes[0] = 0x55u;
    g_chip.undriven_bytes[1] = 0x10u;
    g_chip.undriven_bytes[2] = 0x07u;
    native_ad5422_bind_bus(&g_bus);
    assert(native_ad5422_start(&channel) == TELEMETRY_DRIVER_RESULT_OK);

    assert(g_chip.event_count == sizeof(expected) / sizeof(expected[0]));
    for (index = 0u; index < g_chip.event_count; ++index)
    {
        assert(g_chip.events[index].kind == expected[index].kind);
        if (expected[index].kind == EVENT_WAIT)
        {
            /* >= 40 us reset settle; also covers the 5 us standalone LATCH high time. */
            assert(g_chip.events[index].value >= 40u);
        }
        else
        {
            assert(g_chip.events[index].value == expected[index].value);
        }
    }
    assert(g_chip.ao0_rejected_frames == 0u);
    assert(g_chip.dac[AO0_INDEX].resets == 1u && g_chip.dac[AO1_INDEX].resets == 1u);
    /* The floated word was wiped by AO1's reset; AO1 is chained, and its
     * only CONTROL words are the chain enable and its channel's start. */
    assert(g_chip.dac[AO1_INDEX].control_writes == 3u);
    assert(g_chip.dac[AO1_INDEX].control_history[0] == CTRL_DCEN);
    assert(g_chip.dac[AO1_INDEX].control_reg == (CTRL_OUTEN | CTRL_REXT | CTRL_DCEN | 0x5u));
    /* AO0 has no channel: reset, chained, output still disabled. */
    assert(g_chip.dac[AO0_INDEX].control_reg == CTRL_DCEN && g_chip.dac[AO0_INDEX].data_reg == 0u);
    assert(ao_enable_requested());
    assert_rext_only_with_outen();
}

/** @brief After an MCU-only restart both DACs are still chained and may hold stale state; startup converges anyway. */
static void test_chain_startup_after_warm_restart(void)
{
    native_channel_source_t source = {.source_index = AO0_INDEX, .source_flags = 0x1u};
    telemetry_channel_t channel = make_channel(&source, TELEMETRY_CHANNEL_VALUE_FLOAT);

    chip_reset();
    /* Previous firmware left both enabled on 4-20 mA with a live code, and
     * AO0's shift register still holding its last word. */
    g_chip.dac[AO0_INDEX].control_reg = CTRL_OUTEN | CTRL_REXT | CTRL_DCEN | 0x5u;
    g_chip.dac[AO1_INDEX].control_reg = CTRL_OUTEN | CTRL_REXT | CTRL_DCEN | 0x5u;
    g_chip.dac[AO0_INDEX].data_reg = 0x8000u;
    g_chip.dac[AO1_INDEX].data_reg = 0x8000u;
    g_chip.dac[AO0_INDEX].shift[0] = 0x01u;
    g_chip.dac[AO0_INDEX].shift[1] = 0xFFu;
    g_chip.dac[AO0_INDEX].shift[2] = 0xFFu;
    native_ad5422_bind_bus(&g_bus);
    assert(native_ad5422_start(&channel) == TELEMETRY_DRIVER_RESULT_OK);

    assert(g_chip.dac[AO0_INDEX].resets == 1u && g_chip.dac[AO1_INDEX].resets == 1u);
    assert(g_chip.dac[AO1_INDEX].control_reg == CTRL_DCEN && g_chip.dac[AO1_INDEX].data_reg == 0u);
    assert(g_chip.dac[AO0_INDEX].control_reg == (CTRL_OUTEN | CTRL_REXT | CTRL_DCEN | 0x1u));
    assert(g_chip.dac[AO0_INDEX].data_reg == 0u);
    assert(ao_enable_requested());
}

static void test_chain_startup_runs_once_and_again_after_rebind(void)
{
    native_channel_source_t source_ao0 = {.source_index = AO0_INDEX, .source_flags = 0x1u};
    native_channel_source_t source_ao1 = {.source_index = AO1_INDEX, .source_flags = 0x1u};
    telemetry_channel_t channel_ao0 = make_channel(&source_ao0, TELEMETRY_CHANNEL_VALUE_FLOAT);
    telemetry_channel_t channel_ao1 = make_channel(&source_ao1, TELEMETRY_CHANNEL_VALUE_FLOAT);
    unsigned before;

    chip_reset();
    native_ad5422_bind_bus(&g_bus);
    assert(native_ad5422_start(&channel_ao0) == TELEMETRY_DRIVER_RESULT_OK);
    assert(g_chip.transfers == CHAIN_STARTUP_TRANSFERS + 3u);
    before = g_chip.transfers;
    assert(native_ad5422_start(&channel_ao1) == TELEMETRY_DRIVER_RESULT_OK);
    assert(g_chip.transfers - before == 3u);
    assert(g_chip.dac[AO0_INDEX].resets == 1u && g_chip.dac[AO1_INDEX].resets == 1u);
    /* Stopping one channel leaves the shared enable and the other output alone. */
    assert(native_ad5422_stop(&channel_ao0) == TELEMETRY_DRIVER_RESULT_OK);
    assert(ao_enable_requested());
    assert((g_chip.dac[AO1_INDEX].control_reg & CTRL_OUTEN) != 0u);

    native_ad5422_bind_bus(&g_bus);
    before = g_chip.transfers;
    assert(native_ad5422_start(&channel_ao1) == TELEMETRY_DRIVER_RESULT_OK);
    assert(g_chip.transfers - before == CHAIN_STARTUP_TRANSFERS + 3u);
    assert(g_chip.dac[AO0_INDEX].resets == 2u && g_chip.dac[AO1_INDEX].resets == 2u);
}

static void test_chain_startup_failure_keeps_clear_asserted(void)
{
    native_channel_source_t source = {.source_index = AO0_INDEX, .source_flags = 0x1u};
    telemetry_channel_t channel = make_channel(&source, TELEMETRY_CHANNEL_VALUE_FLOAT);
    telemetry_channel_value_t value = {.f32 = 1.0f};
    unsigned step;

    for (step = 1u; step <= CHAIN_STARTUP_TRANSFERS; ++step)
    {
        chip_reset();
        native_ad5422_bind_bus(&g_bus);
        g_chip.fail_transfer = step;
        assert(native_ad5422_start(&channel) == TELEMETRY_DRIVER_RESULT_TIMEOUT);
        assert(g_chip.transfers == step);
        assert(g_chip.enable_is_output && !g_chip.enable_level);
        /* Nothing was ever enabled: stop has nothing to do, write refuses. */
        assert(native_ad5422_stop(&channel) == TELEMETRY_DRIVER_RESULT_OK);
        assert(native_ad5422_write(&channel, &value) == TELEMETRY_DRIVER_RESULT_INVALID_STATE);
        assert(g_chip.transfers == step);
        /* Not latched: a later start runs the whole sequence. */
        assert(native_ad5422_start(&channel) == TELEMETRY_DRIVER_RESULT_OK);
        assert(g_chip.transfers == step + CHAIN_STARTUP_TRANSFERS + 3u);
        assert(ao_enable_requested());
    }
}

static void test_start_enables_output_and_writing_one_channel_does_not_touch_the_other(void)
{
    native_channel_source_t source_ao0 = {.source_index = AO0_INDEX, .source_flags = 0x1u}; /* 0-10V */
    native_channel_source_t source_ao1 = {.source_index = AO1_INDEX, .source_flags = 0x5u}; /* 4-20mA */
    telemetry_channel_t channel_ao0 = make_channel(&source_ao0, TELEMETRY_CHANNEL_VALUE_FLOAT);
    telemetry_channel_t channel_ao1 = make_channel(&source_ao1, TELEMETRY_CHANNEL_VALUE_DOUBLE);
    telemetry_channel_value_t value_ao0 = {.f32 = 5.0f}; /* half of 0-10V */

    chip_reset();
    native_ad5422_bind_bus(&g_bus);
    assert(native_ad5422_start(&channel_ao0) == TELEMETRY_DRIVER_RESULT_OK);
    assert(native_ad5422_start(&channel_ao1) == TELEMETRY_DRIVER_RESULT_OK);
    /* CONTROL = OUTEN | REXT | DCEN | RANGE, nothing else (CLRSEL, OVRRNG,
     * slew all 0), on both devices. */
    assert(g_chip.dac[AO0_INDEX].control_reg == (CTRL_OUTEN | CTRL_REXT | CTRL_DCEN | 0x1u));
    assert(g_chip.dac[AO1_INDEX].control_reg == (CTRL_OUTEN | CTRL_REXT | CTRL_DCEN | 0x5u));
    assert(g_chip.dac[AO0_INDEX].data_reg == 0u);
    assert(g_chip.dac[AO1_INDEX].data_reg == 0u);

    /* Writing AO0 must not disturb AO1's already-latched data register. */
    g_chip.dac[AO1_INDEX].data_reg = 0xBEEFu;
    assert(native_ad5422_write(&channel_ao0, &value_ao0) == TELEMETRY_DRIVER_RESULT_OK);
    assert(g_chip.dac[AO1_INDEX].data_reg == 0xBEEFu);
    assert_close((double)g_chip.dac[AO0_INDEX].data_reg / 65535.0 * 10.0, 5.0);

    /* Write-only: the board has no SDO path back, so nothing ever asks for rx. */
    assert(!g_chip.rx_requested);
    assert(g_chip.ao0_rejected_frames == 0u);
}

static void test_start_sequence_sets_rext_only_with_outen(void)
{
    native_channel_source_t source = {.source_index = AO1_INDEX, .source_flags = 0x5u};
    telemetry_channel_t channel = make_channel(&source, TELEMETRY_CHANNEL_VALUE_DOUBLE);

    chip_reset();
    native_ad5422_bind_bus(&g_bus);
    assert(native_ad5422_start(&channel) == TELEMETRY_DRIVER_RESULT_OK);
    /* After the chain enable: range selected with the output off,
     * zero-scale loaded, then one enabling write carrying OUTEN and REXT. */
    assert(g_chip.dac[AO1_INDEX].control_writes == 3u);
    assert(g_chip.dac[AO1_INDEX].control_history[1] == (CTRL_DCEN | 0x5u));
    assert(g_chip.dac[AO1_INDEX].control_history[2] == (CTRL_OUTEN | CTRL_REXT | CTRL_DCEN | 0x5u));
    assert(g_chip.dac[AO1_INDEX].data_writes_before_enable == 1u);
    /* The untouched device got only NOPs after its chain enable. */
    assert(g_chip.dac[AO0_INDEX].control_writes == 1u && g_chip.dac[AO0_INDEX].data_writes_before_enable == 0u);
    assert_rext_only_with_outen();
}

static void test_write_scales_voltage_and_current(void)
{
    native_channel_source_t source_voltage = {.source_index = AO0_INDEX, .source_flags = 0x1u}; /* 0-10V */
    native_channel_source_t source_current = {.source_index = AO1_INDEX, .source_flags = 0x5u}; /* 4-20mA */
    telemetry_channel_t channel_voltage = make_channel(&source_voltage, TELEMETRY_CHANNEL_VALUE_FLOAT);
    telemetry_channel_t channel_current = make_channel(&source_current, TELEMETRY_CHANNEL_VALUE_DOUBLE);
    telemetry_channel_value_t ten_volts = {.f32 = 10.0f};
    telemetry_channel_value_t twelve_ma = {.f64 = 12.0};
    telemetry_channel_value_t four_ma = {.f64 = 4.0};

    chip_reset();
    native_ad5422_bind_bus(&g_bus);
    /* Before the chain is up a 48-bit frame is not a valid write. */
    assert(native_ad5422_write(&channel_voltage, &ten_volts) == TELEMETRY_DRIVER_RESULT_INVALID_STATE);
    assert(g_chip.transfers == 0u);
    assert(native_ad5422_start(&channel_voltage) == TELEMETRY_DRIVER_RESULT_OK);
    assert(native_ad5422_start(&channel_current) == TELEMETRY_DRIVER_RESULT_OK);

    assert(native_ad5422_write(&channel_voltage, &ten_volts) == TELEMETRY_DRIVER_RESULT_OK);
    /* Full-scale code (65535) for 10 V on the 0-10V range. */
    assert(g_chip.dac[AO0_INDEX].data_reg == 65535u);
    assert_close((double)channel_voltage.runtime.value.f32, 10.0);

    /* 12 mA on 4-20mA: normalized = (12-4)/16 = 0.5 -> code ~32768. */
    assert(native_ad5422_write(&channel_current, &twelve_ma) == TELEMETRY_DRIVER_RESULT_OK);
    assert(g_chip.dac[AO1_INDEX].data_reg >= 32760u && g_chip.dac[AO1_INDEX].data_reg <= 32770u);
    assert_close(channel_current.runtime.value.f64, 12.0);

    /* 4 mA is the bottom of the range, code 0 -- not 0 mA. */
    assert(native_ad5422_write(&channel_current, &four_ma) == TELEMETRY_DRIVER_RESULT_OK);
    assert(g_chip.dac[AO1_INDEX].data_reg == 0u);

    /* Range edges are producible and map to the end codes. */
    telemetry_channel_value_t zero_volts = {.f32 = 0.0f};
    telemetry_channel_value_t twenty_ma = {.f64 = 20.0};
    assert(native_ad5422_write(&channel_voltage, &zero_volts) == TELEMETRY_DRIVER_RESULT_OK);
    assert(g_chip.dac[AO0_INDEX].data_reg == 0u);
    assert(native_ad5422_write(&channel_current, &twenty_ma) == TELEMETRY_DRIVER_RESULT_OK);
    assert(g_chip.dac[AO1_INDEX].data_reg == 65535u);
    assert(native_ad5422_write(&channel_voltage, &ten_volts) == TELEMETRY_DRIVER_RESULT_OK);
}

/** @brief An unproducible command is refused: no frame, no DAC change, no runtime change. */
static void test_out_of_range_write_is_refused_not_clamped(void)
{
    native_channel_source_t source_voltage = {.source_index = AO0_INDEX, .source_flags = 0x1u}; /* 0-10V */
    native_channel_source_t source_current = {.source_index = AO1_INDEX, .source_flags = 0x5u}; /* 4-20mA */
    telemetry_channel_t channel_voltage = make_channel(&source_voltage, TELEMETRY_CHANNEL_VALUE_FLOAT);
    telemetry_channel_t channel_current = make_channel(&source_current, TELEMETRY_CHANNEL_VALUE_DOUBLE);
    static const float bad_volts[] = {-5.0f, -0.001f, 10.001f, 24.0f, NAN, INFINITY};
    static const double bad_ma[] = {0.0, 3.999, 20.001, 24.0, -INFINITY};
    telemetry_channel_value_t five_volts = {.f32 = 5.0f};
    telemetry_channel_value_t twelve_ma = {.f64 = 12.0};
    size_t index;

    chip_reset();
    native_ad5422_bind_bus(&g_bus);
    assert(native_ad5422_start(&channel_voltage) == TELEMETRY_DRIVER_RESULT_OK);
    assert(native_ad5422_start(&channel_current) == TELEMETRY_DRIVER_RESULT_OK);
    assert(native_ad5422_write(&channel_voltage, &five_volts) == TELEMETRY_DRIVER_RESULT_OK);
    assert(native_ad5422_write(&channel_current, &twelve_ma) == TELEMETRY_DRIVER_RESULT_OK);
    const uint16_t voltage_code = g_chip.dac[AO0_INDEX].data_reg;
    const uint16_t current_code = g_chip.dac[AO1_INDEX].data_reg;
    const unsigned transfers = g_chip.transfers;

    for (index = 0u; index < sizeof(bad_volts) / sizeof(bad_volts[0]); ++index)
    {
        telemetry_channel_value_t value = {.f32 = bad_volts[index]};

        assert(native_ad5422_write(&channel_voltage, &value) == TELEMETRY_DRIVER_RESULT_OUT_OF_RANGE);
        assert_close((double)channel_voltage.runtime.value.f32, 5.0);
    }
    for (index = 0u; index < sizeof(bad_ma) / sizeof(bad_ma[0]); ++index)
    {
        telemetry_channel_value_t value = {.f64 = bad_ma[index]};

        assert(native_ad5422_write(&channel_current, &value) == TELEMETRY_DRIVER_RESULT_OUT_OF_RANGE);
        assert_close(channel_current.runtime.value.f64, 12.0);
    }
    assert(g_chip.transfers == transfers);
    assert(g_chip.dac[AO0_INDEX].data_reg == voltage_code && g_chip.dac[AO1_INDEX].data_reg == current_code);
}

static void test_stop_disables_output(void)
{
    native_channel_source_t source = {.source_index = AO0_INDEX, .source_flags = 0x1u};
    telemetry_channel_t channel = make_channel(&source, TELEMETRY_CHANNEL_VALUE_FLOAT);

    chip_reset();
    native_ad5422_bind_bus(&g_bus);
    assert(native_ad5422_start(&channel) == TELEMETRY_DRIVER_RESULT_OK);
    assert((g_chip.dac[AO0_INDEX].control_reg & CTRL_OUTEN) != 0u);
    assert(native_ad5422_stop(&channel) == TELEMETRY_DRIVER_RESULT_OK);
    /* OUTEN and REXT cleared together; DCEN and range preserved so AO1
     * stays reachable through AO0. */
    assert(g_chip.dac[AO0_INDEX].control_reg == (CTRL_DCEN | 0x1u));

    /* A restart re-enables with REXT again, without re-running chain startup. */
    assert(native_ad5422_start(&channel) == TELEMETRY_DRIVER_RESULT_OK);
    assert(g_chip.dac[AO0_INDEX].control_reg == (CTRL_OUTEN | CTRL_REXT | CTRL_DCEN | 0x1u));
    assert(g_chip.dac[AO0_INDEX].resets == 1u);
    assert_rext_only_with_outen();
}

static void test_failed_enable_leaves_output_disabled(void)
{
    native_channel_source_t source = {.source_index = AO0_INDEX, .source_flags = 0x5u};
    telemetry_channel_t channel = make_channel(&source, TELEMETRY_CHANNEL_VALUE_DOUBLE);

    chip_reset();
    native_ad5422_bind_bus(&g_bus);
    g_chip.fail_transfer = CHAIN_STARTUP_TRANSFERS + 3u; /* the enabling CONTROL write */
    assert(native_ad5422_start(&channel) == TELEMETRY_DRIVER_RESULT_TIMEOUT);
    assert(g_chip.transfers == CHAIN_STARTUP_TRANSFERS + 4u); /* ... followed by the disabling retry */
    assert(g_chip.dac[AO0_INDEX].control_reg == (CTRL_DCEN | 0x5u));
}

static void test_transfer_failure_maps_to_timeout(void)
{
    native_channel_source_t source = {.source_index = AO0_INDEX, .source_flags = 0x1u};
    telemetry_channel_t channel = make_channel(&source, TELEMETRY_CHANNEL_VALUE_FLOAT);
    telemetry_channel_value_t value = {.f32 = 1.0f};

    chip_reset();
    native_ad5422_bind_bus(&g_bus);
    assert(native_ad5422_start(&channel) == TELEMETRY_DRIVER_RESULT_OK);
    g_chip.force_transfer_failure = true;
    assert(native_ad5422_start(&channel) == TELEMETRY_DRIVER_RESULT_TIMEOUT);
    assert(native_ad5422_stop(&channel) == TELEMETRY_DRIVER_RESULT_TIMEOUT);
    assert(native_ad5422_write(&channel, &value) == TELEMETRY_DRIVER_RESULT_TIMEOUT);
}

int main(void)
{
    test_source_validation_is_the_board_range_contract();
    test_unsupported_ranges_are_never_sent();
    test_no_bus_bound();
    test_chain_startup_from_power_up();
    test_chain_startup_after_warm_restart();
    test_chain_startup_runs_once_and_again_after_rebind();
    test_chain_startup_failure_keeps_clear_asserted();
    test_start_enables_output_and_writing_one_channel_does_not_touch_the_other();
    test_start_sequence_sets_rext_only_with_outen();
    test_write_scales_voltage_and_current();
    test_out_of_range_write_is_refused_not_clamped();
    test_stop_disables_output();
    test_failed_enable_leaves_output_disabled();
    test_transfer_failure_maps_to_timeout();
    return 0;
}
