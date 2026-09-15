#include "psram_allocator.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/structs/qmi.h"
#include "hardware/structs/xip_ctrl.h"
#include "hardware/sync.h"
#include "pico/platform.h"

#define PSRAM_CMD_QUAD_END 0xf5u
#define PSRAM_CMD_QUAD_ENABLE 0x35u
#define PSRAM_CMD_READ_ID 0x9fu
#define PSRAM_CMD_RSTEN 0x66u
#define PSRAM_CMD_RST 0x99u
#define PSRAM_CMD_QUAD_READ 0xebu
#define PSRAM_CMD_QUAD_WRITE 0x38u
#define PSRAM_CMD_NOOP 0xffu
#define PSRAM_CMD_LINEAR_TOGGLE 0xc0u
#define PSRAM_KGD_ID 0x5du

#define PSRAM_MAX_SCK_HZ 133000000u
#define PSRAM_ALIGNMENT 8u
#define PSRAM_MIN_SPLIT_SIZE 32u
#define PSRAM_BLOCK_MAGIC 0x5053524du

#if defined(RP2_BOARD_PSRAM_CS_PIN)
#define PSRAM_CS_PIN RP2_BOARD_PSRAM_CS_PIN
#endif

#if defined(RP2_BOARD_PSRAM_SIZE_BYTES)
#define PSRAM_BOARD_SIZE_LIMIT RP2_BOARD_PSRAM_SIZE_BYTES
#else
#define PSRAM_BOARD_SIZE_LIMIT 0u
#endif

typedef struct psram_block
{
    size_t size;
    struct psram_block *next;
    uint32_t magic;
    uint32_t free;
} psram_block_t;

static psram_block_t *g_free_list;
static size_t g_psram_total_size;
static bool g_psram_ready;

static size_t psram_align_up(size_t value)
{
    return (value + (PSRAM_ALIGNMENT - 1u)) & ~(size_t)(PSRAM_ALIGNMENT - 1u);
}

static bool psram_ptr_is_in_range(const void *ptr)
{
    const uintptr_t address = (uintptr_t)ptr;
    return g_psram_total_size > 0u &&
           address >= PSRAM_ALLOCATOR_BASE_ADDR &&
           address < (PSRAM_ALLOCATOR_BASE_ADDR + g_psram_total_size);
}

static void __no_inline_not_in_flash_func(psram_qmi_wait_busy)(void)
{
    while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) != 0u)
    {
        tight_loop_contents();
    }
}

static void __no_inline_not_in_flash_func(psram_direct_end)(void)
{
    qmi_hw->direct_csr &= ~(QMI_DIRECT_CSR_ASSERT_CS1N_BITS | QMI_DIRECT_CSR_EN_BITS);
}

static size_t __no_inline_not_in_flash_func(psram_detect_size)(uint8_t *out_kgd, uint8_t *out_eid)
{
    uint8_t kgd = 0u;
    uint8_t eid = 0u;
    uint32_t intr_stash = save_and_disable_interrupts();

    qmi_hw->direct_csr = (30u << QMI_DIRECT_CSR_CLKDIV_LSB) | QMI_DIRECT_CSR_EN_BITS;
    psram_qmi_wait_busy();

    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
    qmi_hw->direct_tx = QMI_DIRECT_TX_OE_BITS |
                        (QMI_DIRECT_TX_IWIDTH_VALUE_Q << QMI_DIRECT_TX_IWIDTH_LSB) |
                        PSRAM_CMD_QUAD_END;
    psram_qmi_wait_busy();
    (void)qmi_hw->direct_rx;
    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS1N_BITS;

    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
    for (size_t i = 0; i < 7u; ++i)
    {
        qmi_hw->direct_tx = (i == 0u) ? PSRAM_CMD_READ_ID : PSRAM_CMD_NOOP;

        while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_TXEMPTY_BITS) == 0u)
        {
            tight_loop_contents();
        }
        psram_qmi_wait_busy();

        if (i == 5u)
        {
            kgd = (uint8_t)qmi_hw->direct_rx;
        }
        else if (i == 6u)
        {
            eid = (uint8_t)qmi_hw->direct_rx;
        }
        else
        {
            (void)qmi_hw->direct_rx;
        }
    }

    psram_direct_end();
    restore_interrupts(intr_stash);

    if (out_kgd != NULL)
    {
        *out_kgd = kgd;
    }
    if (out_eid != NULL)
    {
        *out_eid = eid;
    }

    if (kgd != PSRAM_KGD_ID)
    {
        return 0u;
    }

    size_t psram_size = 1024u * 1024u;
    const uint8_t size_id = (uint8_t)(eid >> 5u);
    if (eid == 0x26u || size_id == 2u)
    {
        psram_size *= 8u;
    }
    else if (size_id == 1u)
    {
        psram_size *= 4u;
    }
    else if (size_id == 0u)
    {
        psram_size *= 1u;
    }
    else
    {
        psram_size = 0u;
    }

    if (PSRAM_BOARD_SIZE_LIMIT != 0u && psram_size > PSRAM_BOARD_SIZE_LIMIT)
    {
        psram_size = PSRAM_BOARD_SIZE_LIMIT;
    }

    return psram_size;
}

