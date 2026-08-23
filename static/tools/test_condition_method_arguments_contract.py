#!/usr/bin/env python3
"""Guard the v0.4.3 Acknowledge/ConditionRefresh BadInternalError fix.

This firmware's compiled-in namespace-zero data leaves the InputArguments
value empty (Null Variant) for every standard Alarms & Conditions Method that
takes parameters (ConditionRefresh, ConditionRefresh2, Acknowledge,
AddComment); open62541's generic Method dispatcher (checkAdjustArguments())
refuses to invoke any of these Methods without a populated Argument[] value
on that node, so every call failed with BadInternalError before reaching this
project's own callback code -- confirmed live on reference hardware. Compiling
and running a full open62541 server is out of scope for the host suite (see
test_channel_alarm_condition_contract.py for the existing precedent), so this
asserts the fix's structural contract by pattern-matching the actual source:
the four affected Method Argument nodes must be populated with the correct
Part 9 argument signatures, once, before any client could call them.
"""

from __future__ import annotations

import pathlib
import re
import sys


ROOT_RELATIVE = (
    "opcua_model/src/model_events.c",
    "opcua_model/src/model_maintenance.c",
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
    internal_h = sources["opcua_model/src/opcua_model_internal.h"]

    # The fix-up helper must exist and be declared for cross-TU use.
    require(r"UA_StatusCode model_fix_condition_method_arguments\(UA_Server \*server\)",
            events, "missing model_fix_condition_method_arguments definition")
    require(r"UA_StatusCode model_fix_condition_method_arguments\(UA_Server \*server\);",
            internal_h, "missing model_fix_condition_method_arguments declaration")

    # It writes through UA_Server_writeValue onto an Argument[] array (not a
    # scalar, not some other DataType) via UA_Variant_setArrayCopy.
    require(r"static UA_StatusCode model_write_method_input_arguments\([\s\S]*?"
            r"UA_Variant_setArrayCopy\(&value,\s*arguments,\s*spec_count,\s*"
            r"&UA_TYPES\[UA_TYPES_ARGUMENT\]\)[\s\S]*?"
            r"UA_Server_writeValue\(server,\s*input_arguments_node,\s*value\)",
            events, "fix-up must write an Argument[] array via UA_Server_writeValue")

    # Every one of the four standard A&C Methods with parameters is covered,
    # each with the exact Part 9 argument names and data types. A wrong
    # dataType would silently make open62541 reject valid client calls again
    # (BadTypeMismatch instead of BadInternalError), so pin both name and type
    # together per argument, not just presence of the NodeId constant.
    require(r'refresh_args\[0\]\.name = "SubscriptionId";\s*'
            r"refresh_args\[0\]\.data_type = UA_NODEID_NUMERIC\(0, UA_NS0ID_UINT32\);",
            events, "ConditionRefresh must describe SubscriptionId as UInt32")
    require(r"model_write_method_input_arguments\(\s*"
            r"server, UA_NODEID_NUMERIC\(0, "
            r"UA_NS0ID_CONDITIONTYPE_CONDITIONREFRESH_INPUTARGUMENTS\),\s*"
            r"refresh_args, 1u\)",
            events, "ConditionRefresh InputArguments node must be written with refresh_args")

    require(r'refresh2_args\[0\]\.name = "SubscriptionId";\s*'
            r"refresh2_args\[0\]\.data_type = UA_NODEID_NUMERIC\(0, UA_NS0ID_UINT32\);\s*"
            r'refresh2_args\[1\]\.name = "MonitoredItemId";\s*'
            r"refresh2_args\[1\]\.data_type = UA_NODEID_NUMERIC\(0, UA_NS0ID_UINT32\);",
            events, "ConditionRefresh2 must describe SubscriptionId and MonitoredItemId as UInt32")
    require(r"model_write_method_input_arguments\(\s*"
            r"server, UA_NODEID_NUMERIC\(0, "
            r"UA_NS0ID_CONDITIONTYPE_CONDITIONREFRESH2_INPUTARGUMENTS\),\s*"
            r"refresh2_args, 2u\)",
            events, "ConditionRefresh2 InputArguments node must be written with refresh2_args")

    require(r'event_id_comment_args\[0\]\.name = "EventId";\s*'
            r"event_id_comment_args\[0\]\.data_type = UA_NODEID_NUMERIC\(0, UA_NS0ID_BYTESTRING\);\s*"
            r'event_id_comment_args\[1\]\.name = "Comment";\s*'
            r"event_id_comment_args\[1\]\.data_type = UA_NODEID_NUMERIC\(0, UA_NS0ID_LOCALIZEDTEXT\);",
            events, "Acknowledge/AddComment must describe EventId as ByteString and Comment as LocalizedText")
    require(r"model_write_method_input_arguments\(\s*"
            r"server, UA_NODEID_NUMERIC\(0, "
            r"UA_NS0ID_ACKNOWLEDGEABLECONDITIONTYPE_ACKNOWLEDGE_INPUTARGUMENTS\),\s*"
            r"event_id_comment_args, 2u\)",
            events, "Acknowledge InputArguments node must be written with event_id_comment_args")
    require(r"model_write_method_input_arguments\(\s*"
            r"server, UA_NODEID_NUMERIC\(0, "
            r"UA_NS0ID_CONDITIONTYPE_ADDCOMMENT_INPUTARGUMENTS\),\s*"
            r"event_id_comment_args, 2u\)",
            events, "AddComment InputArguments node must be written with event_id_comment_args")

    # A partial fix-up (e.g. one node fixed but not another) must not be
    # silently swallowed: each subsequent write is only attempted while the
    # running status is still Good, and the overall function surfaces the
    # first failure instead of unconditionally returning Good.
    require(r"if \(status == UA_STATUSCODE_GOOD\)\s*\{\s*"
            r"status = model_write_method_input_arguments\(\s*"
            r"server, UA_NODEID_NUMERIC\(0, "
            r"UA_NS0ID_CONDITIONTYPE_CONDITIONREFRESH2_INPUTARGUMENTS\)",
            events, "ConditionRefresh2 fix-up must be gated on the prior write succeeding")
    require(r"return status;\s*\}\s*\n\n#endif /\* UA_ENABLE_SUBSCRIPTIONS_ALARMS_CONDITIONS \*/",
            events, "model_fix_condition_method_arguments must surface its own result, not force Good")

    # The whole helper is guarded the same way as the rest of the channel
    # alarm condition machinery, so a build without A&C support still links.
    require(r"#if defined\(UA_ENABLE_SUBSCRIPTIONS_ALARMS_CONDITIONS\)[\s\S]*?"
            r"UA_StatusCode model_fix_condition_method_arguments",
            events, "fix-up must be compiled only under UA_ENABLE_SUBSCRIPTIONS_ALARMS_CONDITIONS")

    # Wired in before any condition (and therefore before any client) can
    # reach these shared namespace-0 Method nodes, and before the enabling
    # condition-source wiring that follows it in model_build_alarm_conditions.
    require(r"model_fix_condition_method_arguments\(server\)[\s\S]*?"
            r"model_enable_condition_source\(server,",
            maintenance, "argument fix-up must run before model_enable_condition_source")
    require(r"model_fix_condition_method_arguments\(server\);\s*"
            r"if \(status != UA_STATUSCODE_GOOD\)\s*\{[\s\S]*?"
            r"g_model_alarm_conditions_available = false;\s*"
            r"return status;\s*\}",
            maintenance, "a failed argument fix-up must disable A&C instead of continuing silently")

    print("condition method arguments contract: PASS")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print(f"condition method arguments contract: FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
    except FileNotFoundError as error:
        print(f"condition method arguments contract: FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
