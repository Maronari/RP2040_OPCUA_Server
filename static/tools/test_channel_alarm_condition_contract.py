#!/usr/bin/env python3
"""Guard the v0.4.3 per-channel Alarms & Conditions model (T6) contract.

Compiling and running a full open62541 server is out of scope for the host
suite (see the other source-contract tests in this directory for the
existing precedent, e.g. test_maintenance_condition_active_state_contract.py)
so this asserts the structural contract by pattern-matching the actual
source instead: one condition instance is created per alarm-eligible
channel using the frozen NodeId grammar, conditions are reflected from
alarm_service (never evaluated in the OPC UA layer), acknowledgement is
synced bidirectionally per channel, and the preserved
Device.Maintenance.AcknowledgeAllAlarms contract is untouched.
"""

from __future__ import annotations

import pathlib
import re
import sys


ROOT_RELATIVE = (
    "opcua_model/src/model_events.c",
    "opcua_model/src/model_maintenance.c",
    "opcua_model/src/model_configuration.c",
    "opcua_model/src/opcua_model_internal.h",
)


def require(pattern: str, source: str, description: str) -> None:
    if re.search(pattern, source, re.DOTALL) is None:
        raise AssertionError(description)


def forbid(pattern: str, source: str, description: str) -> None:
    if re.search(pattern, source, re.DOTALL) is not None:
        raise AssertionError(description)


