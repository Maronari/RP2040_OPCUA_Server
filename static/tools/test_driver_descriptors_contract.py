#!/usr/bin/env python3
"""Fail-closed check that static/host_descriptors/driver_descriptors.json
(the versioned host-side sensor/module descriptor format, ROADMAP.md:555,
v0.5.1 T15) stays in sync with services/driver_catalog.h's
config_channel_driver_id_t enum -- every compiled driver_id must have a
descriptor entry, and every descriptor entry must name a real driver_id
(neither list may drift ahead of the other), following the same
source-scanning regression-guard convention as
test_driver_catalog_opcua_contract.py (T13).
"""

from __future__ import annotations

import json
import pathlib
import re
import sys


ROOT = pathlib.Path(__file__).resolve().parents[2]
DRIVER_CATALOG_HEADER = ROOT / "services" / "include" / "services" / "driver_catalog.h"
DESCRIPTORS_JSON = ROOT / "static" / "host_descriptors" / "driver_descriptors.json"

REQUIRED_DRIVER_FIELDS = ("driverId", "name", "wiring", "connectionConstraints", "capabilities")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"driver-descriptors-contract: FAIL: {message}")


def main() -> int:
    header_text = DRIVER_CATALOG_HEADER.read_text(encoding="utf-8")
    enum_match = re.search(
        r"typedef enum\s*\{(.*?)\}\s*config_channel_driver_id_t;",
        header_text,
        re.DOTALL,
    )
    require(enum_match is not None, "could not locate config_channel_driver_id_t enum body")
    enum_body = enum_match.group(1)
    id_matches = re.findall(r"CONFIG_CHANNEL_DRIVER_(\w+)\s*=\s*(\d+)", enum_body)
    driver_ids_by_name = {name: int(value) for name, value in id_matches}
    require("NONE" in driver_ids_by_name and driver_ids_by_name["NONE"] == 0,
            "CONFIG_CHANNEL_DRIVER_NONE is not 0 -- descriptor id extraction assumption broke")
    compiled_ids = sorted(v for k, v in driver_ids_by_name.items() if k != "NONE")
    require(len(compiled_ids) >= 9, f"expected at least 9 non-NONE driver ids, found {len(compiled_ids)}")

    descriptors = json.loads(DESCRIPTORS_JSON.read_text(encoding="utf-8"))
    require("schemaVersion" in descriptors, "driver_descriptors.json has no schemaVersion field")
    require(isinstance(descriptors.get("drivers"), list) and len(descriptors["drivers"]) > 0,
            "driver_descriptors.json has no drivers array")

    descriptor_ids = []
    for entry in descriptors["drivers"]:
        for field in REQUIRED_DRIVER_FIELDS:
            require(field in entry, f"driver_descriptors.json entry {entry.get('name', '?')} missing '{field}'")
        descriptor_ids.append(entry["driverId"])

    require(len(descriptor_ids) == len(set(descriptor_ids)),
            f"driver_descriptors.json has duplicate driverId values: {descriptor_ids}")

    missing_from_descriptors = sorted(set(compiled_ids) - set(descriptor_ids))
    require(not missing_from_descriptors,
            f"driver_catalog.h defines driver_id(s) {missing_from_descriptors} with no entry in "
            "static/host_descriptors/driver_descriptors.json")

    stale_in_descriptors = sorted(set(descriptor_ids) - set(compiled_ids))
    require(not stale_in_descriptors,
            f"driver_descriptors.json describes driver_id(s) {stale_in_descriptors} that no longer "
            "exist in services/driver_catalog.h")

    print(f"driver-descriptors-contract: PASS ({len(descriptor_ids)} driver descriptors in sync)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
