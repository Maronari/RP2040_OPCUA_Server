#!/usr/bin/env python3
"""Compile and run deterministic RP2 OPC UA DateTime conversion tests."""

from __future__ import annotations

import argparse
import pathlib
import subprocess
import tempfile


def check_amalgamation_hook(root: pathlib.Path) -> None:
    """The vendored open62541 FreeRTOS clock must delegate to rp2_ua_datetime_now().

    The helper's own tests pass even when nothing calls it: the v1.5.3 ->
    v1.5.7 amalgamation upgrade (36e03c9) replaced the patched
    UA_DateTime_now() with the stock tick-based one, and every OPC UA
    timestamp reverted to 1970 + uptime while this suite stayed green.
    """
    raw = (root / "port" / "open62541" / "open62541.c").read_text(encoding="utf-8", errors="replace")
    text = raw.replace(chr(13) + chr(10), chr(10))
    start = text.find("#ifdef UA_ARCHITECTURE_FREERTOS" + chr(10) + chr(10) + "#include <FreeRTOS.h>")
    if start < 0:
        raise SystemExit("rp2-ua-datetime: FAIL: FreeRTOS clock block not found in open62541.c")
    block = text[start:text.find("#endif /* UA_ARCHITECTURE_FREERTOS */", start)]
    body_start = block.find("UA_DateTime UA_DateTime_now(void) {")
    if body_start < 0:
        raise SystemExit("rp2-ua-datetime: FAIL: UA_DateTime_now() missing from the FreeRTOS clock block")
    body = block[body_start:block.find(chr(10) + "}", body_start)]
    if '#include "rp2_ua_datetime_clock.h"' not in block or "rp2_ua_datetime_now(" not in body:
        raise SystemExit("rp2-ua-datetime: FAIL: open62541.c UA_DateTime_now() no longer uses rp2_ua_datetime_now() "
                         "-- re-apply the RP2 port patch after the amalgamation upgrade")
    if "xTaskGetTickCount" in body:
        raise SystemExit("rp2-ua-datetime: FAIL: open62541.c UA_DateTime_now() derives wall time from FreeRTOS ticks")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", default="gcc", help="Host C compiler")
    args = parser.parse_args()

    root = pathlib.Path(__file__).resolve().parents[2]
    check_amalgamation_hook(root)
    source = root / "port" / "open62541" / "rp2_ua_datetime_clock.c"
    test = root / "static" / "tests" / "rp2_ua_datetime_clock" / "rp2_ua_datetime_clock_test.c"
    with tempfile.TemporaryDirectory(prefix="rp2-ua-datetime-") as directory:
        executable = pathlib.Path(directory) / "rp2_ua_datetime_clock_test.exe"
        subprocess.run(
            [
                args.cc,
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                f"-I{root / 'port' / 'open62541'}",
                f"-I{root / 'services' / 'include'}",
                str(source),
                str(test),
                "-o",
                str(executable),
            ],
            check=True,
        )
        subprocess.run([str(executable)], check=True)

    print("RP2 OPC UA DateTime host suite: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
