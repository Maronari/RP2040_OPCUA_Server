#!/usr/bin/env python3
"""Focused v0.4.1 source checks for the bounded OPC UA channel adapter."""

from __future__ import annotations

import pathlib
import subprocess
import sys


ROOT = pathlib.Path(__file__).resolve().parents[2]
MODEL_DIR = ROOT / "opcua_model" / "src"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"v0.4.1-opcua-source: FAIL: {message}")


def main() -> int:
    sources = "\n".join(path.read_text(encoding="utf-8") for path in MODEL_DIR.glob("*.[ch]"))
    for legacy_api in (
        "channel_manager_get_by_index(",
        "channel_manager_find_by_id(",
        "channel_manager_find_by_name(",
        "channel_manager_copy_channel(",
    ):
        require(legacy_api not in sources, f"legacy raw channel API remains: {legacy_api}")

    require("channel_manager_copy_snapshot_by_id(" in sources,
            "adapter does not read service-owned channel snapshots by id")
    require("channel_manager_copy_snapshot_by_index(" in sources,
            "adapter does not use bounded channel snapshot iteration")
    require("PLATFORM_CHANNEL_LIMIT" in sources,
            "adapter does not use the single platform channel limit")

    maintenance = (MODEL_DIR / "model_maintenance.c").read_text(encoding="utf-8")
    helper_start = maintenance.index("bool model_source_timestamp_to_datetime(")
    helper_end = maintenance.index("void model_apply_channel_source_timestamp(", helper_start)
    conversion = maintenance[helper_start:helper_end]
    require("TELEMETRY_CHANNEL_SOURCE_TIMESTAMP_INVALID" in conversion,
            "invalid channel source timestamps are not rejected")
    require("UA_DATETIME_UNIX_EPOCH" in conversion and "source_timestamp_unix_us * 10u" in conversion,
            "Unix microseconds are not converted losslessly to OPC UA 100 ns ticks")
    require("UA_DateTime_now" not in conversion,
            "valid channel source timestamps fall back to model time")

    history = (MODEL_DIR / "model_history.c").read_text(encoding="utf-8")
    require("model_source_timestamp_to_datetime(sample->source_timestamp_unix_us, &timestamp)" in history,
            "HistoryRead does not preserve microsecond source timestamps through the shared adapter")
    require("history_service_channel_sample_t sample_buffer[HISTORY_SERVICE_CHANNEL_CAPACITY]" not in history,
            "HistoryRead still allocates its full sample buffer on the server-task stack")
    require("history_service_channel_sample_t *const sample_buffer = g_model_history_read_sample_buffer" in history,
            "HistoryRead does not use the model-owned PSRAM sample scratch buffer")

    lifecycle = (MODEL_DIR / "model_core_lifecycle.c").read_text(encoding="utf-8")
    require("g_model_history_read_sample_buffer = (history_service_channel_sample_t *)psram_malloc(" in lifecycle,
            "model initialization does not allocate the bounded HistoryRead scratch buffer in PSRAM")
    require("HISTORY_SERVICE_CHANNEL_CAPACITY * sizeof(*g_model_history_read_sample_buffer)" in lifecycle,
            "HistoryRead scratch allocation is not bounded by HISTORY_SERVICE_CHANNEL_CAPACITY")
    require("psram_free(g_model_history_read_sample_buffer);" in lifecycle and
            "g_model_history_read_sample_buffer = NULL;" in lifecycle,
            "model shutdown does not release the HistoryRead PSRAM scratch buffer")

    result = subprocess.run(
        [sys.executable, "static/tools/verify_opcua_public_contract.py", "--baseline", "v0.4.0"],
        cwd=ROOT,
        check=False,
    )
    require(result.returncode == 0, "v0.4.0 public-contract gate failed")
    print("v0.4.1-opcua-source: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
