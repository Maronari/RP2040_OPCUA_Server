#!/usr/bin/env python3
"""Verify a linked firmware ELF carries exactly its driver pack's industrial drivers.

ROADMAP.md's v0.5.1 acceptance asks to build at least two firmware profiles
and verify that a disabled driver's code/symbols are absent from the smaller
ELF. This reads the ELF's symbol table (`arm-none-eabi-nm`) and checks,
per FIRMWARE_DRIVER_PACK:

- base:       no symbol of any industrial-only driver or board binding
              backing is linked;
- industrial: every driver the industrial board actually binds is linked.

The empty `industrial_board_bindings_apply()` stub is deliberately linked in
every pack (main.c calls it unconditionally) and is not a driver symbol.
Industrial-only options whose code nothing references yet (TPS3851
watchdog, RS-485 transport, board diagnostics) are garbage-collected even
from the industrial ELF, so their presence is not required.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import subprocess
import sys


# Symbol-name prefixes of code that exists only for RP2350_INDUSTRIAL_MVP_V1.
INDUSTRIAL_ONLY_PREFIXES = (
    "native_ads8688_",
    "native_ad5422_",
    "native_max22190_",
    "native_ips2050_",
    "native_uic_",
    "native_tps3851_",
    "rp2350_pio_edge_counter_",
    "rp2350_rs485_",
    "board_diagnostics_",
    "g_industrial_profile",
)

# Drivers the industrial image must link: every one the industrial seed and
# board bindings reference (ADS8688, AD5422, MAX22190, IPS2050, UIC).
INDUSTRIAL_REQUIRED_SYMBOLS = (
    "native_ads8688_start",
    "native_ad5422_start",
    "native_max22190_start",
    "native_ips2050_start",
    "native_uic_start",
    "rp2350_pio_edge_counter_start",
    "g_industrial_profile",
)


def fail(message: str) -> None:
    raise SystemExit(f"driver-pack-symbols: FAIL: {message}")


def parse_nm(text: str) -> set[str]:
    symbols: set[str] = set()
    for line in text.splitlines():
        match = re.match(r"^\s*(?:[0-9A-Fa-f]+\s+)?[A-Za-z?]\s+(\S+)$", line)
        if match:
            symbols.add(match.group(1))
    return symbols


def check(pack: str, symbols: set[str]) -> str:
    if pack == "base":
        leaked = sorted(name for name in symbols if name.startswith(INDUSTRIAL_ONLY_PREFIXES))
        if leaked:
            fail(f"base ELF links industrial-only symbols: {', '.join(leaked[:10])}")
        return f"driver-pack-symbols: PASS (base, no industrial-only symbol among {len(symbols)})"
    if pack == "industrial":
        missing = [name for name in INDUSTRIAL_REQUIRED_SYMBOLS if name not in symbols]
        if missing:
            fail(f"industrial ELF is missing {', '.join(missing)}")
        return f"driver-pack-symbols: PASS (industrial, all {len(INDUSTRIAL_REQUIRED_SYMBOLS)} required symbols linked)"
    fail(f"unknown driver pack {pack!r}")
    raise AssertionError("unreachable")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--driver-pack", required=True)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--elf", type=pathlib.Path, help="ELF to read with --nm")
    source.add_argument("--nm-output", type=pathlib.Path, help="Pre-captured nm output (host tests)")
    parser.add_argument("--nm", default="arm-none-eabi-nm")
    args = parser.parse_args()

    if args.elf is not None:
        if not args.elf.is_file():
            fail(f"missing ELF {args.elf}")
        text = subprocess.run([args.nm, str(args.elf)], check=True, capture_output=True, text=True).stdout
    else:
        text = args.nm_output.read_text(encoding="utf-8")
    symbols = parse_nm(text)
    if not symbols:
        fail("no symbols read")
    print(check(args.driver_pack, symbols))
    return 0


if __name__ == "__main__":
    sys.exit(main())
