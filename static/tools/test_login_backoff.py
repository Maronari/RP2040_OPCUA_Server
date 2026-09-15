#!/usr/bin/env python3
"""Compile and run the login-backoff host suite, and check its clock source.

The arithmetic in services/src/login_backoff.c is only correct on a clock
that advances monotonically. security_service.c used to feed it
get_system_time_us() -- the SNTP/AON wall clock, which reads a constant 0
until SNTP sets it -- so one failed login could lock an account for as long
as the network withheld a time, and an SNTP correction moved every running
lockout. This runner fails if security_service.c's backoff clock is anything
but the monotonic boot clock.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import subprocess
import sys
import tempfile


def check_security_service_clock(root: pathlib.Path) -> None:
    source = (root / "services" / "src" / "security_service.c").read_text(encoding="utf-8")
    match = re.search(r"static uint64_t security_service_now_us\(void\)\s*\{(.*?)\}", source, flags=re.S)
    if match is None:
        raise SystemExit("login-backoff: FAIL: security_service_now_us() not found in security_service.c")
    body = match.group(1)
    if "time_us_64()" not in body or "get_system_time" in body:
        raise SystemExit("login-backoff: FAIL: security_service_now_us() must return time_us_64(), "
                         f"not a wall-clock reading: {body.strip()!r}")
    for call in ("login_backoff_remaining_ms(runtime, now_us)", "login_backoff_record_failure(runtime, now_us)"):
        if call not in source:
            raise SystemExit(f"login-backoff: FAIL: security_service.c no longer calls {call}")
    if "get_system_time_us" in re.sub(r"/\*.*?\*/", "", source, flags=re.S):
        raise SystemExit("login-backoff: FAIL: security_service.c calls get_system_time_us() in code")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", default="gcc", help="Host C compiler")
    args = parser.parse_args()

    root = pathlib.Path(__file__).resolve().parents[2]
    check_security_service_clock(root)
    test_dir = root / "static" / "tests" / "login_backoff"
    with tempfile.TemporaryDirectory(prefix="login-backoff-") as directory:
        executable = pathlib.Path(directory) / "login_backoff_test.exe"
        subprocess.run([
            args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
            f"-I{root / 'services' / 'include'}",
            str(root / "services" / "src" / "login_backoff.c"),
            str(test_dir / "login_backoff_test.c"),
            "-o", str(executable),
        ], check=True)
        subprocess.run([str(executable)], check=True)

    print("login backoff host suite: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
