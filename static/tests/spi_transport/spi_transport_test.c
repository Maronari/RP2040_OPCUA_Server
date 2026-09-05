/**
 * @file
 * @brief Host tests for drivers/src/spi_transport.c's bus-agnostic
 *        assert/transfer/deassert framing helper.
 */
#include "drivers/spi_transport.h"

#include <assert.h>
#include <string.h>

static int g_assert_calls;
static int g_deassert_calls;
static int g_transfer_calls;
static bool g_transfer_result;
static uint8_t g_last_tx[4];
static size_t g_last_length;

static void fake_cs_assert(void *context)
{
    (void)context;
    ++g_assert_calls;
}

static void fake_cs_deassert(void *context)
{
    (void)context;
    ++g_deassert_calls;
}

static bool fake_transfer(void *context, const uint8_t *tx, uint8_t *rx, size_t length)
{
    (void)context;
    ++g_transfer_calls;
    g_last_length = length;
    if (tx != NULL)
    {
        memcpy(g_last_tx, tx, length);
    }
    if (rx != NULL && g_transfer_result)
    {
        /* Echo tx into rx so callers can observe end-to-end plumbing. */
        if (tx != NULL)
        {
            memcpy(rx, tx, length);
        }
    }
    return g_transfer_result;
}

static const spi_bus_vtbl_t g_fake_vtbl = {
    .cs_assert = fake_cs_assert,
    .cs_deassert = fake_cs_deassert,
    .transfer = fake_transfer,
};

static void reset_counters(void)
{
    g_assert_calls = 0;
    g_deassert_calls = 0;
    g_transfer_calls = 0;
}

static void test_null_and_incomplete_vtbl_rejected(void)
{
    spi_bus_t incomplete_bus;
    spi_bus_vtbl_t incomplete_vtbl = {.cs_assert = fake_cs_assert, .cs_deassert = fake_cs_deassert};
    uint8_t tx[1] = {0};
    uint8_t rx[1] = {0};

    reset_counters();
    assert(!spi_bus_transfer_framed(NULL, tx, rx, 1u));

    incomplete_bus.vtbl = NULL;
    incomplete_bus.context = NULL;
    assert(!spi_bus_transfer_framed(&incomplete_bus, tx, rx, 1u));

    incomplete_bus.vtbl = &incomplete_vtbl; /* .transfer missing */
    assert(!spi_bus_transfer_framed(&incomplete_bus, tx, rx, 1u));
    assert(g_assert_calls == 0);
    assert(g_deassert_calls == 0);
}

static void test_successful_transfer_frames_correctly(void)
{
    spi_bus_t bus = {.vtbl = &g_fake_vtbl, .context = NULL};
    uint8_t tx[3] = {0xAAu, 0xBBu, 0xCCu};
    uint8_t rx[3] = {0};

    reset_counters();
    g_transfer_result = true;
    assert(spi_bus_transfer_framed(&bus, tx, rx, sizeof(tx)));
    assert(g_assert_calls == 1);
    assert(g_deassert_calls == 1);
    assert(g_transfer_calls == 1);
    assert(g_last_length == sizeof(tx));
    assert(memcmp(g_last_tx, tx, sizeof(tx)) == 0);
    assert(memcmp(rx, tx, sizeof(tx)) == 0);
}

static void test_failed_transfer_still_deasserts(void)
{
    spi_bus_t bus = {.vtbl = &g_fake_vtbl, .context = NULL};
    uint8_t tx[2] = {0x01u, 0x02u};
    uint8_t rx[2] = {0};

    reset_counters();
    g_transfer_result = false;
    assert(!spi_bus_transfer_framed(&bus, tx, rx, sizeof(tx)));
    assert(g_assert_calls == 1);
    /* CS must never be left asserted after a failed transfer. */
    assert(g_deassert_calls == 1);
}

int main(void)
{
    test_null_and_incomplete_vtbl_rejected();
    test_successful_transfer_frames_correctly();
    test_failed_transfer_still_deasserts();
    return 0;
}
