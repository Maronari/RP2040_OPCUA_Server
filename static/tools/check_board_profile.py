#!/usr/bin/env python3
"""Fail-closed check that a configured build's board wiring matches its driver pack.

FIRMWARE_DRIVER_PACK is a board profile: "industrial" is only for
RP2350_INDUSTRIAL_MVP_V1 and "base" only for the reference Waveshare
Core2350B + W5500 module. The root CMakeLists.txt derives PICO_BOARD and the
W5500 SPI pins from the pack; this tool verifies the result in the actual
build directory -- the cached pack and board, and the compile definitions
the W5500 port sources and the PSRAM allocator were really compiled with --
so a CMake regression cannot quietly ship industrial drivers on reference
wiring again (as every industrial build did before 2026-09-13).
"""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import shlex
import sys


PROFILES = {
    "industrial": {
        "board": "rp2350_industrial_mvp_v1",
        # HARDWARE_MVP.md section 6.3: GPIO4 ETH_MISO, GPIO5 ETH_CS_N,
        # GPIO6 ETH_SCK, GPIO7 ETH_MOSI, GPIO8 ETH_INT_N.
        "w5500_pins": {"PIN_MISO": "4", "PIN_CS": "5", "PIN_SCK": "6", "PIN_MOSI": "7", "PIN_INT": "8"},
    },
    "base": {
        "board": "waveshare_core2350b_16mb_psram8mb",
        # Reference module: the vendored headers' defaults, no overrides.
        "w5500_pins": {},
    },
}

W5500_PIN_NAMES = ("PIN_MISO", "PIN_CS", "PIN_SCK", "PIN_MOSI", "PIN_INT")

# Translation units whose board-specific pins or board header must follow
# the profile, keyed by a path suffix in compile_commands.json.
W5500_SOURCES = ("port/ioLibrary_Driver/src/w5x00_spi.c", "port/ioLibrary_Driver/src/w5x00_gpio_irq.c")
PSRAM_SOURCE = "port/psram/psram_allocator.c"


def fail(message: str) -> None:
    raise SystemExit(f"board-profile: FAIL: {message}")


def read_cache(build_dir: pathlib.Path) -> dict[str, str]:
    cache_path = build_dir / "CMakeCache.txt"
    if not cache_path.is_file():
        fail(f"missing {cache_path}")
    values: dict[str, str] = {}
    for line in cache_path.read_text(encoding="utf-8", errors="replace").splitlines():
        match = re.match(r"^([A-Za-z0-9_]+):[A-Z_]+=(.*)$", line)
        if match:
            values[match.group(1)] = match.group(2)
    return values


def command_arguments(entry: dict[str, object]) -> list[str]:
    if "arguments" in entry:
        return [str(item) for item in entry["arguments"]]  # type: ignore[union-attr]
    command = str(entry.get("command", ""))
    # compile_commands.json from Ninja on Windows uses backslash paths;
    # POSIX-mode splitting would eat them, and only -D tokens matter here.
    return shlex.split(command, posix=False)


def definitions(arguments: list[str]) -> dict[str, str]:
    result: dict[str, str] = {}
    for index, token in enumerate(arguments):
        text = None
        if token.startswith("-D") and len(token) > 2:
            text = token[2:]
        elif token == "-D" and index + 1 < len(arguments):
            text = arguments[index + 1]
        if text is None:
            continue
        name, _, value = text.partition("=")
        # String values arrive shell-escaped (-DPICO_BOARD=\"name\"); the
        # names and numbers compared here never contain quotes themselves.
        result[name] = re.sub(r'[\\"]', "", value)
    return result


def find_entry(entries: list[dict[str, object]], suffix: str) -> dict[str, object]:
    for entry in entries:
        path = str(entry.get("file", "")).replace("\\", "/")
        if path.endswith(suffix):
            return entry
    fail(f"compile_commands.json has no entry for {suffix}")
    raise AssertionError("unreachable")


def check(build_dir: pathlib.Path) -> str:
    cache = read_cache(build_dir)
    pack = cache.get("FIRMWARE_DRIVER_PACK")
    if pack not in PROFILES:
        fail(f"FIRMWARE_DRIVER_PACK is {pack!r}, expected one of {sorted(PROFILES)}")
    profile = PROFILES[pack]
    board = cache.get("PICO_BOARD")
    if board != profile["board"]:
        fail(f"FIRMWARE_DRIVER_PACK={pack} requires PICO_BOARD={profile['board']}, build has {board!r}")
    if "FIRMWARE_ENABLE_INDUSTRIAL_W5500_PINS" in cache:
        fail("FIRMWARE_ENABLE_INDUSTRIAL_W5500_PINS is still a cache entry; the pin mapping must follow the pack")

    compile_commands = build_dir / "compile_commands.json"
    if not compile_commands.is_file():
        fail(f"missing {compile_commands} (configure with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON)")
    entries = json.loads(compile_commands.read_text(encoding="utf-8"))

    for source in W5500_SOURCES:
        defined = definitions(command_arguments(find_entry(entries, source)))
        actual = {name: defined[name] for name in W5500_PIN_NAMES if name in defined}
        if actual != profile["w5500_pins"]:
            fail(f"{source} compiled with W5500 pins {actual}, FIRMWARE_DRIVER_PACK={pack} requires "
                 f"{profile['w5500_pins'] or 'the reference defaults (no overrides)'}")

    psram_board = definitions(command_arguments(find_entry(entries, PSRAM_SOURCE))).get("PICO_BOARD")
    if psram_board != profile["board"]:
        fail(f"{PSRAM_SOURCE} compiled for PICO_BOARD {psram_board!r}, expected {profile['board']!r}")

    return f"board-profile: PASS (FIRMWARE_DRIVER_PACK={pack}, PICO_BOARD={board})"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True, type=pathlib.Path)
    args = parser.parse_args()
    print(check(args.build_dir.resolve()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
