#!/usr/bin/env python3
"""Fail-closed static checks for the v0.5.2 T4 OPC UA benchmark adapter.

The live profile matrix needs a reference board and is deliberately outside
this host check.  This check proves that the source cannot silently turn a
production image into a plaintext/no-events image, and that the explicit
test-none/events-off branches preserve bounded role handling.
"""

from __future__ import annotations

import pathlib
import re
import sys


ROOT = pathlib.Path(__file__).resolve().parents[2]
EVENTS_OFF = "RP2_BENCHMARK_PROFILE_ENABLED && !RP2_BENCHMARK_EVENTS_ENABLED"
TEST_NONE = "RP2_BENCHMARK_PROFILE_ENABLED && RP2_BENCHMARK_SECURITY_NONE"


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def function_body(source: str, signature: str) -> str:
    """Extract one C function body without treating comments as structure."""
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start:index + 1]
    raise AssertionError(f"unterminated function: {signature}")


def main() -> int:
    header = read("services/include/services/benchmark_profile.h")
    lifecycle = read("opcua_model/src/model_core_lifecycle.c")
    access = read("opcua_model/src/model_access.c")
    configuration = read("opcua_model/src/model_configuration.c")
    maintenance = read("opcua_model/src/model_maintenance.c")
    events = read("opcua_model/src/model_events.c")
    alarms = read("services/src/alarm_service.c")

    require(re.search(r"#ifndef RP2_BENCHMARK_PROFILE_ENABLED\s+#define RP2_BENCHMARK_PROFILE_ENABLED 0", header),
            "benchmark profile must default disabled")
    require(re.search(r"#ifndef RP2_BENCHMARK_SECURITY_NONE\s+#define RP2_BENCHMARK_SECURITY_NONE 0", header),
            "security-none must default disabled")
    require(re.search(r"#ifndef RP2_BENCHMARK_EVENTS_ENABLED\s+#define RP2_BENCHMARK_EVENTS_ENABLED 1", header),
            "events must default enabled")

    none_config = function_body(lifecycle, "static UA_StatusCode model_configure_benchmark_none_server")
    require("model_configure_secure_access_control(config)" in none_config,
            "test-none endpoint bypasses username login and role wrappers")
    require("config->allowNonePolicyPassword = true;" in none_config,
            "test-none endpoint does not explicitly retain Username authentication")
    require("UA_ServerConfig_addSecurityPolicyNone(config, NULL)" in none_config and
            "UA_ServerConfig_addEndpoint(config," in none_config and
            "UA_MESSAGESECURITYMODE_NONE" in none_config,
            "test-none endpoint is not explicitly None-only")
    require("UA_ServerConfig_addAllEndpoints" not in none_config and
            "UA_ServerConfig_addSecurityPolicyBasic256Sha256" not in none_config,
            "test-none helper creates an endpoint or policy beyond None")
    require(lifecycle.count("allowNonePolicyPassword = true;") == 1,
            "cleartext Username authentication escaped the test-none helper")

    secure_config = function_body(lifecycle, "UA_StatusCode model_configure_secure_server")
    require(f"#if {TEST_NONE}" in secure_config and
            "return model_configure_benchmark_none_server(config);" in secure_config and
            "#else" in secure_config and
            "UA_ServerConfig_addSecurityPolicyBasic256Sha256" in secure_config and
            "model_keep_only_signandencrypt_endpoints(config)" in secure_config,
            "test-none branch is not isolated from the production secure path")
    require(f"#if !({TEST_NONE})" in access,
            "plaintext role exception is not compile-selected test-only")
    write_adapter = function_body(configuration, "UA_StatusCode model_write_value")
    require(f"#if !({TEST_NONE})" in write_adapter and
            "if (!model_session_is_secure(session_context))" in write_adapter and
            "UA_STATUSCODE_BADSECURITYMODEINSUFFICIENT" in write_adapter,
            "test-none channel Write transport exception is not compile-selected test-only")
    require("model_session_get_role(session_context, &current_role)" in access and
            "current_role < required_role" in access,
            "test-none bypasses role authorization instead of only transport security")

    for signature, expectation in (
        ("UA_StatusCode model_emit_event_ex", "custom-event gate"),
        ("UA_StatusCode model_publish_prepared_maintenance_event", "maintenance-event gate"),
        ("UA_StatusCode model_sync_maintenance_condition", "maintenance-condition gate"),
        ("UA_StatusCode model_build_alarm_conditions", "A&C construction gate"),
        ("void model_sync_condition", "A&C transition gate"),
        ("void model_process_pending_audit_events", "audit-drain gate"),
    ):
        require(f"#if {EVENTS_OFF}" in function_body(maintenance, signature),
                f"missing {expectation}")
    require(f"#if {EVENTS_OFF}" in function_body(events, "void model_security_audit_callback"),
            "security audit callback enqueues OPC UA events while events are disabled")
    require(f"#if {EVENTS_OFF}" in function_body(events, "UA_StatusCode model_build_channel_alarm_conditions"),
            "channel A&C construction is active while events are disabled")
    require(f"#if {EVENTS_OFF}" in function_body(alarms, "void alarm_service_evaluate_all"),
            "channel alarm evaluation is active while events are disabled")

    # T4 intentionally changes profile execution only. It reserves no public
    # NodeId, BrowseName, method, or capacity, and it must not invent one here.
    combined = lifecycle + maintenance + events + access + configuration
    require("Device.Benchmark" not in combined and "BenchmarkProfile" not in combined,
            "T4 adapter introduced a public benchmark model contract")

    print("v0.5.2 benchmark OPC UA adapter source suite: PASS")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (AssertionError, ValueError) as exc:
        print(f"v0.5.2 benchmark OPC UA adapter source suite: FAIL: {exc}", file=sys.stderr)
        raise SystemExit(1)
