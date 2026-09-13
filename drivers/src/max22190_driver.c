/**
 * @file
 * @brief Native channel driver for the `MAX22190` industrial digital-input
 *        frontend.
 *
 * See drivers/include/drivers/max22190_driver.h for the full contract and
 * docs/datasheets/MAX22190-register-notes.md for the register/command
 * reference this implementation follows, including the CRC-5 algorithm
 * (Analog Devices' MAX22190 CRC guideline: initial word `00111`).
 *
 * Ownership: the SPI bus binding (native_max22190_bind_bus()) and the
 * g_chip_ready latch are process-wide file-static state -- one physical
 * `MAX22190` chip is in scope for this release; every channel using this
 * driver shares it. DI0..DI3 are four logical views of that one chip, so
 * chip-level startup (the /READY gate and HARDWARE_MVP.md's register
 * initialization sequence) runs once, not once per channel.
 */
#include "drivers/max22190_driver.h"
#include "drivers/industrial_board_pins.h"

#include "hardware/gpio.h"

#include <stddef.h>

#define MAX22190_ADDR_WB 0x00u
#define MAX22190_ADDR_DI 0x02u
#define MAX22190_ADDR_FAULT1 0x04u
#define MAX22190_FLT_BASE 0x06u /**< FLTn = 0x06 + 2*n, n = 0..7 (this board uses n = 0..3). */
#define MAX22190_ADDR_CFG 0x18u
#define MAX22190_ADDR_INEN 0x1Au
#define MAX22190_ADDR_FAULT2 0x1Cu
#define MAX22190_ADDR_FAULT2EN 0x1Eu
#define MAX22190_ADDR_GPO 0x22u
#define MAX22190_ADDR_FAULT1EN 0x24u

#define MAX22190_FAULT1_POR_BIT (1u << 6)

#define MAX22190_MAX_CHANNEL_INDEX 3u
#define MAX22190_FLT_FLAGS_MASK 0x0Fu /**< FLTn bits 3-0: FBP + DELAY[2:0]. */

/* RP2350_INDUSTRIAL_MVP_V1 chip configuration: HARDWARE_MVP.md section
 * 3.3.6 ("Relevant register contract") and its DI firmware contract. */
#define MAX22190_BOARD_INEN 0x0Fu     /**< IN1..IN4 enabled; IN5..IN8 are grounded and disabled. */
#define MAX22190_BOARD_FLT 0x04u      /**< WBE=0, FBP=0, DELAY=100b (1.6 ms). */
#define MAX22190_BOARD_CFG 0x01u      /**< REFDI_SH_ENA. */
#define MAX22190_BOARD_FAULT2EN 0x3Cu /**< FAULT8CKE|OTSHDNE|RFDIOE|RFDISE; REFWB masked while wire-break is unused. */
#define MAX22190_BOARD_GPO 0x80u      /**< STK: sticky /FAULT. */
#define MAX22190_BOARD_FAULT1EN 0xF8u /**< CRCE|PORE|FAULT2E|ALRMT2E|ALRMT1E; 24VLE/24VME (VDD24 is NC) and WBGE masked. */

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

/** @brief CRC-5 polynomial x^5+x^4+x^2+x^0 with its leading term, `110101`. */
#define MAX22190_CRC_POLY 0x35u
/** @brief Initial word ADI's algorithm appends to the 19 data bits, `00111`. */
#define MAX22190_CRC_INIT_WORD 0x07u

/**
 * @brief MAX22190 CRC-5 over the 19 data bits of a frame (@p bits19, low 19
 *        bits), for both host-generated and chip-generated frames.
 *
 * Analog Devices' "Guidelines to Implement CRC Algorithm for the MAX22190
 * and MAX22192" (and its reference code, reproduced in ADI's no-OS
 * `max22190.c`): append the initial word `00111` to the 19 data bits and
 * divide the resulting 24-bit stream by `110101`; the 5-bit remainder is
 * the CRC. The initial word is not optional -- a zero-initialized CRC over
 * the same polynomial differs from the chip's by exactly `0x07` on every
 * frame, so the chip would discard every command and every response would
 * fail its check. The datasheet revision in `docs/datasheets/` (Rev 0,
 * 12/17) gives only the polynomial.
 */
