#!/usr/bin/env python3
"""Pure helper/argument safety checks for verify_v050.py (no network)."""

from __future__ import annotations

import asyncio
import importlib.util
import pathlib
import sys
from types import SimpleNamespace


ROOT = pathlib.Path(__file__).resolve().parents[2]
TOOLS = ROOT / "static" / "tools"
sys.path.insert(0, str(TOOLS))


def load(name: str):
    spec = importlib.util.spec_from_file_location(name, TOOLS / f"{name}.py")
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def main() -> int:
    verifier = load("verify_v050")
    stage = load("stage_config")
    client_cert, client_key = stage.resolve_auto_client_certificate_paths()
    assert client_cert.parent.name == "rp2040_opcua_client"
    assert client_key.parent == client_cert.parent
    baseline = stage.canonicalize_config_document(load("test_stage_config_roundtrip").export_shaped_document())

    assert verifier.parse_endpoint("opc.tcp://192.168.1.102:4840")
    parsed = verifier.parse_args(["opc.tcp://192.168.1.102:4840", "--verify-presets"])
    assert (parsed.verify_presets and parsed.stability_timeout > 0
            and parsed.stability_interval > 0 and parsed.disconnect_timeout > 0)
    for unsafe in ("http://192.168.1.102:4840", "opc.tcp://user:secret@host:4840", "opc.tcp://host"):
        try:
            verifier.parse_endpoint(unsafe)
        except Exception:
            pass
        else:
            raise AssertionError(f"unsafe endpoint accepted: {unsafe}")

    original_hash = verifier.document_hash(baseline)
    no_restart, slot = verifier.make_no_restart_candidate(baseline)
    assert slot == 0 and verifier.document_hash(no_restart) != original_hash
    assert no_restart["channels"][slot]["description"] == baseline["channels"][slot]["description"]
    restart, restart_slot, description = verifier.make_restart_candidate(baseline)
    assert restart_slot == 0 and description != baseline["channels"][restart_slot]["description"]
    assert len(description) == len(baseline["channels"][restart_slot]["description"])
    assert restart["channels"][restart_slot]["alarmHysteresis"] == baseline["channels"][restart_slot]["alarmHysteresis"]
    missing, missing_slot, missing_id = verifier.make_missing_driver_candidate(baseline, {1, 2, 3})
    assert missing_slot == 0 and missing_id == 4 and missing["channels"][missing_slot]["driverId"] == 4
    assert verifier.document_hash(baseline) == original_hash

    catalog = [
        {"id": 1, "name": "sensor_node", "description": "reference"},
        {"id": 2, "name": "remote_io", "description": "remote I/O"},
        {"id": 3, "name": "dev_benchmark", "description": "benchmark"},
        {"id": 99, "name": "future", "description": "must not be applied"},
    ]
    assert verifier.required_preset_names(catalog) == (
        "sensor_node", "remote_io", "dev_benchmark",
    )
    for bad_catalog in ([], [{"name": "sensor_node"}],
                        [{"name": "sensor_node"}, {"name": "sensor_node"}]):
        try:
            verifier.required_preset_names(bad_catalog)
        except verifier.VerificationError:
            pass
        else:
            raise AssertionError(f"invalid preset catalog accepted: {bad_catalog!r}")

    inventory = verifier.active_channel_inventory(baseline)
    assert inventory == {"channelCount": 3, "channelIds": ["channel_0", "channel_1", "channel_2"]}
    stable = verifier.stable_config_state({
        "runningRevision": 7, "stagedRevision": 7, "stagedDiffers": False,
        "importSessionActive": False, "rebootRequired": False,
    })
    assert stable["runningRevision"] == 7
    assert verifier.config_state_snapshot({
        "runningRevision": 7, "stagedRevision": 7, "stagedDiffers": False,
        "importSessionActive": False, "rebootRequired": True, "ignored": "not evidence",
    }) == {
        "runningRevision": 7, "stagedRevision": 7, "stagedDiffers": False,
        "importSessionActive": False, "rebootRequired": True,
    }
    for unstable in (
        {"runningRevision": 7, "stagedRevision": 6, "stagedDiffers": True, "importSessionActive": False},
        {"runningRevision": 7, "stagedRevision": 7, "stagedDiffers": False, "importSessionActive": True},
        {"runningRevision": 7, "stagedRevision": 7, "stagedDiffers": False,
         "importSessionActive": False, "rebootRequired": True},
    ):
        try:
            verifier.stable_config_state(unstable)
        except verifier.VerificationError:
            pass
        else:
            raise AssertionError(f"unstable state accepted: {unstable!r}")

    async def exercise_disconnect_probe() -> None:
        original_get_state = verifier.opcua_common.get_config_state
        calls = 0

        async def disconnecting_get_state(configuration):
            nonlocal calls
            del configuration
            calls += 1
            if calls >= 2:
                raise ConnectionError("fixture transport closed")
            return {
                "runningRevision": 1, "stagedRevision": 1,
                "stagedDiffers": False, "importSessionActive": False,
                "rebootRequired": True,
            }

        async def live_get_state(configuration):
            del configuration
            return {
                "runningRevision": 1, "stagedRevision": 1,
                "stagedDiffers": False, "importSessionActive": False,
                "rebootRequired": True,
            }

        args = SimpleNamespace(disconnect_timeout=0.15)
        evidence = verifier.Evidence("opc.tcp://fixture:4840")
        try:
            verifier.opcua_common.get_config_state = disconnecting_get_state
            await verifier.require_server_disconnect(
                args, object(), evidence, "fixture-disconnect")
            assert any(item["name"] == "fixture-disconnect"
                       for item in evidence.payload["passed"])

            verifier.opcua_common.get_config_state = live_get_state
            try:
                await verifier.require_server_disconnect(
                    args, object(), evidence, "fixture-still-live")
            except verifier.VerificationError:
                pass
            else:
                raise AssertionError("usable old session was accepted as a restart")
        finally:
            verifier.opcua_common.get_config_state = original_get_state

    asyncio.run(exercise_disconnect_probe())

    strings_only = stage.canonicalize_config_document(baseline)
    for channel in strings_only["channels"]:
        channel["inUse"] = False
        channel["valueType"] = 7
    try:
        verifier.select_numeric_channel(strings_only)
    except verifier.VerificationError:
        pass
    else:
        raise AssertionError("no numeric channel was accepted")
    print("verify-v050-helpers: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