def main() -> int:
    root = pathlib.Path(__file__).resolve().parents[2]
    sources = {path: (root / path).read_text(encoding="utf-8") for path in ROOT_RELATIVE}
    events = sources["opcua_model/src/model_events.c"]
    maintenance = sources["opcua_model/src/model_maintenance.c"]
    configuration = sources["opcua_model/src/model_configuration.c"]
    internal_h = sources["opcua_model/src/opcua_model_internal.h"]
    combined = events + maintenance + configuration + internal_h

    # Condition type: subtype of the standard SystemOffNormalAlarmType, the
    # same base already used by the pre-existing SecurityFaultConditionType.
    require(r'MODEL_CONDITION_TYPE_CHANNEL_ALARM\s+"Types\.Conditions\.ChannelAlarmConditionType"',
            events, "missing ChannelAlarmConditionType NodeId constant")
    require(r"model_add_condition_type\(server,\s*"
            r"MODEL_CONDITION_TYPE_CHANNEL_ALARM,\s*"
            r'"ChannelAlarmConditionType",\s*'
            r"UA_NODEID_NUMERIC\(0,\s*UA_NS0ID_SYSTEMOFFNORMALALARMTYPE\)\)",
            events, "ChannelAlarmConditionType must be a SystemOffNormalAlarmType subtype")

    # NodeId grammar: derived from model_build_channel_node_id() with the
    # "AlarmCondition" field for the instance and no field (the channel
    # object itself) for the ConditionSource -- can never collide with an
    # existing channel field name because every such name is a distinct
    # fixed string, not a wildcard.
    require(r'MODEL_CHANNEL_ALARM_CONDITION_FIELD_NAME\s+"AlarmCondition"',
            events, "missing AlarmCondition field-name constant")
    require(r"model_build_channel_node_id\(channel,\s*MODEL_CHANNEL_ALARM_CONDITION_FIELD_NAME,\s*"
            r"condition_node_id_buf,\s*sizeof\(condition_node_id_buf\)\)",
            events, "condition NodeId must be derived via model_build_channel_node_id")
    require(r"model_build_channel_node_id\(channel,\s*NULL,\s*source_node_id_buf,\s*"
            r"sizeof\(source_node_id_buf\)\)",
            events, "ConditionSource NodeId must be the channel object itself")

    # One instance per alarm-eligible channel, using the shared eligibility
    # rule owned by alarm_service (not duplicated in the OPC UA layer).
    require(r"UA_StatusCode model_build_channel_alarm_conditions\(UA_Server \*server\)", events,
            "missing per-channel condition builder")
    require(r"channel_manager_count\(\)", events,
            "condition builder must enumerate the live channel registry")
    require(r"alarm_service_value_type_is_eligible\(snapshot\.channel\.metadata\.value_type\)",
            events, "condition builder must reuse alarm_service's eligibility rule")

    # The OPC UA layer only reflects alarm_service state; it must not
    # evaluate thresholds itself (no direct threshold comparisons against
    # runtime.value in the condition machinery).
    forbid(r"model_process_channel_alarm_conditions[\s\S]*?warning_threshold\s*[<>=]",
           events, "OPC UA layer must not evaluate alarm thresholds itself")
    require(r"void model_process_channel_alarm_conditions\(UA_Server \*server\)", events,
            "missing per-channel condition poller")
    require(r"alarm_service_get_condition_by_index\(index,\s*&condition\)", events,
            "poller must read already-evaluated alarm_service condition state")
    require(r"model_sync_condition\(server,\s*"
            r"condition_node_id,\s*"
            r"source_node_id,",
            events, "poller must reflect state via the shared model_sync_condition helper")

    # Acknowledgement: standard per-condition Method wired back to
    # alarm_service, independent per channel; service-side acknowledgement
    # (including AcknowledgeAllAlarms) reconciled back onto AckedState.
    require(r"UA_Server_setConditionTwoStateVariableCallback\(server,\s*"
            r"created_id,\s*"
            r"source_id,\s*"
            r"false,\s*"
            r"model_channel_alarm_acked_callback,\s*"
            r"UA_ENTERING_ACKEDSTATE\)",
            events, "missing standard per-condition Acknowledge sync-back registration")
    require(r"static UA_StatusCode model_channel_alarm_acked_callback\([\s\S]*?"
            r"alarm_service_acknowledge_channel\(slot->channel_id\)",
            events, "Acknowledge callback must call alarm_service_acknowledge_channel per channel")
    require(r"condition\.acknowledged != slot->last_acknowledged_seen[\s\S]*?"
            r'model_condition_set_two_state_id\(server, condition_node_id, "AckedState"',
            events, "poller must reconcile AckedState from service-side acknowledgement")

    # The v0.4.2 public Method contract is untouched: AcknowledgeAllAlarms
    # still just calls alarm_service_acknowledge_all() (now generalized
    # internally to every condition, with no OPC UA-side change required).
    require(r"UA_StatusCode model_acknowledge_alarms\([\s\S]*?"
            r"alarm_service_acknowledge_all\(\);[\s\S]*?"
            r"return UA_STATUSCODE_GOOD;\n\}",
            configuration, "AcknowledgeAllAlarms contract changed")

    # No Flash-journal persistence for alarms in this release.
    forbid(r"model_process_channel_alarm_conditions[\s\S]*?flash", events,
           "alarm condition processing must not touch Flash")
    forbid(r"model_build_channel_alarm_conditions[\s\S]*?flash", events,
           "alarm condition build must not touch Flash")

    # Declarations exist for cross-TU use from model_maintenance.c.
    require(r"UA_StatusCode model_build_channel_alarm_conditions\(UA_Server \*server\);", internal_h,
            "missing model_build_channel_alarm_conditions declaration")
    require(r"void model_process_channel_alarm_conditions\(UA_Server \*server\);", internal_h,
            "missing model_process_channel_alarm_conditions declaration")

    # The polling generalization actually wires the new functions in.
    require(r"model_build_channel_alarm_conditions\(server\)", maintenance,
            "model_process_runtime_events must build channel alarm conditions")
    require(r"model_process_device_health_conditions\(server, state\);\s*"
            r"model_process_channel_alarm_conditions\(server\);",
            maintenance, "model_process_alarm_conditions must poll channel alarm conditions")

    print("channel alarm condition contract: PASS")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print(f"channel alarm condition contract: FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
    except FileNotFoundError as error:
        print(f"channel alarm condition contract: FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
