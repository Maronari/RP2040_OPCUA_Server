#!/usr/bin/env python3
"""Fail-closed check that every services/driver_catalog.h resource type has
a corresponding JSON wire-name mapping in opcua_model/src/model_configuration.c.

opcua_model/'s existing convention for this class of invariant is static
source-text scanning (see test_v050_opcua_adapter_sources.py), not a
compiled unit test -- compiling opcua_model/ standalone for host tests
would require the full open62541 stack. This test follows that same
convention for a gap the v0.5.1 T13 driver-catalog/OPC UA review found:
model_driver_resource_type_to_string() silently fell behind
driver_catalog_resource_type_t as v0.5.1 added six new resource types
(SPI_CHIP_SELECT/SPI_PERIPHERAL/AD5422_CHANNEL/MAX22190_CHANNEL/
IPS2050_CHANNEL/UIC_CHANNEL), so every new industrial driver's resource
claims were reported as "none" through the existing
Device.Compatibility.SupportedDrivers OPC UA exposure without anything
catching it. This test exists so the next new resource type cannot drift
the same way.
"""

from __future__ import annotations

import pathlib
import re
import sys


ROOT = pathlib.Path(__file__).resolve().parents[2]
DRIVER_CATALOG_HEADER = ROOT / "services" / "include" / "services" / "driver_catalog.h"
MODEL_CONFIGURATION_SOURCE = ROOT / "opcua_model" / "src" / "model_configuration.c"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"driver-catalog-opcua-contract: FAIL: {message}")


def main() -> int:
    header_text = DRIVER_CATALOG_HEADER.read_text(encoding="utf-8")
    enum_match = re.search(
        r"typedef enum\s*\{(.*?)\}\s*driver_catalog_resource_type_t;",
        header_text,
        re.DOTALL,
    )
    require(enum_match is not None, "could not locate driver_catalog_resource_type_t enum body")
    enum_body = enum_match.group(1)
    resource_types = sorted(set(re.findall(r"\bDRIVER_CATALOG_RESOURCE_[A-Z0-9_]+\b", enum_body)))
    require(len(resource_types) >= 9, f"expected at least 9 resource types, found {len(resource_types)}")

    source_text = MODEL_CONFIGURATION_SOURCE.read_text(encoding="utf-8")
    missing = [
        name for name in resource_types
        if f"case {name}:" not in source_text
    ]
    require(
        not missing,
        "model_driver_resource_type_to_string() (opcua_model/src/model_configuration.c) "
        f"has no case for: {', '.join(missing)} -- every driver_catalog_resource_type_t "
        "value must map to a JSON wire name or SupportedDrivers silently reports \"none\"",
    )

    print(f"driver-catalog-opcua-contract: PASS ({len(resource_types)} resource types mapped)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
