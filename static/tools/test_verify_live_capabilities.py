#!/usr/bin/env python3
"""Host suite for verify_live_capabilities.py's comparison logic (no device, no toolchain).

Also guards the drift the live check depends on: every catalog entry's
capability bits in services/src/driver_catalog.c must match its entry in
static/host_descriptors/driver_descriptors.json -- the mismatch that let
AD5422 keep advertising selfTest in the descriptor after the firmware
dropped it would fail here.
"""

from __future__ import annotations

import copy
import importlib.util
import json
import pathlib
import re
import sys
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[2]
TOOLS = ROOT / "static" / "tools"

CAPABILITY_BITS = {
    "START": "start", "STOP": "stop", "READ": "read", "WRITE": "write",
    "CONFIGURE": "configure", "SELF_TEST": "self_test",
}


def load():
    spec = importlib.util.spec_from_file_location("verify_live_capabilities", TOOLS / "verify_live_capabilities.py")
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    sys.modules["verify_live_capabilities"] = module
    spec.loader.exec_module(module)
    return module


def catalog_capabilities(source: str) -> dict[str, list[str]]:
    start = source.index("g_driver_catalog[")
    body = source[start:source.index("};", start)]
    result = {}
    for block in re.findall(r"\{(.*?)\n    \}", body, flags=re.S):
        name = re.search(r'\.name\s*=\s*"([^"]+)"', block)
        caps = re.search(r"\.capabilities\s*=\s*(.*?),\s*\n\s*\.", block, flags=re.S)
        if name and caps:
            result[name.group(1)] = sorted(CAPABILITY_BITS[bit] for bit in
                                           re.findall(r"TELEMETRY_DRIVER_CAPABILITY_(\w+)", caps.group(1)))
    return result


def main() -> int:
    module = load()
    catalog_source = module.CATALOG_SOURCE.read_text(encoding="utf-8")
    descriptors = json.loads(module.DESCRIPTORS.read_text(encoding="utf-8"))

    # Every compiled catalog entry is guarded by a flag, and matches its descriptor.
    flags = module.catalog_flags(catalog_source)
    assert set(flags) == {"internal_temp", "adc_voltage", "gpio_input", "gpio_output", "ads8688", "ad5422",
                          "max22190", "ips2050", "uic", "synthetic"}, flags
    catalog_caps = catalog_capabilities(catalog_source)
    assert set(catalog_caps) == set(flags), catalog_caps
    by_name = {entry["name"]: entry for entry in descriptors["drivers"]}
    for name, caps in catalog_caps.items():
        described = sorted(module.CAPABILITY_ALIASES.get(c, c) for c in by_name[name]["capabilities"])
        assert described == caps, f"{name}: descriptor capabilities {described} != catalog {caps}"

    with tempfile.TemporaryDirectory() as tmp:
        build = pathlib.Path(tmp)
        cache = ["FIRMWARE_DRIVER_PACK:STRING=base"]
        for name, flag in flags.items():
            on = name in {"internal_temp", "adc_voltage", "gpio_input", "gpio_output", "synthetic"}
            cache.append(f"{flag}:BOOL={'ON' if on else 'OFF'}")
        (build / "CMakeCache.txt").write_text("\n".join(cache) + "\n", encoding="utf-8")
        config_wire = module.CONFIG_WIRE_HEADER.read_text(encoding="utf-8")
        schema = module.config_schema_version(config_wire)
        export = {"schemaVersion": schema, "device": {
            "hwRevision": "waveshare_core2350b_16mb_psram8mb/W5500", "fwRevision": "0.5.1"}}
        (build / "config_export.json").write_text(json.dumps(export), encoding="utf-8")
        expected = module.expected_from_build(build, catalog_source, descriptors, config_wire)

        # A build artifact that drifted from the firmware constant is refused.
        export["schemaVersion"] = schema + 1
        (build / "config_export.json").write_text(json.dumps(export), encoding="utf-8")
        try:
            module.expected_from_build(build, catalog_source, descriptors, config_wire)
        except ValueError as error:
            assert "schemaVersion" in str(error)
        else:
            raise AssertionError("a drifted config_export.json schemaVersion must be refused")

    assert expected["firmwareProfile"] == "base"
    assert expected["configurationSchemaVersion"] == schema
    assert sorted(expected["drivers"]) == ["adc_voltage", "gpio_input", "gpio_output", "internal_temp", "synthetic"]
    assert expected["drivers"]["gpio_output"]["capabilities"] == ["configure", "self_test", "start", "stop", "write"]

    live = {
        "firmwareProfile": "base",
        "hardwareRevision": "waveshare_core2350b_16mb_psram8mb/W5500",
        "softwareRevision": "0.5.1",
        "configurationSchemaVersion": schema,
        "drivers": {
            name: {"id": spec["id"], "name": name, "capabilities": list(reversed(spec["capabilities"])),
                   "resources": [{"type": "gpio", "index": 0, "shared": False}]}
            for name, spec in expected["drivers"].items()
        },
    }
    assert module.compare(expected, live) == []

    def problems_after(mutate) -> list[str]:
        changed = copy.deepcopy(live)
        mutate(changed)
        return module.compare(expected, changed)

    assert any("firmwareProfile" in p for p in problems_after(lambda d: d.update(firmwareProfile="industrial")))
    assert any("softwareRevision" in p for p in problems_after(lambda d: d.update(softwareRevision="0.5.0")))
    assert any("configurationSchemaVersion" in p for p in problems_after(lambda d: d.update(configurationSchemaVersion=None)))
    # The reviewed gap: any integer used to pass.
    assert any("configurationSchemaVersion" in p for p in problems_after(lambda d: d.update(configurationSchemaVersion=999)))
    assert any("configurationSchemaVersion" in p for p in problems_after(lambda d: d.update(configurationSchemaVersion=True)))
    assert any("missing compiled drivers" in p for p in problems_after(lambda d: d["drivers"].pop("synthetic")))
    assert any("does not compile" in p for p in problems_after(
        lambda d: d["drivers"].__setitem__("ads8688", {"id": 5, "name": "ads8688", "capabilities": []})))
    assert any("capabilities" in p for p in problems_after(
        lambda d: d["drivers"]["gpio_output"]["capabilities"].remove("self_test")))
    assert any("id" in p for p in problems_after(lambda d: d["drivers"]["gpio_input"].update(id=99)))
    assert any("not valid JSON" in p for p in problems_after(lambda d: d["drivers"].__setitem__("synthetic", "{broken")))
    assert any("unknown resource claim" in p for p in problems_after(
        lambda d: d["drivers"]["internal_temp"]["resources"].append({"type": "i2c_bus"})))

    literals = module.source_node_literals(module.MODEL_SOURCES)
    assert "Device.Compatibility.SupportedDrivers" in literals and "Device.Compatibility.FirmwareProfile" in literals
    # The first live run reported both of these: a runtime prefix
    # (model_build_channel_node_id()) and a doc-comment example.
    assert "Device.Channels.c_" not in literals
    assert "Device.Channels.c_74656D7031.AlarmCondition" not in literals
    stripped = module.strip_c_comments('/* "Device.A" */ x = "Device.B"; // "Device.C"\n y = "a//b";')
    assert '"Device.B"' in stripped and '"a//b"' in stripped
    assert "Device.A" not in stripped and "Device.C" not in stripped

    print("verify-live-capabilities: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
