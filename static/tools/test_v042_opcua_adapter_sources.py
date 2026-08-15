#!/usr/bin/env python3
"""Fail-closed source and public-contract checks for the v0.4.2 channel model."""

from __future__ import annotations

import pathlib
import subprocess
import sys


ROOT = pathlib.Path(__file__).resolve().parents[2]
MODEL_DIR = ROOT / "opcua_model" / "src"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"v0.4.2-opcua-source: FAIL: {message}")


def main() -> int:
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

    result = subprocess.run(
        [sys.executable, "static/tools/verify_opcua_public_contract.py",
         "--baseline", "v0.4.1", "--allow-v042-channel-additions"],
        cwd=ROOT,
        check=False,
    )
    require(result.returncode == 0, "v0.4.1 public-contract comparison failed")
    print("v0.4.2-opcua-source: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
