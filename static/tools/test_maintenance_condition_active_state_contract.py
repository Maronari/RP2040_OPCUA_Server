#!/usr/bin/env python3
"""Guard service-owned MaintenanceRequired lifecycle fields."""

from __future__ import annotations

import pathlib
import re
import sys


def require(pattern: str, source: str, description: str) -> None:
    if re.search(pattern, source, re.DOTALL) is None:
        raise AssertionError(description)


def main() -> int:
    root = pathlib.Path(__file__).resolve().parents[2]
    source = (root / "opcua_model" / "src" / "opcua_model.c").read_text(encoding="utf-8")

    require(r"MODEL_MAINTENANCE_VALUE_ACTIVE_STATE", source,
            "missing MaintenanceRequired ActiveState value kind")
    require(r"MODEL_MAINTENANCE_VALUE_RETAIN", source,
            "missing MaintenanceRequired Retain value kind")
    require(r"case MODEL_MAINTENANCE_VALUE_ACTIVE_STATE:\s*"
            r"case MODEL_MAINTENANCE_VALUE_RETAIN:\s*"
            r"active = model_maintenance_condition_is_active\(&snapshot\) \? UA_TRUE : UA_FALSE;\s*"
            r"status = UA_Variant_setScalarCopy\(&value->value, &active,\s*"
            r"&UA_TYPES\[UA_TYPES_BOOLEAN\]\);",
            source,
            "ActiveState and Retain must be Boolean values from the recovered maintenance snapshot")
    require(r"active_state_id_path\[\] = \{\s*"
            r"UA_QUALIFIEDNAME\(0, \(char \*\)\"ActiveState\"\),\s*"
            r"UA_QUALIFIEDNAME\(0, \(char \*\)\"Id\"\)\};",
            source,
            "missing ActiveState/Id browse path")
    require(r"condition_id, 2u, active_state_id_path,\s*"
            r"MODEL_MAINTENANCE_VALUE_ACTIVE_STATE\);",
            source,
            "ActiveState/Id does not install the service-owned value source")
    require(r"retain_path\[\] = \{UA_QUALIFIEDNAME\(0, \(char \*\)\"Retain\"\)\};",
            source,
            "missing Retain browse path")
    require(r"condition_id, 1u, retain_path, MODEL_MAINTENANCE_VALUE_RETAIN\);",
            source,
            "Retain does not install the service-owned value source")

    sync = re.search(r"static UA_StatusCode model_sync_maintenance_condition\(.*?^}\n",
                     source, re.DOTALL | re.MULTILINE)
    if sync is None:
        raise AssertionError("missing maintenance condition synchronizer")
    if '"ActiveState"' in sync.group(0):
        raise AssertionError("recovery synchronizer must not write ActiveState")
    if '"Retain"' in sync.group(0):
        raise AssertionError("recovery synchronizer must not write Retain")
    if "UA_Server_triggerConditionEvent" in sync.group(0):
        raise AssertionError("recovery synchronizer must not trigger an event")

    print("maintenance condition lifecycle service-owned contract: PASS")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print(f"maintenance condition lifecycle service-owned contract: FAIL: {error}",
              file=sys.stderr)
        raise SystemExit(1)