static void __no_inline_not_in_flash_func(psram_configure_timing)(void)
{
    const uint32_t clock_hz = clock_get_hz(clk_sys);
    uint32_t clock_divider = (clock_hz + PSRAM_MAX_SCK_HZ - 1u) / PSRAM_MAX_SCK_HZ;
    if (clock_divider == 1u && clock_hz > 100000000u)
    {
        clock_divider = 2u;
    }

    uint32_t rxdelay = clock_divider;
    if ((clock_hz / clock_divider) > 100000000u)
    {
        rxdelay += 1u;
    }

    const uint32_t clock_period_fs = (uint32_t)(1000000000000000ull / clock_hz);
    const uint32_t max_select = (125u * 1000000u) / clock_period_fs;
    const uint32_t min_deselect =
        ((18u * 1000000u) + (clock_period_fs - 1u)) / clock_period_fs -
        ((clock_divider + 1u) / 2u);

    qmi_hw->m[1].timing =
        (QMI_M1_TIMING_PAGEBREAK_VALUE_1024 << QMI_M1_TIMING_PAGEBREAK_LSB) |
        (1u << QMI_M1_TIMING_COOLDOWN_LSB) |
        (rxdelay << QMI_M1_TIMING_RXDELAY_LSB) |
        (max_select << QMI_M1_TIMING_MAX_SELECT_LSB) |
        (min_deselect << QMI_M1_TIMING_MIN_DESELECT_LSB) |
        (clock_divider << QMI_M1_TIMING_CLKDIV_LSB);
}

