#!/usr/bin/env python3
"""Compile and run the config-transaction Flash backend host suite (drivers layer).

Covers stage-write success/classification, a simulated torn stage write
classifying as safely discardable, commit correctly promoting
staged->running and running->last-known-good, a simulated power-loss abort
at every transaction phase (before/after the stage commit page,
mid-promotion in both the last-known-good and running sub-steps, and after
a full commit) each recovering to the documented outcome, and
last-known-good retrieval after a corrupted running region -- entirely
through injected fake storage callbacks, no real hardware required.
"""

from __future__ import annotations

import argparse
import pathlib
import subprocess
import tempfile


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="gcc", help="Host C compiler")
    args = parser.parse_args()
    root = pathlib.Path(__file__).resolve().parents[2]
    with tempfile.TemporaryDirectory(prefix="config-transaction-backend-") as directory:
        executable = pathlib.Path(directory) / "config_transaction_backend_test.exe"
        subprocess.run([
            args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
            f"-I{root / 'drivers' / 'include'}", f"-I{root / 'services' / 'include'}",
            str(root / "drivers" / "src" / "config_transaction_backend.c"),
            str(root / "static" / "tests" / "config_transaction_backend" /
                "config_transaction_backend_test.c"),
            "-o", str(executable),
        ], check=True)
        subprocess.run([str(executable)], check=True)
    print("config transaction backend host suite: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
