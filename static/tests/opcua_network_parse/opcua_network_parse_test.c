#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* MODEL_PARSE_IMPLEMENTATIONS */

static void assert_bytes(const uint8_t *actual, const uint8_t *expected, size_t count)
{
    assert(memcmp(actual, expected, count) == 0);
}

static void test_ipv4_parser(void)
{
    static const uint8_t expected_normal[4] = {192u, 168u, 1u, 20u};
    static const uint8_t expected_upper[4] = {255u, 255u, 255u, 255u};
    static const uint8_t expected_trailing[4] = {1u, 2u, 3u, 4u};
    uint8_t parsed[4] = {0};

    assert(model_parse_ipv4_string("192.168.1.20", parsed));
    assert_bytes(parsed, expected_normal, sizeof(parsed));
    assert(model_parse_ipv4_string("255.255.255.255", parsed));
    assert_bytes(parsed, expected_upper, sizeof(parsed));
    assert(!model_parse_ipv4_string(NULL, parsed));
    assert(!model_parse_ipv4_string("1.2.3.4", NULL));
    assert(!model_parse_ipv4_string("", parsed));
    assert(!model_parse_ipv4_string("1.2.3", parsed));
    assert(!model_parse_ipv4_string("1.2.3.x", parsed));
    assert(!model_parse_ipv4_string("1.2.3.256", parsed));

    /* Characterization: the sscanf format accepts text after four octets. */
    assert(model_parse_ipv4_string("1.2.3.4suffix", parsed));
    assert_bytes(parsed, expected_trailing, sizeof(parsed));
    assert(model_parse_ipv4_string("1.2.3.4.5", parsed));
    assert_bytes(parsed, expected_trailing, sizeof(parsed));
}

static void test_mac_parser(void)
{
    static const uint8_t expected_colon[6] = {0u, 17u, 34u, 51u, 68u, 85u};
    static const uint8_t expected_hyphen[6] = {255u, 238u, 221u, 204u, 187u, 170u};
    static const uint8_t expected_single_digit[6] = {10u, 11u, 12u, 13u, 14u, 15u};
    static const uint8_t expected_trailing[6] = {1u, 2u, 3u, 4u, 5u, 6u};
    uint8_t parsed[6] = {0};

    assert(model_parse_mac_string("00:11:22:33:44:55", parsed));
    assert_bytes(parsed, expected_colon, sizeof(parsed));
    assert(model_parse_mac_string("ff-ee-dd-cc-bb-aa", parsed));
    assert_bytes(parsed, expected_hyphen, sizeof(parsed));

    /* Current legacy policy: %x accepts one hexadecimal digit per byte. */
    assert(model_parse_mac_string("a:b:c:d:e:f", parsed));
    assert_bytes(parsed, expected_single_digit, sizeof(parsed));
    assert(!model_parse_mac_string(NULL, parsed));
    assert(!model_parse_mac_string("00:11:22:33:44:55", NULL));
    assert(!model_parse_mac_string("00:11:22:33:44", parsed));
    assert(!model_parse_mac_string("00-11:22-33:44-55", parsed));
    assert(!model_parse_mac_string("gg:11:22:33:44:55", parsed));
    assert(!model_parse_mac_string("100:11:22:33:44:55", parsed));
    assert(!model_parse_mac_string("1ff:11:22:33:44:55", parsed));

    /* Characterization: both formats accept a suffix after the sixth byte. */
    assert(model_parse_mac_string("01:02:03:04:05:06suffix", parsed));
    assert_bytes(parsed, expected_trailing, sizeof(parsed));
    assert(model_parse_mac_string("01-02-03-04-05-06-07", parsed));
    assert_bytes(parsed, expected_trailing, sizeof(parsed));
}

int main(void)
{
    test_ipv4_parser();
    test_mac_parser();
    puts("OPC UA network parser extracted-source suite: PASS (22 cases)");
    return 0;
}
