#!/usr/bin/env python3
"""Guard open62541-normalized custom Event field keys."""

from __future__ import annotations

import pathlib
import re
import sys


MODEL_SOURCE_SET = (
    "opcua_model/src/model_events.c",
    "opcua_model/src/model_maintenance.c",
    "opcua_model/src/model_configuration.c",
)


def require(pattern: str, source: str, description: str) -> None:
    if re.search(pattern, source, re.DOTALL) is None:
        raise AssertionError(description)


def function_body(name: str, source: str) -> str:
    match = re.search(rf"(?:static )?UA_StatusCode {name}\(.*?^}}\n",
                      source, re.DOTALL | re.MULTILINE)
    if match is None:
        raise AssertionError(f"missing {name}")
    return match.group(0)


def main() -> int:
    root = pathlib.Path(__file__).resolve().parents[2]
    model = "\n".join((root / path).read_text(encoding="utf-8")
                        for path in MODEL_SOURCE_SET)
    open62541 = (root / "port" / "open62541" / "open62541.c").read_text(encoding="utf-8")

    require(r"res \|= UA_String_append\(&tmp, UA_STRING\(\"/\"\)\);",
            open62541, "SimpleAttributeOperand paths must begin with a slash")
    require(r"if\(qn->namespaceIndex > 0\).*?"
            r"UA_String_append\(&tmp, UA_STRING\(\":\"\)\);",
            open62541, "SimpleAttributeOperand paths must include a nonzero BrowseName namespace")
    require(r"found = UA_KeyValueMap_get\(ed->eventFields, pathString\);",
            open62541, "event field lookup must use the printed operand path")

    key_builder = function_body("model_make_event_field_key", model)
    require(r"MODEL_EVENT_FIELD_PATH_CAPACITY", key_builder,
            "normalized key builder must keep a fixed buffer bound")
    require(r"snprintf\(field_key, MODEL_EVENT_FIELD_PATH_CAPACITY, \"/1:%s\",",
            key_builder, "normalized key builder must encode the ns=1 event BrowseName path")
    require(r"BADENCODINGLIMITSEXCEEDED", key_builder,
            "normalized key builder must reject an overlong path")

    for name in ("model_set_event_string_field", "model_set_event_bool_field",
                 "model_set_event_uint32_field", "model_set_event_float_field"):
        body = function_body(name, model)
        require(r"model_make_event_field_key\(browse_name, field_key\)", body,
                f"{name} must use the normalized event-field key")
        require(r"UA_QUALIFIEDNAME\(0, field_key\)", body,
                f"{name} must use the normalized namespace-zero KVM key")

    reset = function_body("model_reset_maintenance_journal", model)
    require(r"model_audit_session_event\(session_context, \"maintenance-reset\",\s*"
            r"\"ResetMaintenanceJournal\", true,", reset,
            "successful reset must emit its one reset audit")
    if "model_audit_maintenance_accept(session_context, \"ResetMaintenanceJournal\")" in reset:
        raise AssertionError("successful reset must not enqueue a duplicate maintenance-call audit")

    reboot = function_body("model_request_reboot", model)
    require(r'model_audit_session_event\(session_context, "reboot", "Reboot", true,',
            reboot, "successful Reboot audit must use the public Method name as Subject")
    require(r"model_schedule_reboot\(1500u\)", reboot,
            "Reboot must retain a bounded audit-delivery grace period")

    print("OPC UA event-field key and reset audit contract: PASS")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print(f"OPC UA event-field key and reset audit contract: FAIL: {error}",
              file=sys.stderr)
        raise SystemExit(1)
