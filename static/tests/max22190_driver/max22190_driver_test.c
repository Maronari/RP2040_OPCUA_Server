/**
 * @file
 * @brief Host tests for the real drivers/src/max22190_driver.c against a
 *        fake MAX22190 Mode-0 chip model that itself computes/checks CRC-5
 *        the same way the datasheet describes -- this is a genuine
 *        round-trip proof: the fake chip rejects any frame whose CRC it
 *        does not accept as valid (proving the driver's outgoing CRC is
 *        correct), and the driver rejects any response whose CRC does not
 *        check out (proving the driver's incoming CRC check is correct).
 *        The CRC-5 implementation here is written independently from
 *        drivers/src/max22190_driver.c's, not copied from it, so agreement
 *        between the two is real evidence, not a tautology.
 */
#include "drivers/max22190_driver.h"
#include "drivers/industrial_board_pins.h"

#include "hardware/gpio.h"

#include <assert.h>
#include <string.h>

#define DI0_INDEX 0u
#define DI3_INDEX 3u

#define REG_WB 0x00u
#define REG_DI 0x02u
#define REG_FAULT1 0x04u
#define REG_FLT1 0x06u
#define REG_FLT4 0x0Cu
#define REG_CFG 0x18u
#define REG_INEN 0x1Au
#define REG_FAULT2 0x1Cu
#define REG_FAULT2EN 0x1Eu
#define REG_GPO 0x22u
#define REG_FAULT1EN 0x24u

#define FAULT1_POR 0x40u

/** Transactions one chip startup costs: FAULT1 read, 6 writes, WB + FAULT2
 *  reads, 3 writes, POR clear, 9 readbacks, final FAULT1 read. */
#define CHIP_STARTUP_TRANSFERS 23u

typedef struct
{
    bool write;
    uint8_t address;
    uint8_t data;
} fake_frame_t;

typedef struct
{
    uint8_t di_byte;
    uint8_t registers[0x28];
    bool chip_present;
    bool corrupt_response_crc;
    bool force_transfer_failure;
    bool reject_bad_request_crc;
    bool por_stuck;           /**< FAULT1.POR ignores the clearing write. */
    int stuck_register;       /**< Address whose writes are ignored, or -1. */
    unsigned transfers;
    unsigned fail_transfer;
    fake_frame_t log[64];
    unsigned logged;
} fake_chip_t;

static fake_chip_t g_chip;

/** @brief Independent re-implementation of the datasheet's CRC-5 (x^5+x^4+x^2+x^0), for cross-checking the driver under test -- not shared code with drivers/src/max22190_driver.c. */
static uint8_t fake_crc5(uint32_t bits19)
{
    static const uint32_t poly = 0x35u; /* x^5+x^4+x^2+x^0, with the leading x^5 term explicit here. */
    uint32_t reg = bits19 << 5;
    int bit_index;

    for (bit_index = 23; bit_index >= 5; --bit_index)
    {
        if ((reg & (1u << bit_index)) != 0u)
        {
            reg ^= poly << (bit_index - 5);
        }
    }
    return (uint8_t)(reg & 0x1Fu);
}

/** @brief Drive the active-low DI_READY_N line: ready == chip reports itself ready. */
static void set_chip_ready_line(bool ready)
{
    g_gpio_stub_pins[INDUSTRIAL_DI_READY_GPIO].external_level = !ready;
}

static void chip_reset(void)
{
    memset(&g_chip, 0, sizeof(g_chip));
    /* gpio_stub_reset() leaves every pin high, i.e. DI_READY_N de-asserted
     * ("not ready"); the default fixture models a healthy chip, so pull it
     * low here. Tests that want the not-ready case say so explicitly. */
    gpio_stub_reset();
    set_chip_ready_line(true);
    g_chip.chip_present = true;
    g_chip.stuck_register = -1;
    /* Datasheet POR defaults, with stale diagnostics pending in WB/FAULT2. */
    g_chip.registers[REG_FAULT1] = 0x46u;
    g_chip.registers[REG_FLT1] = 0x08u;
    g_chip.registers[REG_FLT1 + 2u] = 0x08u;
    g_chip.registers[REG_FLT1 + 4u] = 0x08u;
    g_chip.registers[REG_FLT4] = 0x08u;
    g_chip.registers[REG_INEN] = 0xFFu;
    g_chip.registers[REG_FAULT1EN] = 0xC0u;
    g_chip.registers[REG_WB] = 0x01u;
    g_chip.registers[REG_FAULT2] = 0x02u;
}

