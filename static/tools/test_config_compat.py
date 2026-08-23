#!/usr/bin/env python3
"""Compile and run frozen v0.4.0 configuration migration tests."""

from __future__ import annotations

import argparse
import pathlib
import subprocess
import tempfile


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", default="gcc")
    args = parser.parse_args()
    root = pathlib.Path(__file__).resolve().parents[2]
    with tempfile.TemporaryDirectory(prefix="config-compat-") as directory:
        temporary = pathlib.Path(directory)
        stack_object = temporary / "config_compat_stack.o"
        subprocess.run([
            args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O0",
            "-fstack-usage", "-DPLATFORM_CHANNEL_LIMIT=16",
            f"-I{root / 'services' / 'include'}", "-c",
            str(root / "services" / "src" / "config_compat.c"),
            "-o", str(stack_object),
        ], check=True)
        stack_usage = stack_object.with_suffix(".su")
        if not stack_usage.is_file():
            raise SystemExit("config compatibility stack-usage report was not generated")
        decode_usage = None
        for line in stack_usage.read_text(encoding="utf-8").splitlines():
            fields = line.split("\t")
            if len(fields) < 2 or ":config_compat_" not in fields[0]:
                continue
            stack_bytes = int(fields[1])
            if stack_bytes > 512:
                raise SystemExit(
                    f"configuration compatibility helper stack use {stack_bytes} exceeds 512-byte bound")
            if fields[0].endswith(":config_compat_decode_binary"):
                decode_usage = stack_bytes
        if decode_usage is None:
            raise SystemExit("config_compat_decode_binary is absent from stack-usage report")

        for limit in (1, 4, 16):
            executable = temporary / f"config_compat_test_{limit}.exe"
            subprocess.run([
                args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
                f"-DPLATFORM_CHANNEL_LIMIT={limit}",
                f"-I{root / 'services' / 'include'}",
                str(root / "services" / "src" / "config_compat.c"),
                str(root / "static" / "tests" / "config_compat" / "config_compat_test.c"),
                "-o", str(executable),
            ], check=True)
            subprocess.run([str(executable)], check=True)

        for invalid in (0, 17):
            result = subprocess.run([
                args.cc, "-std=c11", f"-DPLATFORM_CHANNEL_LIMIT={invalid}",
                f"-I{root / 'services' / 'include'}", "-x", "c", "-", "-c",
                "-o", str(pathlib.Path(directory) / f"invalid_{invalid}.o"),
            ], input='#include "services/platform_limits.h"\n',
               text=True, capture_output=True, check=False)
            if result.returncode == 0:
                raise SystemExit(f"invalid channel limit {invalid} unexpectedly compiled")
    print("v0.4.0 config migration host suite: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
