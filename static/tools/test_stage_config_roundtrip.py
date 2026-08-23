#!/usr/bin/env python3
"""Exercise the 16-slot ExportConfig JSON shape through stage_config's canonicalizer."""

from __future__ import annotations

import importlib.util
import json
import pathlib
import sys


ROOT = pathlib.Path(__file__).resolve().parents[2]
STAGE_CONFIG = ROOT / "static" / "tools" / "stage_config.py"


def load_stage_config():
    spec = importlib.util.spec_from_file_location("stage_config", STAGE_CONFIG)
    if spec is None or spec.loader is None:
        raise RuntimeError("could not load stage_config.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def channel(slot: int) -> dict:
    return {
        "slot": slot, "inUse": slot < 3, "kind": 1, "valueType": 1,
        "driverId": 1, "sourceIndex": slot, "sourceFlags": 0,
        "id": f"channel_{slot}", "name": f"Channel {slot}",
        "description": "export round-trip", "unit": "C", "flags": 1,
        "minimum": -100.0, "maximum": 250.0, "hasMinimum": True,
        "hasMaximum": True, "enabled": True, "samplePeriodMs": 1000,
        "publishPeriodMs": 1000, "historyIntervalMs": 1000, "scale": 1.0,
        "offset": 0.0, "warningThreshold": 80.0, "criticalThreshold": 90.0,
        "lowThreshold": -10.0 - slot, "lowLowThreshold": -20.0 - slot,
        "alarmHysteresis": 0.25 + slot, "alarmDebounceMs": 50 + slot,
    }


def export_shaped_document() -> dict:
    return {
        "schemaVersion": 1,
        "device": {
            "manufacturer": "RP2", "model": "Reference", "deviceName": "node",
            "opcuaHostname": "node", "serialNumber": "test", "productInstanceUri": "urn:test",
            "assetId": "asset", "hwRevision": "A", "fwRevision": "0.4.3",
            "buildTime": "now", "gitHash": "deadbeef", "location": "lab",
            "assetClass": "test", "manufactureDateUtc": 0, "calibrationDateUtc": 0,
            "hierarchicalLocationPath": "lab", "operationalLocationPath": "line",
        },
        "network": {"mode": "dhcp", "mac": "00:00:00:00:00:00", "staticIp": "0.0.0.0",
                    "staticNetmask": "0.0.0.0", "staticGateway": "0.0.0.0", "staticDns": "0.0.0.0"},
        "time": {"ntpServer": "pool.ntp.org", "timezoneOffsetMinutes": 0},
        "mqtt": {"enabled": False, "useJson": True, "brokerUrl": "", "topicRoot": ""},
        "diagnostics": {"enableHeapDiagnostics": False, "enableTaskDiagnostics": False, "watchdogTimeoutMs": 1000},
        "opcua": {"port": 4840, "enableAlarmAckMethod": True},
        "channels": [channel(slot) for slot in range(16)],
    }


def main() -> int:
    stage_config = load_stage_config()
    exported = export_shaped_document()
    canonical = stage_config.canonicalize_config_document(exported)
    reparsed = json.loads(stage_config.serialize_canonical_document(canonical))
    validated = stage_config.canonicalize_config_document(reparsed)

    assert len(validated["channels"]) == 16
    for slot, item in enumerate(validated["channels"]):
        assert item["slot"] == slot
        assert item["lowThreshold"] == -10.0 - slot
        assert item["lowLowThreshold"] == -20.0 - slot
        assert item["alarmHysteresis"] == 0.25 + slot
        assert item["alarmDebounceMs"] == 50 + slot

    print("stage-config-roundtrip: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
