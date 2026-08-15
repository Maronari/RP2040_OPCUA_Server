#!/usr/bin/env python3
"""Run the repository's deterministic host and static contract suites.

``--inject-failure`` is intentionally not part of normal coverage. It runs
before the normal list so CI can prove that this runner propagates a failed
suite without running any later suite.
"""

from __future__ import annotations

import argparse
import pathlib
import subprocess
import sys


SUITES = (
    "test_channel_manager.py",
    "test_channel_runtime.py",
    "test_history_service.py",
    "test_sensor_manager_rollback.py",
    "test_config_transaction.py",
    "test_config_compat.py",
    "test_config_service_identity.py",
    "test_config_validate.py",
    "test_v042_opcua_adapter_sources.py",
    "test_channel_model_contract.py",
    "test_verify_channel_model.py",
    "test_verify_channel_model_e2e.py",
    "test_opcua_logging_source.py",
    "test_temperature_adc_source.py",
    "test_export_nodeset.py",
    "test_opcua_network_parse.py",
    "test_flash_prerequisites.py",
    "test_maintenance_condition_active_state_contract.py",
    "test_maintenance_journal_backend.py",
    "test_maintenance_journal_codec.py",
    "test_maintenance_service_atomic.py",
    "test_opcua_event_field_key_contract.py",
    "test_ota_state.py",
    "test_rp2_ua_datetime_clock.py",
    "test_w5x00_transport.py",
    "test_open62541_lwip_listener.py",
    "test_bootloader_handoff.py",
    "test_stage_ota_resume.py",
)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--inject-failure", action="store_true",
                        help="Run a deliberate failing sentinel before the normal suite list")
    args = parser.parse_args()
    tool_dir = pathlib.Path(__file__).resolve().parent

    print("host suites:")
    if args.inject_failure:
        print("[RUN ] injected-failure", flush=True)
        result = subprocess.run(
            [sys.executable, "-c", "raise SystemExit(1)"], check=False
        )
        if result.returncode != 0:
            print(
                f"[FAIL] injected-failure: child exit {result.returncode}",
                file=sys.stderr,
            )
            return result.returncode if result.returncode > 0 else 1
        print("[FAIL] injected-failure unexpectedly passed", file=sys.stderr)
        return 3

    for name in SUITES:
        print(f"  {name}")
    for name in SUITES:
        path = tool_dir / name
        if not path.is_file():
            print(f"[FAIL] missing suite: {path}", file=sys.stderr)
            return 2
        print(f"[RUN ] {name}", flush=True)
        result = subprocess.run([sys.executable, str(path)], check=False)
        if result.returncode != 0:
            print(f"[FAIL] {name}: exit {result.returncode}", file=sys.stderr)
            return result.returncode if result.returncode > 0 else 1
        print(f"[PASS] {name}")

    print(f"host test runner: PASS ({len(SUITES)} suites)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
