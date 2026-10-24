#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static uint32_t portable_free_words(const uint8_t *stack)
{
    uint32_t bytes = 0u;

    while (*stack == 0xa5u)
    {
        ++stack;
        ++bytes;
    }

    return bytes / (uint32_t)sizeof(uint32_t);
}

static uint32_t rp2350_free_words(const uint8_t *stack, size_t words_available)
{
    const uint32_t *words = (const uint32_t *)stack;
    const uint32_t *last = words + words_available - 1u;
    uint32_t count = 0u;

    while (((size_t)(last - words) >= 3u) &&
           (((words[0] ^ UINT32_C(0xa5a5a5a5)) |
            (words[1] ^ UINT32_C(0xa5a5a5a5)) |
            (words[2] ^ UINT32_C(0xa5a5a5a5)) |
            (words[3] ^ UINT32_C(0xa5a5a5a5))) == 0u))
    {
        words += 4;
        count += 4u;
    }

    while ((words <= last) && (*words == UINT32_C(0xa5a5a5a5)))
    {
        ++words;
        ++count;
    }

    return count;
}

int main(void)
{
    enum { tested_words = 32u, tail_words = 4u };
    _Alignas(uint32_t) uint8_t stack[(tested_words + tail_words) * sizeof(uint32_t)];

    for (size_t first_non_fill = 0u;
         first_non_fill <= tested_words * sizeof(uint32_t);
         ++first_non_fill)
    {
        memset(stack, 0xa5, sizeof(stack));
        stack[first_non_fill] = 0u;
        assert(rp2350_free_words(stack, tested_words) ==
               portable_free_words(stack));
        assert(rp2350_free_words(stack, tested_words) ==
               first_non_fill / sizeof(uint32_t));
    }

    return 0;
}
