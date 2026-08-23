#!/usr/bin/env python3
"""Focused no-hardware checks for the v0.4.2 channel-model contract."""

from __future__ import annotations

import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from verify_channel_model import channel_node_id  # noqa: E402


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"channel-model-contract: FAIL: {message}")


def main() -> int:
    require(channel_node_id("channel.temperature.1") ==
            "Device.Channels.c_6368616E6E656C2E74656D70657261747572652E31",
            "temperature NodeId grammar drifted")
    for channel_id in ("a.b", "a_b", "a/Value", "x" * 31):
        node_id = channel_node_id(channel_id, "Value")
        require(node_id.endswith(".Value") and len(node_id) < 128,
                f"unbounded or ambiguous NodeId for {channel_id!r}")
    source = (ROOT / "opcua_model/src/model_address_space.c").read_text(encoding="utf-8")
    require("MODEL_CHANNEL_NAMESPACE_URI" in source, "generic namespace is not registered")
    require("UA_Server_deleteNode" in source and "model_rollback_channel_model" in source,
            "channel construction has no explicit rollback")
    require("model_delete_legacy_channel_subtree" in source and
            "model_clear_legacy_channel_contexts" in source and
            "goto fail;" in source,
            "legacy subtree creation has no deterministic failure cleanup")
    require("EngineeringUnits" in source and "UA_TYPES_EUINFORMATION" in source,
            "EUInformation property is absent")
    require("EURange" in source and "UA_TYPES_RANGE" in source, "Range property is absent")
    require(
        "status = model_add_legacy_temperature_nodes(server, channels_id, channel_index, channel);"
        in source,
        "v0.4.1 channel aliases are not constructed for every registry channel",
    )
    history = (ROOT / "opcua_model/src/model_history.c").read_text(encoding="utf-8")
    require(
        "UA_NODEID_STRING(g_model_channel_namespace_index" in history,
        "generic HistoryRead lookup does not use the channel-model namespace",
    )
    require(
        "strcmp(channel->metadata.id, DEVICE_CHANNEL_ID_TEMPERATURE_1)" not in history,
        "legacy HistoryRead lookup is restricted to temperature",
    )
    pubsub = (ROOT / "opcua_model/src/model_pubsub.c").read_text(encoding="utf-8")
    for field in ("Value", "Status", "SourceTimestamp", "AlarmState"):
        require(f'"{field}");' in pubsub, f"PubSub field {field} was removed")
    configuration = (ROOT / "opcua_model/src/model_configuration.c").read_text(encoding="utf-8")
    require("channel_manager_write_value" in configuration, "Value callback bypasses service write API")
    require("BADSECURITYMODEINSUFFICIENT" in configuration, "generic write does not require secure session")
    access = (ROOT / "opcua_model/src/model_access.c").read_text(encoding="utf-8")
    for field in (
        "lhs->kind != rhs->kind", "lhs->driver_id != rhs->driver_id",
        "lhs->source_index != rhs->source_index", "lhs->source_flags != rhs->source_flags",
        "(lhs->has_minimum != 0u) != (rhs->has_minimum != 0u)",
        "(lhs->has_maximum != 0u) != (rhs->has_maximum != 0u)",
        "lhs->minimum != rhs->minimum", "lhs->maximum != rhs->maximum",
        "model_string_changed(lhs->description, rhs->description)",
        "model_string_changed(lhs->unit, rhs->unit)",
        "lhs->settings.sample_period_ms != rhs->settings.sample_period_ms",
        "lhs->settings.publish_period_ms != rhs->settings.publish_period_ms",
        "TELEMETRY_CHANNEL_FLAG_WRITABLE",
    ):
        require(field in access, f"startup-materialized channel field missing from rebuild check: {field}")
    print("channel-model-contract: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
