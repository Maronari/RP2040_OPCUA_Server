#!/usr/bin/env python3
"""Host suite for check_board_profile.py: the driver pack must fix the board and W5500 pins.

Builds synthetic CMakeCache.txt/compile_commands.json fixtures -- no
toolchain needed -- including the exact failure the tool exists for: an
industrial pack compiled with the reference module's W5500 pins and board.
"""

from __future__ import annotations

import importlib.util
import json
import pathlib
import sys
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[2]
TOOLS = ROOT / "static" / "tools"

INDUSTRIAL_PINS = ["-DPIN_MISO=4", "-DPIN_CS=5", "-DPIN_SCK=6", "-DPIN_MOSI=7", "-DPIN_INT=8"]


def load():
    spec = importlib.util.spec_from_file_location("check_board_profile", TOOLS / "check_board_profile.py")
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    sys.modules["check_board_profile"] = module
    spec.loader.exec_module(module)
    return module


def write_build(directory: pathlib.Path, pack: str, board: str, pins: list[str], *,
                psram_board: str | None = None, stale_option: bool = False, use_arguments: bool = False) -> None:
    directory.mkdir(parents=True, exist_ok=True)
    cache = [f"FIRMWARE_DRIVER_PACK:STRING={pack}", f"PICO_BOARD:STRING={board}"]
    if stale_option:
        cache.append("FIRMWARE_ENABLE_INDUSTRIAL_W5500_PINS:BOOL=OFF")
    (directory / "CMakeCache.txt").write_text("\n".join(cache) + "\n", encoding="utf-8")

    def entry(source: str, extra: list[str]) -> dict[str, object]:
        board_define = f'-DPICO_BOARD=\\"{psram_board or board}\\"'
        arguments = ["arm-none-eabi-gcc", "-DPICO_ON_DEVICE=1", board_define, *extra, "-c", source]
        if use_arguments:
            return {"file": source, "arguments": arguments}
        return {"file": source, "command": " ".join(arguments)}

    entries = [
        entry("E:\\Projects\\repo\\port\\ioLibrary_Driver\\src\\w5x00_spi.c", pins),
        entry("/repo/port/ioLibrary_Driver/src/w5x00_gpio_irq.c", pins),
        entry("/repo/port/psram/psram_allocator.c", pins),
    ]
    (directory / "compile_commands.json").write_text(json.dumps(entries), encoding="utf-8")


def expect_fail(module, directory: pathlib.Path, fragment: str) -> None:
    try:
        module.check(directory)
    except SystemExit as error:
        assert fragment in str(error), f"expected {fragment!r} in {error}"
        return
    raise AssertionError(f"{directory.name}: expected failure containing {fragment!r}")


def main() -> int:
    module = load()
    with tempfile.TemporaryDirectory() as tmp:
        root = pathlib.Path(tmp)

        write_build(root / "industrial", "industrial", "rp2350_industrial_mvp_v1", INDUSTRIAL_PINS)
        assert "PASS" in module.check(root / "industrial")
        write_build(root / "industrial-args", "industrial", "rp2350_industrial_mvp_v1", INDUSTRIAL_PINS,
                    use_arguments=True)
        assert "PASS" in module.check(root / "industrial-args")
        write_build(root / "base", "base", "waveshare_core2350b_16mb_psram8mb", [])
        assert "PASS" in module.check(root / "base")

        # The pre-2026-09-13 industrial build: reference board, reference pins.
        write_build(root / "legacy", "industrial", "waveshare_core2350b_16mb_psram8mb", [], stale_option=True)
        expect_fail(module, root / "legacy", "requires PICO_BOARD=rp2350_industrial_mvp_v1")
        write_build(root / "reference-pins", "industrial", "rp2350_industrial_mvp_v1", [])
        expect_fail(module, root / "reference-pins", "w5x00_spi.c compiled with W5500 pins {}")
        write_build(root / "partial-pins", "industrial", "rp2350_industrial_mvp_v1", INDUSTRIAL_PINS[:-1])
        expect_fail(module, root / "partial-pins", "W5500 pins")
        write_build(root / "base-industrial-pins", "base", "waveshare_core2350b_16mb_psram8mb", INDUSTRIAL_PINS)
        expect_fail(module, root / "base-industrial-pins", "reference defaults")
        write_build(root / "stale-option", "industrial", "rp2350_industrial_mvp_v1", INDUSTRIAL_PINS,
                    stale_option=True)
        expect_fail(module, root / "stale-option", "still a cache entry")
        write_build(root / "psram-board", "industrial", "rp2350_industrial_mvp_v1", INDUSTRIAL_PINS,
                    psram_board="waveshare_core2350b_16mb_psram8mb")
        expect_fail(module, root / "psram-board", "psram_allocator.c compiled for PICO_BOARD")
        write_build(root / "unknown-pack", "motion", "rp2350_industrial_mvp_v1", INDUSTRIAL_PINS)
        expect_fail(module, root / "unknown-pack", "FIRMWARE_DRIVER_PACK is 'motion'")

        (root / "empty").mkdir()
        expect_fail(module, root / "empty", "missing")

    print("check-board-profile: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
