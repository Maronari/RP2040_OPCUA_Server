/**
 * @file
 * @brief Native channel driver for the `MAX22190` industrial digital-input
 *        frontend.
 *
 * See drivers/include/drivers/max22190_driver.h for the full contract and
 * docs/datasheets/MAX22190-register-notes.md for the register/command
 * reference this implementation follows -- including the important CRC
 * caveat (unverified against real silicon or a datasheet worked example).
 *
 * Ownership: the SPI bus binding (native_max22190_bind_bus()) and the
 * g_chip_ready latch are process-wide file-static state -- one physical
 * `MAX22190` chip is in scope for this release; every channel using this
 * driver shares it. DI0..DI3 are four logical views of that one chip, so
 * chip-level startup (the /READY gate and the presence check) runs once,
 * not once per channel.
 */
#include "drivers/max22190_driver.h"
#include "drivers/industrial_board_pins.h"

#include "hardware/gpio.h"

#include <stddef.h>

#define MAX22190_ADDR_DI 0x02u
#define MAX22190_ADDR_FAULT1 0x04u
#define MAX22190_FLT_BASE 0x06u /**< FLTn = 0x06 + 2*n, n = 0..7 (this board uses n = 0..3). */

#define MAX22190_FAULT1_CRC_BIT (1u << 7)

#define MAX22190_MAX_CHANNEL_INDEX 3u
#define MAX22190_FLT_FLAGS_MASK 0x0Fu /**< FLTn bits 3-0: FBP + DELAY[2:0]. */

/**
 * @brief How many times the chip-startup gate samples `DI_READY_N` before
 * giving up.
 *
 * Deliberately a poll count, not a millisecond timeout: drivers in this
 * codebase have no time source (`start()` is not handed `now_ms`, and
 * nothing here may block SensorTask), and no confirmed `t_READY` figure
 * for this part exists in `docs/datasheets/MAX22190-register-notes.md` to
 * calibrate one against. The bound is small on purpose. Any channel start
 * happens long after board power-up -- FreeRTOS, the network stack and
 * configuration load all precede it -- so a healthy chip has asserted
 * `/READY` well before this runs and the very first sample succeeds. The
 * slack covers a marginal edge; a line stuck high means the chip is
 * absent, unpowered or broken, and that must fail fast rather than stall
 * the sensor task. Bring-up records the real assertion behavior
 * (`docs/releases/v0.5.1-hardware-bring-up-checklists.md` section 3).
 */
#define MAX22190_READY_POLL_ATTEMPTS 1000u

static spi_bus_t g_bus;
static bool g_bus_bound;
/** @brief Set once the one physical chip has passed its /READY gate and presence check; see native_max22190_start(). */
static bool g_chip_ready;

void native_max22190_bind_bus(const spi_bus_t *bus)
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
    /* A (re)binding means a different bus/board context, so the previous
     * chip's startup result no longer applies. */
    g_chip_ready = false;
}

bool native_max22190_source_is_valid(uint8_t source_index, uint8_t source_flags)
{
    return source_index <= MAX22190_MAX_CHANNEL_INDEX && (source_flags & (uint8_t)~MAX22190_FLT_FLAGS_MASK) == 0u;
}

/** @brief Reinterpret channel->driver_context as this driver's source selector, or NULL. */
static const native_channel_source_t *max22190_source(const telemetry_channel_t *channel)
{
    return channel != NULL ? (const native_channel_source_t *)channel->driver_context : NULL;
}

/**
 * @brief Bit-by-bit CRC-5, polynomial x^5+x^4+x^2+x^0 (XOR mask 0x15 --
 * the polynomial's low 5 bits with the implicit leading x^5 term
 * dropped), MSB-first over the low 19 bits of @p bits19.
 */
static uint8_t max22190_crc5(uint32_t bits19)
{
    uint8_t crc = 0u;
    int bit_index;

    for (bit_index = 18; bit_index >= 0; --bit_index)
    {
        uint8_t bit = (uint8_t)((bits19 >> bit_index) & 1u);
        uint8_t msb = (uint8_t)((crc >> 4) & 1u);

        crc = (uint8_t)((crc << 1) & 0x1Fu);
        if ((bit ^ msb) != 0u)
        {
            crc ^= 0x15u;
        }
    }
    return (uint8_t)(crc & 0x1Fu);
}

