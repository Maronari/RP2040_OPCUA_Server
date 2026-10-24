#!/usr/bin/env python3
"""Fail-closed source and public-contract checks for the v0.5.1 driver SDK,
superseding test_v050_opcua_adapter_sources.py.

Carries forward every v0.5.0-era invariant check unchanged (T13 preserved
all of them -- confirmed by running the v0.5.0 file against this tree
before retiring it) and updates only the public-contract baseline
comparison at the end to the reviewed v0.5.1 delta (the
model_driver_resource_type_to_string() fix -- six new JSON wire-name
strings for the resource types v0.5.1's Phase 1 drivers added to
services/driver_catalog.h; no NodeId/BrowseName/method was added, removed
or renamed -- see verify_opcua_public_contract.py).
"""

from __future__ import annotations

import argparse
import pathlib
import subprocess
import sys


ROOT = pathlib.Path(__file__).resolve().parents[2]
MODEL_DIR = ROOT / "opcua_model" / "src"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"v0.5.1-opcua-source: FAIL: {message}")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--skip-public-contract", action="store_true")
    args = parser.parse_args(argv)
    sources = "\n".join(path.read_text(encoding="utf-8") for path in MODEL_DIR.glob("*.[ch]"))
    require("channel_manager_copy_snapshot_by_id(" in sources,
            "adapter does not read coherent channel snapshots by stable id")
    require("channel_manager_copy_snapshot_by_index(" in sources,
            "adapter does not use bounded registry discovery")
    require("channel_manager_write_value(" in sources,
            "adapter does not route writes through the service API")
    require("driver_context" not in (MODEL_DIR / "model_address_space.c").read_text(encoding="utf-8"),
            "channel builder inspects opaque driver context")
    require("->write(" not in sources,
            "OPC UA adapter invokes a driver write directly")
    require("MODEL_CHANNEL_NODE_ID_MAX_LEN" in sources and "PLATFORM_CHANNEL_LIMIT" in sources,
            "channel model is not explicitly bounded")
    require("HISTORY_SERVICE_MAX_RETURN_COUNT" in sources,
            "HistoryRead is not bounded by a documented max-return-count cap")
    require("continuationPoint" in sources,
            "HistoryRead no longer implements continuation points")
    history = (MODEL_DIR / "model_history.c").read_text(encoding="utf-8")
    internal = (MODEL_DIR / "opcua_model_internal.h").read_text(encoding="utf-8")
    access = (MODEL_DIR / "model_access.c").read_text(encoding="utf-8")
    configuration = (MODEL_DIR / "model_configuration.c").read_text(encoding="utf-8")
    security = (MODEL_DIR / "model_security.c").read_text(encoding="utf-8")
    maintenance = (MODEL_DIR / "model_maintenance.c").read_text(encoding="utf-8")
    events = (MODEL_DIR / "model_events.c").read_text(encoding="utf-8")
    diagnostics = (MODEL_DIR / "model_diagnostics.c").read_text(encoding="utf-8")
    lifecycle = (MODEL_DIR / "model_core_lifecycle.c").read_text(encoding="utf-8")
    channels = (MODEL_DIR / "model_channels.c").read_text(encoding="utf-8")
    open62541 = (ROOT / "port" / "open62541" / "open62541.c").read_text(encoding="utf-8")
    require("MODEL_HISTORY_CONTINUATION_CAPACITY 16u" in internal,
            "HistoryRead continuation table does not have the frozen capacity")
    require("UA_STATUSCODE_BADNOCONTINUATIONPOINTS" in history,
            "HistoryRead exhaustion is not reported explicitly")
    require("model_history_release_session_points(session_context)" in access,
            "HistoryRead points are not released on session close")
    require("HISTORY_SERVICE_RESTORE_STATUS_READY" in history,
            "HistoryRead does not fail closed when persisted history restore is unavailable")
    require("g_model_maintenance_export_payload" in configuration and
            "char payload[384]" not in configuration,
            "GetConfigState still uses its undersized stack buffer")
    require("MODEL_MAINTENANCE_EXPORT_PAYLOAD_CAPACITY" in security and
            "char payload[4096]" not in security,
            "ExportConfig still uses its undersized stack buffer")
    require(configuration.count("model_set_maintenance_export_method_output") == 1 and
            "model_set_maintenance_export_method_output" in security and
            "model_set_maintenance_export_method_output" in maintenance and
            "UA_VARIANT_DATA_NODELETE" in sources,
            "large PSRAM method responses do not use the bounded persistent view")
    require("model_format_channel_config_json" in configuration and
            "UA_Variant_setScalarCopy" in configuration,
            "small stack-backed channel config response is not copied")
    require("#define OPCUA_SEND_BUFFER_SIZE 16384u" in internal,
            "OPC UA send buffer cannot carry the bounded 16-channel export in one chunk")
    require("#define OPCUA_MAX_METHODS_PER_CALL 1u" in internal and
            "config->maxNodesPerMethodCall = OPCUA_MAX_METHODS_PER_CALL" in lifecycle,
            "batched method calls can multiply large owned response allocations")
    require("request->methodsToCallSize > server->config.maxNodesPerMethodCall" in open62541 and
            "UA_STATUSCODE_BADTOOMANYOPERATIONS" in open62541,
            "configured method-call limit is not enforced fail closed")
    require("condition_status == UA_STATUSCODE_GOOD" in maintenance and
            "g_model_channel_alarm_conditions_built = true" in maintenance,
            "condition builder is marked complete before successful build")
    require("return status;" in events and "UA_Server_deleteCondition" in events,
            "partial channel condition creation is not rolled back and retried")
    require("alarm_service_get_condition_by_channel_id(" in sources,
            "adapter does not resolve alarm state through per-channel conditions")

    # v0.5.0 T8: config-preset apply/list methods and the Device.Compatibility
    # subtree (firmware profile, hardware revision, configuration-schema
    # version, per-driver SupportedDrivers descriptors).
    require("config_presets_get_by_name(preset_name)" in configuration and
            "config_presets_apply(preset->preset_id)" in configuration,
            "ApplyConfigPreset does not resolve and apply through config_presets")
    require('"Device.Maintenance.ApplyConfigPreset"' in internal,
            "ApplyConfigPreset NodeId is missing")
    require('"Device.Maintenance.ListConfigPresets"' in internal,
            "ListConfigPresets NodeId is missing")
    require("driver_catalog_count()" in diagnostics and "driver_catalog_get_by_index(index)" in diagnostics,
            "Device.Compatibility.SupportedDrivers is not derived from the compiled driver catalog")
    require("CONFIG_STORE_SCHEMA_VERSION_CURRENT" in diagnostics,
            "Device.Compatibility.ConfigurationSchemaVersion does not source from config_wire.h")
    require("MODEL_FIRMWARE_DRIVER_PACK_NAME" in diagnostics,
            "Device.Compatibility.FirmwareProfile does not expose the compile-time driver-pack identity")
    require("driver_catalog_get_resource_claims(" in configuration,
            "SupportedDrivers descriptor does not reuse driver_catalog's resource-claim API")

    # A controlled restart is driven only by a mismatch between the address
    # space snapshot and the service-owned RUNNING configuration. A merely
    # staged candidate must never tear down an otherwise valid live server.
    helper_start = channels.index("bool model_address_space_snapshot_requires_server_restart(void)")
    helper_end = channels.index("bool model_is_reboot_required(void)", helper_start)
    restart_helper = channels[helper_start:helper_end]
    reboot_required = channels[helper_end:]
    require("const device_profile_config_t *running = config_service_get();" in restart_helper and
            "config_service_get_staged()" not in restart_helper and
            "model_address_space_snapshot_requires_server_restart()" in reboot_required and
            "config_service_get_staged()" in reboot_required,
            "address-space restart comparison is not RUNNING-only")
    require("model_address_space_snapshot_requires_server_restart()" in lifecycle and
            "g_pending_actions.restart_pending = true;" in lifecycle,
            "restart callback does not arm the deferred restart from the live snapshot")
    run_iterate_index = lifecycle.index("UA_Server_run_iterate(server, false)")
    fail_safe_index = lifecycle.rfind("model_address_space_snapshot_requires_server_restart()",
                                      0,
                                      run_iterate_index)
    require(fail_safe_index >= 0,
            "run loop lacks a snapshot-vs-RUNNING fail-safe before UA_Server_run_iterate")

    # v0.5.1 T13: every services/driver_catalog.h resource type must map to a
    # real JSON wire name in SupportedDrivers, not silently fall back to
    # "none" (see static/tools/test_driver_catalog_opcua_contract.py for the
    # dedicated, self-updating check of this specific mapping).
    require("case DRIVER_CATALOG_RESOURCE_SPI_CHIP_SELECT:" in configuration and
            "case DRIVER_CATALOG_RESOURCE_UIC_CHANNEL:" in configuration,
            "model_driver_resource_type_to_string() has fallen behind driver_catalog_resource_type_t again")

    if not args.skip_public_contract:
        result = subprocess.run(
            [sys.executable, "static/tools/verify_opcua_public_contract.py",
             "--baseline", "c6da6c44326e6b1fe67b06656ac8a3518c12ea9e", "--allow-v051-additions"],
            cwd=ROOT,
            check=False,
        )
        require(result.returncode == 0, "v0.5.0 public-contract comparison failed")
    print("v0.5.1-opcua-source: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