static size_t __no_inline_not_in_flash_func(psram_setup)(uint32_t psram_cs_pin,
                                                         uint8_t *out_kgd,
                                                         uint8_t *out_eid)
{
    gpio_set_function(psram_cs_pin, GPIO_FUNC_XIP_CS1);

    const size_t psram_size = psram_detect_size(out_kgd, out_eid);
    if (psram_size == 0u)
    {
        return 0u;
    }

    uint32_t intr_stash = save_and_disable_interrupts();
    qmi_hw->direct_csr = (30u << QMI_DIRECT_CSR_CLKDIV_LSB) | QMI_DIRECT_CSR_EN_BITS;
    psram_qmi_wait_busy();

    for (uint8_t i = 0u; i < 4u; ++i)
    {
        qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
        if (i == 0u)
        {
            qmi_hw->direct_tx = PSRAM_CMD_RSTEN;
        }
        else if (i == 1u)
        {
            qmi_hw->direct_tx = PSRAM_CMD_RST;
        }
        else if (i == 2u)
        {
            qmi_hw->direct_tx = PSRAM_CMD_QUAD_ENABLE;
        }
        else
        {
            qmi_hw->direct_tx = PSRAM_CMD_LINEAR_TOGGLE;
        }

        psram_qmi_wait_busy();
        qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
        for (size_t j = 0u; j < 20u; ++j)
        {
            __asm volatile("nop");
        }
        (void)qmi_hw->direct_rx;
    }

    psram_direct_end();

    psram_configure_timing();

    qmi_hw->m[1].rfmt =
        (QMI_M1_RFMT_PREFIX_WIDTH_VALUE_Q << QMI_M1_RFMT_PREFIX_WIDTH_LSB) |
        (QMI_M1_RFMT_ADDR_WIDTH_VALUE_Q << QMI_M1_RFMT_ADDR_WIDTH_LSB) |
        (QMI_M1_RFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M1_RFMT_SUFFIX_WIDTH_LSB) |
        (QMI_M1_RFMT_DUMMY_WIDTH_VALUE_Q << QMI_M1_RFMT_DUMMY_WIDTH_LSB) |
        (QMI_M1_RFMT_DUMMY_LEN_VALUE_24 << QMI_M1_RFMT_DUMMY_LEN_LSB) |
        (QMI_M1_RFMT_DATA_WIDTH_VALUE_Q << QMI_M1_RFMT_DATA_WIDTH_LSB) |
        (QMI_M1_RFMT_PREFIX_LEN_VALUE_8 << QMI_M1_RFMT_PREFIX_LEN_LSB) |
        (QMI_M1_RFMT_SUFFIX_LEN_VALUE_NONE << QMI_M1_RFMT_SUFFIX_LEN_LSB);
    qmi_hw->m[1].rcmd = PSRAM_CMD_QUAD_READ;

    qmi_hw->m[1].wfmt =
        (QMI_M1_WFMT_PREFIX_WIDTH_VALUE_Q << QMI_M1_WFMT_PREFIX_WIDTH_LSB) |
        (QMI_M1_WFMT_ADDR_WIDTH_VALUE_Q << QMI_M1_WFMT_ADDR_WIDTH_LSB) |
        (QMI_M1_WFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M1_WFMT_SUFFIX_WIDTH_LSB) |
        (QMI_M1_WFMT_DUMMY_WIDTH_VALUE_Q << QMI_M1_WFMT_DUMMY_WIDTH_LSB) |
        (QMI_M1_WFMT_DUMMY_LEN_VALUE_NONE << QMI_M1_WFMT_DUMMY_LEN_LSB) |
        (QMI_M1_WFMT_DATA_WIDTH_VALUE_Q << QMI_M1_WFMT_DATA_WIDTH_LSB) |
        (QMI_M1_WFMT_PREFIX_LEN_VALUE_8 << QMI_M1_WFMT_PREFIX_LEN_LSB) |
        (QMI_M1_WFMT_SUFFIX_LEN_VALUE_NONE << QMI_M1_WFMT_SUFFIX_LEN_LSB);
    qmi_hw->m[1].wcmd = PSRAM_CMD_QUAD_WRITE;

    xip_ctrl_hw->ctrl |= XIP_CTRL_WRITABLE_M1_BITS;
    restore_interrupts(intr_stash);

    return psram_size;
}

static void psram_heap_reset(size_t psram_size)
{
    g_free_list = (psram_block_t *)PSRAM_ALLOCATOR_BASE_ADDR;
    g_free_list->size = psram_size - sizeof(psram_block_t);
    g_free_list->next = NULL;
    g_free_list->magic = PSRAM_BLOCK_MAGIC;
    g_free_list->free = 1u;
}

static bool psram_probe_words(uintptr_t address, size_t word_count)
{
    volatile uint32_t *words = (volatile uint32_t *)address;
    static const uint32_t patterns[] = {
        0x00000000u,
        0xffffffffu,
        0xa5a5a5a5u,
        0x5a5a5a5au,
        0x12345678u,
        0x87654321u,
    };

    for (size_t pattern_index = 0u;
         pattern_index < (sizeof(patterns) / sizeof(patterns[0]));
         ++pattern_index)
    {
        const uint32_t pattern = patterns[pattern_index];
        for (size_t i = 0u; i < word_count; ++i)
        {
            words[i] = pattern ^ (uint32_t)(address + (i * sizeof(uint32_t)));
        }

        for (size_t i = 0u; i < word_count; ++i)
        {
            const uint32_t expected = pattern ^ (uint32_t)(address + (i * sizeof(uint32_t)));
            if (words[i] != expected)
            {
                return false;
            }
        }
    }

    return true;
}