static void max22190_build_frame(bool write, uint8_t address, uint8_t data, uint8_t out[3])
{
    uint32_t bits19 = ((uint32_t)(write ? 1u : 0u) << 18) | ((uint32_t)(address & 0x7Fu) << 11) |
                       ((uint32_t)data << 3);
    uint8_t crc = max22190_crc5(bits19);
    uint32_t frame24 = (bits19 << 5) | crc;

    out[0] = (uint8_t)(frame24 >> 16);
    out[1] = (uint8_t)(frame24 >> 8);
    out[2] = (uint8_t)(frame24 & 0xFFu);
}

/**
 * @brief Send one 24-bit Mode 0 frame and validate the response's own CRC.
 * @param out_byte1 Optional; receives the response's first byte (always
 *        the live `DI[7:0]` status regardless of the addressed register).
 * @param out_byte2 Optional; receives the response's second byte (the
 *        addressed register's value on a read; `WB[7:0]` status on a write).
 * @return false on a transport-level failure or a response CRC mismatch
 *         (both are communication failures from the caller's perspective).
 */
static bool max22190_transact(bool write, uint8_t address, uint8_t data, uint8_t *out_byte1, uint8_t *out_byte2)
{
    uint8_t tx[3];
    uint8_t rx[3] = {0};
    uint32_t response_bits19;
    uint8_t response_crc;
    uint8_t expected_crc;

    max22190_build_frame(write, address, data, tx);
    if (!spi_bus_transfer_framed(&g_bus, tx, rx, sizeof(tx)))
    {
        return false;
    }
    response_bits19 = ((uint32_t)rx[0] << 11) | ((uint32_t)rx[1] << 3) | (((uint32_t)rx[2] >> 5) & 0x07u);
    response_crc = rx[2] & 0x1Fu;
    expected_crc = max22190_crc5(response_bits19);
    if (expected_crc != response_crc)
    {
        return false;
    }
    if (out_byte1 != NULL)
    {
        *out_byte1 = rx[0];
    }
    if (out_byte2 != NULL)
    {
        *out_byte2 = rx[1];
    }
    return true;
}

/**
 * @brief One-time startup for the single physical chip: bring up the
 *        `DI_READY_N` input, wait (bounded) for the chip to report ready,
 *        then prove it communicates.
 *
 * `HARDWARE_MVP.md`'s initialization sequence requires observing
 * `DI_READY_N == LOW` before relying on any register access, so this runs
 * strictly before the first SPI transaction rather than alongside it.
 * @return TELEMETRY_DRIVER_RESULT_OK on success; TELEMETRY_DRIVER_RESULT_TIMEOUT
 *         if `/READY` never asserts within the poll bound;
 *         TELEMETRY_DRIVER_RESULT_HARDWARE_FAILURE if the presence read fails.
 */
static telemetry_driver_result_t max22190_start_chip(void)
{
    uint8_t fault1 = 0u;
    uint32_t attempt;

    /* Input, internal pulls off: the board already provides an external
     * 10k -> +3V3_DIG pull-up on this open-drain line (HARDWARE_MVP.md),
     * and this codebase's convention is not to duplicate one. */
    gpio_init(INDUSTRIAL_DI_READY_GPIO);
    gpio_set_dir(INDUSTRIAL_DI_READY_GPIO, GPIO_IN);
    gpio_disable_pulls(INDUSTRIAL_DI_READY_GPIO);
    for (attempt = 0u; gpio_get(INDUSTRIAL_DI_READY_GPIO); ++attempt)
    {
        if (attempt >= MAX22190_READY_POLL_ATTEMPTS)
        {
            return TELEMETRY_DRIVER_RESULT_TIMEOUT;
        }
    }
    /* Real communication check: a CRC-validated read confirms the chip is
     * present and responding before any channel is trusted. FAULT1EN/INEN
     * are deliberately left at their POR defaults (0xC0/0xFF) -- 0xC0
     * already keeps the VDD24-related fault enables masked, satisfying
     * HARDWARE_MVP.md's requirement without an extra write. */
    if (!max22190_transact(false, MAX22190_ADDR_FAULT1, 0u, NULL, &fault1))
    {
        return TELEMETRY_DRIVER_RESULT_HARDWARE_FAILURE;
    }
    return TELEMETRY_DRIVER_RESULT_OK;
}