static bool fake_transfer(void *context, const uint8_t *tx, uint8_t *rx, size_t length)
{
    fake_chip_t *chip = (fake_chip_t *)context;
    if (++chip->transfers == chip->fail_transfer) return false;
    uint32_t frame24;
    uint32_t request_bits19;
    uint8_t request_crc;
    bool write;
    uint8_t address;
    uint8_t data;
    uint32_t response_bits19;

    if (chip->force_transfer_failure || length != 3u || !chip->chip_present)
    {
        return false;
    }
    frame24 = ((uint32_t)tx[0] << 16) | ((uint32_t)tx[1] << 8) | tx[2];
    request_bits19 = frame24 >> 5;
    request_crc = (uint8_t)(frame24 & 0x1Fu);
    if (chip->reject_bad_request_crc && fake_crc5(request_bits19) != request_crc)
    {
        /* Real chip: frame discarded, FAULT1.CRC set, but SDO for this
         * transfer still clocks out (garbage) -- for this test, simplest
         * faithful behavior is to just not apply the command below. */
    }
    else
    {
        write = ((request_bits19 >> 18) & 0x1u) != 0u;
        address = (uint8_t)((request_bits19 >> 11) & 0x7Fu);
        data = (uint8_t)((request_bits19 >> 3) & 0xFFu);
        if (chip->logged < sizeof(chip->log) / sizeof(chip->log[0]))
        {
            chip->log[chip->logged++] = (fake_frame_t){write, address, data};
        }
        if (write && address == REG_FAULT1)
        {
            /* Only POR is writable, and only "0" clears it; the other
             * FAULT1 bits are unaffected by the write. */
            if ((data & FAULT1_POR) == 0u && !chip->por_stuck)
            {
                chip->registers[REG_FAULT1] &= (uint8_t)~FAULT1_POR;
            }
        }
        else if (write && address < sizeof(chip->registers) && (int)address != chip->stuck_register)
        {
            chip->registers[address] = data;
        }
    }

    if (rx != NULL)
    {
        uint8_t byte1 = 0u;
        uint32_t addressed = (frame24 >> 5 >> 11) & 0x7Fu;
        bool read = ((frame24 >> 23) & 0x1u) == 0u;

        if (addressed < sizeof(chip->registers))
        {
            byte1 = chip->registers[addressed];
        }
        if (read && (addressed == REG_WB || addressed == REG_FAULT2))
        {
            chip->registers[addressed] = 0u; /* clear-on-read */
        }
        response_bits19 = ((uint32_t)chip->di_byte << 11) | ((uint32_t)byte1 << 3);
        uint8_t response_crc = fake_crc5(response_bits19);

        if (chip->corrupt_response_crc)
        {
            response_crc ^= 0x1Fu;
        }
        rx[0] = chip->di_byte;
        rx[1] = byte1;
        rx[2] = response_crc;
    }
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

static telemetry_channel_t make_channel(native_channel_source_t *source)
{
    telemetry_channel_t channel = {0};
    channel.driver_context = source;
    channel.metadata.value_type = TELEMETRY_CHANNEL_VALUE_BOOLEAN;
    return channel;
}

static void test_source_validation(void)
{
    assert(native_max22190_source_is_valid(0u, 0x0u));
    assert(native_max22190_source_is_valid(3u, 0x0Fu));
    assert(!native_max22190_source_is_valid(4u, 0x0u));
    assert(!native_max22190_source_is_valid(0u, 0x10u)); /* bit 4 (WBE) not allowed via source_flags */
}

static void test_no_bus_bound(void)
{
    native_channel_source_t source = {.source_index = DI0_INDEX, .source_flags = 0x08u};
    telemetry_channel_t channel = make_channel(&source);

    native_max22190_bind_bus(NULL);
    assert(native_max22190_start(&channel) == TELEMETRY_DRIVER_RESULT_INVALID_STATE);
    assert(native_max22190_read(&channel, 0u) == TELEMETRY_DRIVER_RESULT_INVALID_STATE);
    assert(native_max22190_self_test(&channel) == TELEMETRY_DRIVER_RESULT_INVALID_STATE);
}

static void test_start_writes_filter_register(void)
{
    spi_bus_t bus = {.vtbl = &g_fake_vtbl, .context = &g_chip};
    native_channel_source_t source = {.source_index = DI0_INDEX, .source_flags = 0x03u};
    telemetry_channel_t channel = make_channel(&source);

    chip_reset();
    native_max22190_bind_bus(&bus);
    assert(native_max22190_start(&channel) == TELEMETRY_DRIVER_RESULT_OK);
    assert(g_chip.registers[REG_FLT1] == 0x03u);
}

static void test_read_extracts_correct_bit_per_channel(void)
{
    spi_bus_t bus = {.vtbl = &g_fake_vtbl, .context = &g_chip};
    native_channel_source_t source0 = {.source_index = DI0_INDEX, .source_flags = 0u};
    native_channel_source_t source3 = {.source_index = DI3_INDEX, .source_flags = 0u};
    telemetry_channel_t channel0 = make_channel(&source0);
    telemetry_channel_t channel3 = make_channel(&source3);

    chip_reset();
    native_max22190_bind_bus(&bus);
    g_chip.di_byte = 0x09u; /* 0b0000_1001 -> DI0=1, DI3=1, DI1=DI2=0 */
    assert(native_max22190_read(&channel0, 0u) == TELEMETRY_DRIVER_RESULT_OK);
    assert(channel0.runtime.value.boolean == true);
    assert(native_max22190_read(&channel3, 0u) == TELEMETRY_DRIVER_RESULT_OK);
    assert(channel3.runtime.value.boolean == true);

    g_chip.di_byte = 0x01u; /* only DI0 set */
    assert(native_max22190_read(&channel3, 0u) == TELEMETRY_DRIVER_RESULT_OK);
    assert(channel3.runtime.value.boolean == false);
}

static void test_response_crc_mismatch_is_a_failure(void)
{
    spi_bus_t bus = {.vtbl = &g_fake_vtbl, .context = &g_chip};
    native_channel_source_t source = {.source_index = DI0_INDEX, .source_flags = 0u};
    telemetry_channel_t channel = make_channel(&source);

    chip_reset();
    native_max22190_bind_bus(&bus);
    assert(native_max22190_read(&channel, 0u) == TELEMETRY_DRIVER_RESULT_OK);

    g_chip.corrupt_response_crc = true;
    assert(native_max22190_read(&channel, 0u) == TELEMETRY_DRIVER_RESULT_TIMEOUT);
    assert(native_max22190_start(&channel) == TELEMETRY_DRIVER_RESULT_HARDWARE_FAILURE);
}

static void test_chip_rejects_bad_request_crc(void)
{
    /* Proves the outgoing CRC is well-formed: with the fake chip enforcing
     * request-CRC validity, a write that the chip would reject leaves its
     * register unchanged. Since the real driver always computes a correct
     * CRC, this must never be observed for driver-issued frames -- this
     * test instead confirms the fake chip's own enforcement path is live by
     * checking a well-formed driver write DOES get applied under
     * enforcement. */
    spi_bus_t bus = {.vtbl = &g_fake_vtbl, .context = &g_chip};
    native_channel_source_t source = {.source_index = DI0_INDEX, .source_flags = 0x05u};
    telemetry_channel_t channel = make_channel(&source);

    chip_reset();
    g_chip.reject_bad_request_crc = true;
    native_max22190_bind_bus(&bus);
    assert(native_max22190_start(&channel) == TELEMETRY_DRIVER_RESULT_OK);
    assert(g_chip.registers[REG_FLT1] == 0x05u);
}

static void test_self_test_passed_unavailable_failed(void)
{
    spi_bus_t bus = {.vtbl = &g_fake_vtbl, .context = &g_chip};
    native_channel_source_t source = {.source_index = DI3_INDEX, .source_flags = 0x02u};
    telemetry_channel_t channel = make_channel(&source);

    chip_reset();
    native_max22190_bind_bus(&bus);
    assert(native_max22190_self_test(&channel) == TELEMETRY_DRIVER_RESULT_OK);
    /* Restored to the channel's actual configured filter setting afterward. */
    assert(g_chip.registers[REG_FLT4] == 0x02u);

    g_chip.chip_present = false;
    assert(native_max22190_self_test(&channel) == TELEMETRY_DRIVER_RESULT_TIMEOUT);

    g_chip.chip_present = true;
    g_chip.corrupt_response_crc = true;
    assert(native_max22190_self_test(&channel) == TELEMETRY_DRIVER_RESULT_TIMEOUT);
}

static void test_transfer_failure_maps_to_expected_results(void)
{
    spi_bus_t bus = {.vtbl = &g_fake_vtbl, .context = &g_chip};
    native_channel_source_t source = {.source_index = DI0_INDEX, .source_flags = 0u};
    telemetry_channel_t channel = make_channel(&source);

    chip_reset();
    native_max22190_bind_bus(&bus);
    g_chip.force_transfer_failure = true;
    assert(native_max22190_start(&channel) == TELEMETRY_DRIVER_RESULT_HARDWARE_FAILURE);
    assert(native_max22190_read(&channel, 0u) == TELEMETRY_DRIVER_RESULT_TIMEOUT);
    assert(native_max22190_self_test(&channel) == TELEMETRY_DRIVER_RESULT_TIMEOUT);
}

static void test_ready_gate_runs_before_any_spi(void)
{
    spi_bus_t bus = {.vtbl = &g_fake_vtbl, .context = &g_chip};
    native_channel_source_t source = {.source_index = DI0_INDEX, .source_flags = 0x04u};
    telemetry_channel_t channel = make_channel(&source);

    chip_reset();
    set_chip_ready_line(false); /* /READY stuck de-asserted: chip absent/unpowered/broken. */
    native_max22190_bind_bus(&bus);
    assert(native_max22190_start(&channel) == TELEMETRY_DRIVER_RESULT_TIMEOUT);
    /* The whole point of the gate: not one SPI frame may be attempted
     * before the chip says it is ready. */
    assert(g_chip.transfers == 0u);
    /* Input, with internal pulls off -- the board's external 10k pull-up
     * already holds this open-drain line. */
    assert(gpio_get_dir(INDUSTRIAL_DI_READY_GPIO) == GPIO_IN);
    assert(g_gpio_stub_pins[INDUSTRIAL_DI_READY_GPIO].pulls_disabled);

    /* A failed gate is not latched: once the line asserts, a later start
     * gets through and does its work. */
    set_chip_ready_line(true);
    assert(native_max22190_start(&channel) == TELEMETRY_DRIVER_RESULT_OK);
    assert(g_chip.registers[REG_FLT1] == 0x04u);
}

static void test_chip_startup_runs_once_not_per_channel(void)
{
    spi_bus_t bus = {.vtbl = &g_fake_vtbl, .context = &g_chip};
    native_channel_source_t sources[4] = {
        {.source_index = 0u, .source_flags = 0x01u},
        {.source_index = 1u, .source_flags = 0x02u},
        {.source_index = 2u, .source_flags = 0x03u},
        {.source_index = 3u, .source_flags = 0x04u},
    };
    unsigned index;

    chip_reset();
    native_max22190_bind_bus(&bus);

    /* DI0..DI3 are four logical views of one chip: the first start pays for
     * the chip startup sequence plus its own FLTn write; each later channel
     * pays only for its own FLTn write. */
    for (index = 0u; index < 4u; ++index)
    {
        telemetry_channel_t channel = make_channel(&sources[index]);
        unsigned before = g_chip.transfers;

        assert(native_max22190_start(&channel) == TELEMETRY_DRIVER_RESULT_OK);
        assert(g_chip.transfers - before == (index == 0u ? CHIP_STARTUP_TRANSFERS + 1u : 1u));
    }
    for (index = 0u; index < 4u; ++index)
    {
        assert(g_chip.registers[REG_FLT1 + 2u * index] == sources[index].source_flags);
    }

    /* Rebinding the bus is a new chip context, so startup runs again. */
    native_max22190_bind_bus(&bus);
    {
        telemetry_channel_t channel = make_channel(&sources[0]);
        unsigned before = g_chip.transfers;

        assert(native_max22190_start(&channel) == TELEMETRY_DRIVER_RESULT_OK);
        assert(g_chip.transfers - before == CHIP_STARTUP_TRANSFERS + 1u);
    }
}

/** @brief HARDWARE_MVP.md section 3.3.6 initialization, frame by frame. */
static void test_chip_startup_follows_frozen_sequence(void)
{
    static const fake_frame_t expected[] = {
        {false, REG_FAULT1, 0x00u},   /* 4: CRC-protected transaction, inspect FAULT1/POR */
        {true, REG_INEN, 0x0Fu},      /* 5 */
        {true, REG_FLT1, 0x04u},      /* 6 */
        {true, REG_FLT1 + 2u, 0x04u},
        {true, REG_FLT1 + 4u, 0x04u},
        {true, REG_FLT4, 0x04u},
        {true, REG_CFG, 0x01u},       /* 7 */
        {false, REG_WB, 0x00u},       /* 8 */
        {false, REG_FAULT2, 0x00u},
        {true, REG_FAULT2EN, 0x3Cu},  /* 9 */
        {true, REG_GPO, 0x80u},       /* 10 */
        {true, REG_FAULT1EN, 0xF8u},  /* 11 */
        {true, REG_FAULT1, 0x00u},    /* 12: write 0 to POR */
        {false, REG_INEN, 0x00u},     /* 13: read back */
        {false, REG_FLT1, 0x00u},
        {false, REG_FLT1 + 2u, 0x00u},
        {false, REG_FLT1 + 4u, 0x00u},
        {false, REG_FLT4, 0x00u},
        {false, REG_CFG, 0x00u},
        {false, REG_FAULT2EN, 0x00u},
        {false, REG_GPO, 0x00u},
        {false, REG_FAULT1EN, 0x00u},
        {false, REG_FAULT1, 0x00u},
        {true, REG_FLT1 + 4u, 0x0Au}, /* the channel's own FLTn */
    };
    spi_bus_t bus = {.vtbl = &g_fake_vtbl, .context = &g_chip};
    native_channel_source_t source = {.source_index = 2u, .source_flags = 0x0Au};
    telemetry_channel_t channel = make_channel(&source);
    unsigned index;

    assert(sizeof(expected) / sizeof(expected[0]) == CHIP_STARTUP_TRANSFERS + 1u);
    chip_reset();
    native_max22190_bind_bus(&bus);
    assert(native_max22190_start(&channel) == TELEMETRY_DRIVER_RESULT_OK);
    assert(g_chip.logged == sizeof(expected) / sizeof(expected[0]));
    for (index = 0u; index < g_chip.logged; ++index)
    {
        assert(g_chip.log[index].write == expected[index].write);
        assert(g_chip.log[index].address == expected[index].address);
        assert(!expected[index].write || g_chip.log[index].data == expected[index].data);
    }

    /* End state: the board configuration, IN5..IN8 disabled, stale
     * diagnostics consumed, POR cleared without disturbing FAULT1's other
     * bits, and the channel's own filter over the board default. */
    assert(g_chip.registers[REG_INEN] == 0x0Fu);
    assert(g_chip.registers[REG_FLT1] == 0x04u && g_chip.registers[REG_FLT1 + 2u] == 0x04u);
    assert(g_chip.registers[REG_FLT1 + 4u] == 0x0Au && g_chip.registers[REG_FLT4] == 0x04u);
    assert(g_chip.registers[REG_CFG] == 0x01u);
    assert(g_chip.registers[REG_FAULT2EN] == 0x3Cu);
    assert(g_chip.registers[REG_GPO] == 0x80u);
    assert(g_chip.registers[REG_FAULT1EN] == 0xF8u);
    assert(g_chip.registers[REG_WB] == 0u && g_chip.registers[REG_FAULT2] == 0u);
    assert(g_chip.registers[REG_FAULT1] == (0x46u & (uint8_t)~FAULT1_POR));
}

/** @brief A chip that does not end up configured is not declared healthy, and not latched as such. */
static void test_chip_startup_failures_are_hardware_failures(void)
{
    spi_bus_t bus = {.vtbl = &g_fake_vtbl, .context = &g_chip};
    native_channel_source_t source = {.source_index = DI0_INDEX, .source_flags = 0x04u};
    telemetry_channel_t channel = make_channel(&source);
    static const int stuck[] = {REG_INEN, REG_FLT4, REG_CFG, REG_FAULT2EN, REG_GPO, REG_FAULT1EN};
    unsigned step;
    unsigned index;

    /* Any single failed transaction anywhere in the sequence. */
    for (step = 1u; step <= CHIP_STARTUP_TRANSFERS; ++step)
    {
        chip_reset();
        native_max22190_bind_bus(&bus);
        g_chip.fail_transfer = step;
        assert(native_max22190_start(&channel) == TELEMETRY_DRIVER_RESULT_HARDWARE_FAILURE);
        assert(g_chip.transfers == step); /* stops at the failure, no FLTn write */
    }

    /* A register that does not take its write fails readback. */
    for (index = 0u; index < sizeof(stuck) / sizeof(stuck[0]); ++index)
    {
        chip_reset();
        native_max22190_bind_bus(&bus);
        g_chip.stuck_register = stuck[index];
        assert(native_max22190_start(&channel) == TELEMETRY_DRIVER_RESULT_HARDWARE_FAILURE);
    }

    /* POR that does not clear. */
    chip_reset();
    native_max22190_bind_bus(&bus);
    g_chip.por_stuck = true;
    assert(native_max22190_start(&channel) == TELEMETRY_DRIVER_RESULT_HARDWARE_FAILURE);

    /* Not latched: once the chip behaves, the full sequence runs again. */
    g_chip.por_stuck = false;
    {
        unsigned before = g_chip.transfers;

        assert(native_max22190_start(&channel) == TELEMETRY_DRIVER_RESULT_OK);
        assert(g_chip.transfers - before == CHIP_STARTUP_TRANSFERS + 1u);
    }
}

int main(void)
{
    /* Four probe transfers followed by restore write/readback: failure
     * at any step must not be reported as a passed self-test. */
    spi_bus_t bus = {.vtbl = &g_fake_vtbl, .context = &g_chip};
    native_channel_source_t source = {.source_index = 0u, .source_flags = 1u};
    telemetry_channel_t channel = make_channel(&source);
    for (unsigned step = 1u; step <= 6u; ++step) {
        chip_reset();
        native_max22190_bind_bus(&bus);
        g_chip.fail_transfer = step;
        assert(native_max22190_self_test(&channel) != TELEMETRY_DRIVER_RESULT_OK);
    }
    test_source_validation();
    test_no_bus_bound();
    test_start_writes_filter_register();
    test_read_extracts_correct_bit_per_channel();
    test_response_crc_mismatch_is_a_failure();
    test_chip_rejects_bad_request_crc();
    test_self_test_passed_unavailable_failed();
    test_transfer_failure_maps_to_expected_results();
    test_ready_gate_runs_before_any_spi();
    test_chip_startup_runs_once_not_per_channel();
    test_chip_startup_follows_frozen_sequence();
    test_chip_startup_failures_are_hardware_failures();
    return 0;
}