static bool psram_probe_mapped_memory(size_t psram_size)
{
    const size_t probe_bytes = 64u;
    const size_t word_count = probe_bytes / sizeof(uint32_t);
    const uintptr_t base = PSRAM_ALLOCATOR_BASE_ADDR;
    const uintptr_t middle = base + ((psram_size / 2u) & ~(uintptr_t)(probe_bytes - 1u));
    const uintptr_t end = base + psram_size - probe_bytes;

    return psram_size >= (probe_bytes * 3u) &&
           psram_probe_words(base, word_count) &&
           psram_probe_words(middle, word_count) &&
           psram_probe_words(end, word_count);
}

bool psram_allocator_init(void)
{
    if (g_psram_ready)
    {
        return true;
    }

#if defined(PSRAM_CS_PIN)
    uint8_t kgd = 0u;
    uint8_t eid = 0u;
    const size_t psram_size = psram_setup(PSRAM_CS_PIN, &kgd, &eid);
    if (psram_size <= sizeof(psram_block_t))
    {
        printf("[PSRAM]\t\tinit failed cs=%u id=0x%02x eid=0x%02x\n",
               (unsigned)PSRAM_CS_PIN,
               (unsigned)kgd,
               (unsigned)eid);
        g_psram_total_size = 0u;
        g_psram_ready = false;
        return false;
    }

    if (!psram_probe_mapped_memory(psram_size))
    {
        printf("[PSRAM]\t\tself-test failed cs=%u id=0x%02x eid=0x%02x size=%u\n",
               (unsigned)PSRAM_CS_PIN,
               (unsigned)kgd,
               (unsigned)eid,
               (unsigned)psram_size);
        g_psram_total_size = 0u;
        g_psram_ready = false;
        return false;
    }

    g_psram_total_size = psram_size;
    psram_heap_reset(psram_size);
    g_psram_ready = true;

    printf("[PSRAM]\t\tinit ok cs=%u id=0x%02x eid=0x%02x size=%u base=0x%08x self-test=ok\n",
           (unsigned)PSRAM_CS_PIN,
           (unsigned)kgd,
           (unsigned)eid,
           (unsigned)g_psram_total_size,
           (unsigned)PSRAM_ALLOCATOR_BASE_ADDR);
    return true;
#else
    printf("[PSRAM]\t\tdisabled: no board PSRAM CS pin\n");
    return false;
#endif
}

bool psram_allocator_is_ready(void)
{
    return g_psram_ready;
}

bool psram_allocator_self_test(void)
{
    if (!g_psram_ready && !psram_allocator_init())
    {
        return false;
    }

    enum
    {
        TEST_SIZE = 512
    };

    uint8_t *buffer = (uint8_t *)psram_malloc(TEST_SIZE);
    if (buffer == NULL)
    {
        return false;
    }

    for (size_t i = 0u; i < TEST_SIZE; ++i)
    {
        buffer[i] = (uint8_t)(0xa5u ^ (uint8_t)i);
    }

    bool ok = true;
    for (size_t i = 0u; i < TEST_SIZE; ++i)
    {
        if (buffer[i] != (uint8_t)(0xa5u ^ (uint8_t)i))
        {
            ok = false;
            break;
        }
    }

    psram_free(buffer);
    return ok;
}

