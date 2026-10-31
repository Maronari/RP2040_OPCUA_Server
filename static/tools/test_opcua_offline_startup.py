#!/usr/bin/env python3
"""Guard DHCP-independent OPC UA startup and late discovery publication.

The model task is intentionally too coupled to open62541 and FreeRTOS for a
host-compiled unit harness. These source assertions protect the exact startup
prerequisite, endpoint-selection, and transactional discovery-update control
flow: an initialized network stack is mandatory, while an IPv4 lease is not.
"""

from __future__ import annotations

import argparse
import pathlib
import subprocess
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE_PATH = ROOT / "opcua_model" / "src" / "model_core_lifecycle.c"


def extract_block(source: str, start: int) -> str:
    """Return the balanced brace block starting at or after *start*."""
    opening = source.find("{", start)
    if opening < 0:
        raise AssertionError("missing opening brace")

    depth = 0
    for index in range(opening, len(source)):
        character = source[index]
        if character == "{":
            depth += 1
        elif character == "}":
            depth -= 1
            if depth == 0:
                return source[opening:index + 1]
    raise AssertionError("unbalanced source block")


def require_once(source: str, text: str) -> int:
    count = source.count(text)
    assert count == 1, f"expected one {text!r}, found {count}"
    return source.index(text)


def run_discovery_helper(source: str, compiler: str) -> None:
    """Compile the actual helper with allocation/copy-failure mocks."""
    signature = "static UA_StatusCode model_publish_discovery_url("
    helper_start = source.index(signature)
    helper_opening = source.index("{", helper_start)
    implementation = source[helper_start:helper_opening] + extract_block(source, helper_start)
    harness = f"""
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint32_t UA_StatusCode;
#define UA_STATUSCODE_GOOD ((UA_StatusCode)0u)
#define UA_STATUSCODE_BADINVALIDARGUMENT ((UA_StatusCode)1u)
#define UA_STATUSCODE_BADOUTOFMEMORY ((UA_StatusCode)2u)
#define UA_STATUSCODE_BADINTERNALERROR ((UA_StatusCode)3u)

typedef struct {{ size_t length; uint8_t *data; }} UA_String;
typedef struct {{
    bool initialized;
    bool has_ipv4;
    char ipv4_address[16];
}} network_manager_status_t;
typedef struct {{ UA_String *discoveryUrls; size_t discoveryUrlsSize; }} UA_ApplicationDescription;
typedef struct {{ UA_ApplicationDescription applicationDescription; }} UA_ServerConfig;

static int UA_TYPES[1];
#define UA_TYPES_STRING 0
static bool fail_array_allocation;
static bool fail_string_copy;
static size_t live_arrays;
static size_t live_strings;

#define UA_STRING(text) ((UA_String){{ strlen(text), (uint8_t *)(text) }})

static void *UA_Array_new(size_t count, const int *type) {{
    (void)type;
    if (fail_array_allocation) {{
        return NULL;
    }}
    UA_String *result = calloc(count, sizeof(*result));
    if (result != NULL) {{
        ++live_arrays;
    }}
    return result;
}}

static UA_StatusCode UA_String_copy(const UA_String *source, UA_String *destination) {{
    if (fail_string_copy) {{
        return UA_STATUSCODE_BADINTERNALERROR;
    }}
    destination->data = malloc(source->length);
    if (destination->data == NULL && source->length != 0u) {{
        return UA_STATUSCODE_BADOUTOFMEMORY;
    }}
    if (source->length != 0u) {{
        memcpy(destination->data, source->data, source->length);
    }}
    destination->length = source->length;
    ++live_strings;
    return UA_STATUSCODE_GOOD;
}}

static void UA_String_clear(UA_String *value) {{
    if (value->data != NULL) {{
        free(value->data);
        --live_strings;
    }}
    value->data = NULL;
    value->length = 0u;
}}

static void UA_free(void *value) {{
    if (value != NULL) {{
        free(value);
        --live_arrays;
    }}
}}

static const char k_model_opc_tcp_url_format[] = "opc.tcp://%s:%u/";

{implementation}

#define ASSERT(condition) do {{ \
    if (!(condition)) {{ \
        fprintf(stderr, "assertion failed: %s\\n", #condition); \
        return 1; \
    }} \
}} while (0)

static UA_String make_owned(const char *text) {{
    UA_String owned = {{0}};
    UA_String borrowed = UA_STRING(text);
    if (UA_String_copy(&borrowed, &owned) != UA_STATUSCODE_GOOD) {{
        abort();
    }}
    return owned;
}}

int main(void) {{
    UA_ServerConfig config = {{0}};
    network_manager_status_t no_lease = {{ .initialized = true }};
    network_manager_status_t lease = {{ .initialized = true, .has_ipv4 = true,
                                       .ipv4_address = "10.20.30.40" }};
    UA_String *original = UA_Array_new(1u, &UA_TYPES[UA_TYPES_STRING]);
    ASSERT(original != NULL);
    original[0] = make_owned("opc.tcp://old:4840/");
    config.applicationDescription.discoveryUrls = original;
    config.applicationDescription.discoveryUrlsSize = 1u;

    ASSERT(model_publish_discovery_url(&config, &no_lease, 4840u) ==
           UA_STATUSCODE_BADINVALIDARGUMENT);
    ASSERT(config.applicationDescription.discoveryUrls == original);
    ASSERT(config.applicationDescription.discoveryUrlsSize == 1u);

    fail_array_allocation = true;
    ASSERT(model_publish_discovery_url(&config, &lease, 4840u) ==
           UA_STATUSCODE_BADOUTOFMEMORY);
    ASSERT(config.applicationDescription.discoveryUrls == original);
    fail_array_allocation = false;

    fail_string_copy = true;
    ASSERT(model_publish_discovery_url(&config, &lease, 4840u) ==
           UA_STATUSCODE_BADINTERNALERROR);
    ASSERT(config.applicationDescription.discoveryUrls == original);
    ASSERT(live_arrays == 1u && live_strings == 1u);
    fail_string_copy = false;

    ASSERT(model_publish_discovery_url(&config, &lease, 4840u) == UA_STATUSCODE_GOOD);
    ASSERT(config.applicationDescription.discoveryUrlsSize == 1u);
    ASSERT(config.applicationDescription.discoveryUrls != original);
    ASSERT(config.applicationDescription.discoveryUrls[0].length ==
           strlen("opc.tcp://10.20.30.40:4840/"));
    ASSERT(memcmp(config.applicationDescription.discoveryUrls[0].data,
                  "opc.tcp://10.20.30.40:4840/",
                  config.applicationDescription.discoveryUrls[0].length) == 0);
    ASSERT(live_arrays == 1u && live_strings == 1u);

    ASSERT(model_publish_discovery_url(&config, &lease, 4840u) == UA_STATUSCODE_GOOD);
    ASSERT(config.applicationDescription.discoveryUrlsSize == 1u);
    ASSERT(live_arrays == 1u && live_strings == 1u);

    UA_String_clear(&config.applicationDescription.discoveryUrls[0]);
    UA_free(config.applicationDescription.discoveryUrls);
    ASSERT(live_arrays == 0u && live_strings == 0u);
    return 0;
}}
"""
    with tempfile.TemporaryDirectory(prefix="opcua-offline-startup-") as directory:
        directory_path = pathlib.Path(directory)
        source_path = directory_path / "discovery_helper_test.c"
        executable_path = directory_path / "discovery_helper_test.exe"
        source_path.write_text(harness, encoding="utf-8", newline="\n")
        subprocess.run(
            [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
             str(source_path), "-o", str(executable_path)],
            check=True,
        )
        subprocess.run([str(executable_path)], check=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", default="gcc", help="Host C compiler")
    args = parser.parse_args()
    source = SOURCE_PATH.read_text(encoding="utf-8")

    status_declaration = require_once(
        source, "network_manager_status_t network_status = {0};"
    )
    status_call = "network_manager_get_status(&network_status);"
    assert source.count(status_call) == 2
    status_read = source.index(status_call)
    status_refresh = source.index(status_call, status_read + 1)
    assert status_declaration < status_read
    benchmark_wait = """#if RP2_BENCHMARK_PROFILE_ENABLED
    /* Preserve the established lease-inclusive synthetic benchmark timing.
     * Production and measurement builds intentionally proceed without DHCP. */
    while (!network_manager_is_ready())
    {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
#endif"""
    assert source.count("network_manager_is_ready()") == 1
    assert benchmark_wait in source
    assert source.index(benchmark_wait) < status_read

    initialized_guard = "if (!network_status.initialized)"
    initialized_index = require_once(source, initialized_guard)
    initialized_block = extract_block(source, initialized_index)
    assert "vTaskDelete(NULL);" in initialized_block
    assert initialized_index < source.index("UA_ServerConfig_setMinimalCustomBuffer", initialized_index)
    assert initialized_index < source.index("UA_Server_newWithConfig", initialized_index)

    no_ipv4_guard = "if (!network_status.has_ipv4)"
    assert source.count(no_ipv4_guard) == 2
    no_ipv4_index = source.index(no_ipv4_guard)
    no_ipv4_block = extract_block(source, no_ipv4_index)
    assert "OTA_MEASUREMENT_PHASE_WAITING_IPV4" in no_ipv4_block
    assert "OTA_MEASUREMENT_STATE_UNAVAILABLE" in no_ipv4_block
    assert "vTaskDelete(NULL);" not in no_ipv4_block
    assert "vTaskDelay(" not in no_ipv4_block

    url_selection = source[status_refresh:
                           source.index("#ifdef UA_ENABLE_ENCRYPTION_MBEDTLS", status_refresh)]
    assert "discovery_url_update_pending = !network_status.has_ipv4;" in url_selection
    assert "No IPv4 address at server creation; using wildcard endpoint" in url_selection
    url_no_ipv4_index = source.index(no_ipv4_guard, no_ipv4_index + 1)
    url_no_ipv4_block = extract_block(source, url_no_ipv4_index)
    assert "vTaskDelete(NULL);" not in url_no_ipv4_block
    assert "vTaskDelay(" not in url_no_ipv4_block

    ipv4_branch = source[no_ipv4_index:source.index("model_bind_open62541_allocators", no_ipv4_index)]
    assert "IPv4 is ready, continuing startup" in ipv4_branch
    assert "OTA_MEASUREMENT_PHASE_IPV4_READY" in ipv4_branch
    assert "OTA_MEASUREMENT_STATE_OK" in ipv4_branch

    url_guard = (
        "if (network_status.has_ipv4 && network_status.ipv4_address[0] != '\\0' &&\n"
        "        g_model_config.hostname != NULL && g_model_config.hostname[0] != '\\0')"
    )
    url_index = require_once(source, url_guard)
    url_block = extract_block(source, url_index)
    assert "const char *hostname_text = network_status.ipv4_address;" in url_block
    assert "model_configure_server_url(config, hostname_text, g_model_config.port)" in url_block
    assert source.index("UA_ServerConfig_setMinimalCustomBuffer", initialized_index) < url_index
    assert "const char *hostname_text = g_model_config.hostname;" not in source

    allocation_failure = extract_block(
        source, source.index("if (status != UA_STATUSCODE_GOOD)",
                             source.index("UA_ServerConfig_setMinimalCustomBuffer"))
    )
    assert "UA_ServerConfig_setMinimalCustomBuffer failed" in allocation_failure
    assert "vTaskDelete(NULL);" in allocation_failure

    helper_signature = "static UA_StatusCode model_publish_discovery_url("
    helper_index = require_once(source, helper_signature)
    helper_block = extract_block(source, helper_index)
    assert "k_model_opc_tcp_url_format" in helper_block
    assert "UA_Array_new(1u, &UA_TYPES[UA_TYPES_STRING])" in helper_block
    assert "UA_STATUSCODE_BADOUTOFMEMORY" in helper_block
    assert "UA_String_copy(&endpoint_string, &replacement_urls[0])" in helper_block
    assert "UA_String_clear(&replacement_urls[0]);" in helper_block
    assert "UA_free(replacement_urls);" in helper_block
    assert helper_block.index("UA_String_copy(&endpoint_string, &replacement_urls[0])") < \
        helper_block.index("previous_urls = config->applicationDescription.discoveryUrls;")
    assert helper_block.index("previous_urls = config->applicationDescription.discoveryUrls;") < \
        helper_block.index("config->applicationDescription.discoveryUrls = replacement_urls;")
    assert "!network_status->has_ipv4" in helper_block
    assert "network_status->ipv4_address[0] == '\\0'" in helper_block

    late_update = extract_block(source, source.index("if (discovery_url_update_pending)",
                                                      source.index("const TickType_t diag_now")))
    assert "late_network_status.initialized && late_network_status.has_ipv4" in late_update
    assert "model_publish_discovery_url(config," in late_update
    assert "discovery_url_update_pending = false;" in late_update
    # newWithConfig moves/clears the stack configuration. Late publication
    # must use the live server-owned config rather than that moved-from object.
    task_start = source.index("void model_core_task(void *argument)")
    server_create = source.index("server = UA_Server_newWithConfig(config);", task_start)
    live_config = source.index("config = UA_Server_getConfig(server);", server_create)
    late_publish = source.index("model_publish_discovery_url(config,", live_config)
    assert server_create < live_config < late_publish

    assert source.count("static const char k_model_opc_tcp_url_format[] = \"opc.tcp://%s:%u/\";") == 1
    assert "Waiting for IPv4 before server startup" not in source
    assert "Network manager is not initialized" in source

    run_discovery_helper(source, args.cc)

    print("OPC UA offline-startup source contract: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