telemetry_driver_result_t native_max22190_start(telemetry_channel_t *channel)
{
    const native_channel_source_t *source = max22190_source(channel);

    if (source == NULL || !native_max22190_source_is_valid(source->source_index, source->source_flags))
    {
        return TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT;
    }
    if (!g_bus_bound)
    {
        return TELEMETRY_DRIVER_RESULT_INVALID_STATE;
    }
    /* DI0..DI3 are four logical views of one chip, so chip-level startup
     * runs once rather than four times. Per-channel communication is still
     * proven every time: the FLTn write below is itself a CRC-checked
     * transaction that fails loudly if this channel cannot reach the chip. */
    if (!g_chip_ready)
    {
        telemetry_driver_result_t chip_result = max22190_start_chip();

        if (chip_result != TELEMETRY_DRIVER_RESULT_OK)
        {
            return chip_result;
        }
        g_chip_ready = true;
    }
    if (!max22190_transact(true, (uint8_t)(MAX22190_FLT_BASE + 2u * source->source_index), source->source_flags,
                            NULL, NULL))
    {
        return TELEMETRY_DRIVER_RESULT_TIMEOUT;
    }
    return TELEMETRY_DRIVER_RESULT_OK;
}

telemetry_driver_result_t native_max22190_stop(telemetry_channel_t *channel)
{
    (void)channel;
    return TELEMETRY_DRIVER_RESULT_OK;
}

telemetry_driver_result_t native_max22190_read(telemetry_channel_t *channel, uint32_t now_ms)
{
    const native_channel_source_t *source = max22190_source(channel);
    uint8_t di_byte = 0u;

    (void)now_ms;
    if (source == NULL || !native_max22190_source_is_valid(source->source_index, source->source_flags) ||
        channel->metadata.value_type != TELEMETRY_CHANNEL_VALUE_BOOLEAN)
    {
        return TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT;
    }
    if (!g_bus_bound)
    {
        return TELEMETRY_DRIVER_RESULT_INVALID_STATE;
    }
    if (!max22190_transact(false, MAX22190_ADDR_DI, 0u, &di_byte, NULL))
    {
        return TELEMETRY_DRIVER_RESULT_TIMEOUT;
    }
    channel->runtime.value.boolean = (di_byte & (1u << source->source_index)) != 0u;
    return TELEMETRY_DRIVER_RESULT_OK;
}

telemetry_driver_result_t native_max22190_self_test(telemetry_channel_t *channel)
{
    const native_channel_source_t *source = max22190_source(channel);
    uint8_t address;
    uint8_t echo_a = 0u;
    uint8_t echo_b = 0u;
    const uint8_t probe_a = 0x05u;
    const uint8_t probe_b = 0x0Au;
    telemetry_driver_result_t result;
    bool probes_ok;

    if (source == NULL || !native_max22190_source_is_valid(source->source_index, source->source_flags))
    {
        return TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT;
    }
    if (!g_bus_bound)
    {
        return TELEMETRY_DRIVER_RESULT_INVALID_STATE;
    }
    address = (uint8_t)(MAX22190_FLT_BASE + 2u * source->source_index);
    probes_ok = max22190_transact(true, address, probe_a, NULL, NULL) &&
                max22190_transact(false, address, 0u, NULL, &echo_a) &&
                max22190_transact(true, address, probe_b, NULL, NULL) &&
                max22190_transact(false, address, 0u, NULL, &echo_b);
    if (!probes_ok)
    {
        result = TELEMETRY_DRIVER_RESULT_TIMEOUT;
    }
    else if (echo_a == echo_b)
    {
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
    /* A successful probe is not successful diagnostics if restore fails. */
    uint8_t restored = 0u;
    if (!max22190_transact(true, address, source->source_flags, NULL, NULL) ||
        !max22190_transact(false, address, 0u, NULL, &restored))
    {
        return TELEMETRY_DRIVER_RESULT_TIMEOUT;
    }
    if (restored != source->source_flags && result == TELEMETRY_DRIVER_RESULT_OK)
    {
        return TELEMETRY_DRIVER_RESULT_HARDWARE_FAILURE;
    }
    return result;
}