void *psram_malloc(size_t size)
{
    if (!g_psram_ready && !psram_allocator_init())
    {
        return NULL;
    }

    if (size == 0u)
    {
        return NULL;
    }

    const size_t aligned_size = psram_align_up(size);
    uint32_t intr_stash = save_and_disable_interrupts();

    psram_block_t *prev = NULL;
    psram_block_t *block = g_free_list;
    while (block != NULL)
    {
        if (block->free != 0u && block->size >= aligned_size)
        {
            const size_t remaining = block->size - aligned_size;
            if (remaining >= (sizeof(psram_block_t) + PSRAM_MIN_SPLIT_SIZE))
            {
                psram_block_t *next =
                    (psram_block_t *)((uint8_t *)(block + 1) + aligned_size);
                next->size = remaining - sizeof(psram_block_t);
                next->next = block->next;
                next->magic = PSRAM_BLOCK_MAGIC;
                next->free = 1u;
                block->size = aligned_size;
                block->next = next;
            }

            if (prev == NULL)
            {
                g_free_list = block->next;
            }
            else
            {
                prev->next = block->next;
            }

            block->next = NULL;
            block->free = 0u;
            restore_interrupts(intr_stash);
            return (void *)(block + 1);
        }

        prev = block;
        block = block->next;
    }

    restore_interrupts(intr_stash);
    return NULL;
}

void *psram_calloc(size_t count, size_t size)
{
    if (count == 0u || size == 0u || size > (SIZE_MAX / count))
    {
        return NULL;
    }

    const size_t total_size = count * size;
    void *ptr = psram_malloc(total_size);
    if (ptr != NULL)
    {
        memset(ptr, 0, total_size);
    }
    return ptr;
}

void psram_free(void *ptr)
{
    if (ptr == NULL)
    {
        return;
    }

    if (!psram_ptr_is_in_range(ptr))
    {
        return;
    }

    psram_block_t *block = ((psram_block_t *)ptr) - 1;
    if (block->magic != PSRAM_BLOCK_MAGIC || block->free != 0u)
    {
        return;
    }

    uint32_t intr_stash = save_and_disable_interrupts();
    block->free = 1u;

    psram_block_t *prev = NULL;
    psram_block_t *cur = g_free_list;
    while (cur != NULL && cur < block)
    {
        prev = cur;
        cur = cur->next;
    }

    block->next = cur;
    if (prev == NULL)
    {
        g_free_list = block;
    }
    else
    {
        prev->next = block;
    }

    if (block->next != NULL &&
        ((uint8_t *)(block + 1) + block->size) == (uint8_t *)block->next)
    {
        block->size += sizeof(psram_block_t) + block->next->size;
        block->next = block->next->next;
    }

    if (prev != NULL && ((uint8_t *)(prev + 1) + prev->size) == (uint8_t *)block)
    {
        prev->size += sizeof(psram_block_t) + block->size;
        prev->next = block->next;
    }

    restore_interrupts(intr_stash);
}

void *psram_realloc(void *ptr, size_t size)
{
    if (ptr == NULL)
    {
        return psram_malloc(size);
    }

    if (size == 0u)
    {
        psram_free(ptr);
        return NULL;
    }

    psram_block_t *block = ((psram_block_t *)ptr) - 1;
    if (!psram_ptr_is_in_range(ptr) || block->magic != PSRAM_BLOCK_MAGIC)
    {
        return NULL;
    }

    if (block->size >= size)
    {
        return ptr;
    }

    void *new_ptr = psram_malloc(size);
    if (new_ptr == NULL)
    {
        return NULL;
    }

    memcpy(new_ptr, ptr, block->size);
    psram_free(ptr);
    return new_ptr;
}

bool psram_contains_ptr(const void *ptr)
{
    return psram_ptr_is_in_range(ptr);
}

size_t psram_total_size(void)
{
    return g_psram_total_size;
}

size_t psram_free_size(void)
{
    size_t total = 0u;
    uint32_t intr_stash = save_and_disable_interrupts();
    for (psram_block_t *block = g_free_list; block != NULL; block = block->next)
    {
        total += block->size;
    }
    restore_interrupts(intr_stash);
    return total;
}

size_t psram_largest_free_block(void)
{
    size_t largest = 0u;
    uint32_t intr_stash = save_and_disable_interrupts();
    for (psram_block_t *block = g_free_list; block != NULL; block = block->next)
    {
        if (block->size > largest)
        {
            largest = block->size;
        }
    }
    restore_interrupts(intr_stash);
    return largest;
}
