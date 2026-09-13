#!/usr/bin/env python3
"""Host suite for check_driver_pack_symbols.py, on captured-nm-style fixtures (no toolchain)."""

from __future__ import annotations

import importlib.util
import pathlib
import sys


ROOT = pathlib.Path(__file__).resolve().parents[2]
TOOLS = ROOT / "static" / "tools"

GENERIC = """\
10000a34 T industrial_board_bindings_apply
10001000 T main
10002000 T native_gpio_output_start
20000000 B g_basic_profile
         U memcpy
"""

INDUSTRIAL = GENERIC + """\
10030000 T native_ads8688_start
10030100 T native_ad5422_start
10030200 T native_max22190_start
10030300 T native_ips2050_start
10030c50 T native_uic_start
10030fd0 t rp2350_pio_edge_counter_start
10342688 R g_industrial_profile
"""


def load():
    spec = importlib.util.spec_from_file_location("check_driver_pack_symbols", TOOLS / "check_driver_pack_symbols.py")
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    sys.modules["check_driver_pack_symbols"] = module
    spec.loader.exec_module(module)
    return module


def expect_fail(module, pack: str, text: str, fragment: str) -> None:
    try:
        module.check(pack, module.parse_nm(text))
    except SystemExit as error:
        assert fragment in str(error), f"expected {fragment!r} in {error}"
        return
    raise AssertionError(f"{pack}: expected failure containing {fragment!r}")


def main() -> int:
    module = load()

    symbols = module.parse_nm(INDUSTRIAL)
    assert "memcpy" in symbols and "g_industrial_profile" in symbols

    # The no-op bindings stub is linked in every pack and is not a driver.
    assert "PASS" in module.check("base", module.parse_nm(GENERIC))
    assert "PASS" in module.check("industrial", module.parse_nm(INDUSTRIAL))

    # A base ELF with any industrial driver leaked in fails, whichever one.
    for leaked in ("10000000 T native_max22190_read", "10000000 t rp2350_pio_edge_counter_start",
                   "10000000 T board_diagnostics_collect", "10000000 R g_industrial_profile"):
        expect_fail(module, "base", GENERIC + leaked + "\n", "base ELF links industrial-only symbols")

    # An industrial ELF missing a bound driver fails and names it.
    without_uic = "\n".join(line for line in INDUSTRIAL.splitlines() if "native_uic_start" not in line)
    expect_fail(module, "industrial", without_uic, "native_uic_start")
    # The swapped case: industrial expectations against a base image.
    expect_fail(module, "industrial", GENERIC, "native_ads8688_start")

    expect_fail(module, "motion", GENERIC, "unknown driver pack")

    print("check-driver-pack-symbols: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