static uint8_t max22190_crc5(uint32_t bits19)
{
    uint32_t remainder = ((bits19 & 0x7FFFFu) << 5) | MAX22190_CRC_INIT_WORD;
    int bit_index;

    for (bit_index = 23; bit_index >= 5; --bit_index)
    {
        if ((remainder & (1uL << bit_index)) != 0u)
        {
            remainder ^= (uint32_t)MAX22190_CRC_POLY << (bit_index - 5);
        }
    }
    return (uint8_t)(remainder & 0x1Fu);
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

/** @brief One register and the value the board configuration requires in it. */
typedef struct
{
    uint8_t address;
    uint8_t value;
} max22190_register_value_t;

/**
 * @brief Board configuration written by chip startup, in `HARDWARE_MVP.md`'s
 * order: `INEN`, `FLT1..FLT4`, `CFG` go before the stale-diagnostic clear;
 * `FAULT2EN`, `GPO`, `FAULT1EN` after it (see max22190_start_chip()).
 */
static const max22190_register_value_t g_board_config_before_clear[] = {
    {MAX22190_ADDR_INEN, MAX22190_BOARD_INEN},
    {MAX22190_FLT_BASE + 0u, MAX22190_BOARD_FLT},
    {MAX22190_FLT_BASE + 2u, MAX22190_BOARD_FLT},
    {MAX22190_FLT_BASE + 4u, MAX22190_BOARD_FLT},
    {MAX22190_FLT_BASE + 6u, MAX22190_BOARD_FLT},
    {MAX22190_ADDR_CFG, MAX22190_BOARD_CFG},
};
static const max22190_register_value_t g_board_config_after_clear[] = {
    {MAX22190_ADDR_FAULT2EN, MAX22190_BOARD_FAULT2EN},
    {MAX22190_ADDR_GPO, MAX22190_BOARD_GPO},
    {MAX22190_ADDR_FAULT1EN, MAX22190_BOARD_FAULT1EN},
};

#define MAX22190_TABLE_LENGTH(table) (sizeof(table) / sizeof((table)[0]))

/** @brief Write every entry of @p table, in order; false on the first failed transaction. */
static bool max22190_write_table(const max22190_register_value_t *table, size_t count)
{
    size_t index;

    for (index = 0u; index < count; ++index)
    {
        if (!max22190_transact(true, table[index].address, table[index].value, NULL, NULL))
        {
            return false;
        }
    }
    return true;
}

/** @brief Read every entry of @p table back; false on a failed transaction or any mismatch. */
static bool max22190_verify_table(const max22190_register_value_t *table, size_t count)
{
    size_t index;

    for (index = 0u; index < count; ++index)
    {
        uint8_t value = 0u;

        if (!max22190_transact(false, table[index].address, 0u, NULL, &value) || value != table[index].value)
        {
            return false;
        }
    }
    return true;
}

/**
 * @brief One-time startup for the single physical chip: the frozen
 *        initialization sequence of `HARDWARE_MVP.md` section 3.3.6.
 *
 * 1-2.  configure `DI_READY_N` as an input and wait (bounded) for it to go low;
 * 3.    the SPI clock mode is the board binding's (mode 0, SCLK idle low);
 * 4.    a CRC-protected `FAULT1` read proves the chip is present;
 * 5-7.  `INEN = 0x0F`, `FLT1..FLT4 = 0x04`, `CFG = 0x01`;
 * 8.    read `WB` and `FAULT2`, both clear-on-read, to drop stale/POR state;
 * 9-11. `FAULT2EN = 0x3C`, `GPO = 0x80`, `FAULT1EN = 0xF8`;
 * 12.   write `FAULT1 = 0x00` -- `POR` clears only when "0" is written to
 *       it, and the write leaves every other `FAULT1` bit unaffected
 *       (datasheet, `FAULT1` register description);
 * 13.   read every configured register back, and `FAULT1` to confirm `POR`
 *       really cleared, before the chip is declared healthy.
 *
 * `/READY` is observed strictly before the first SPI transaction. The
 * `FLTn` values written here are the board default for all four wired
 * inputs; each channel's start() then writes its own `source_flags` over
 * its own register.
 * @return TELEMETRY_DRIVER_RESULT_OK on success; TELEMETRY_DRIVER_RESULT_TIMEOUT
 *         if `/READY` never asserts within the poll bound;
 *         TELEMETRY_DRIVER_RESULT_HARDWARE_FAILURE if any transaction of the
 *         sequence fails, a register does not read back as written, or
 *         `POR` stays set.
 */
static telemetry_driver_result_t max22190_start_chip(void)
{
    uint8_t fault1 = 0u;
    uint8_t discarded = 0u;
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
    if (!max22190_transact(false, MAX22190_ADDR_FAULT1, 0u, NULL, &fault1) ||
        !max22190_write_table(g_board_config_before_clear, MAX22190_TABLE_LENGTH(g_board_config_before_clear)) ||
        !max22190_transact(false, MAX22190_ADDR_WB, 0u, NULL, &discarded) ||
        !max22190_transact(false, MAX22190_ADDR_FAULT2, 0u, NULL, &discarded) ||
        !max22190_write_table(g_board_config_after_clear, MAX22190_TABLE_LENGTH(g_board_config_after_clear)) ||
        !max22190_transact(true, MAX22190_ADDR_FAULT1, 0u, NULL, NULL) ||
        !max22190_verify_table(g_board_config_before_clear, MAX22190_TABLE_LENGTH(g_board_config_before_clear)) ||
        !max22190_verify_table(g_board_config_after_clear, MAX22190_TABLE_LENGTH(g_board_config_after_clear)) ||
        !max22190_transact(false, MAX22190_ADDR_FAULT1, 0u, NULL, &fault1) ||
        (fault1 & MAX22190_FAULT1_POR_BIT) != 0u)
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
     * (the whole HARDWARE_MVP.md sequence) runs once rather than four
     * times. Per-channel communication is still proven every time: the FLTn
     * write below is itself a CRC-checked transaction that fails loudly if
     * this channel cannot reach the chip. */
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
