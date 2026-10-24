#!/usr/bin/env python3
"""Fixture tests for the deterministic v0.5.2 static size reporter."""

from __future__ import annotations

import importlib.util
import contextlib
import io
import json
import pathlib
import sys
import tempfile


TOOL = pathlib.Path(__file__).with_name("generate_static_size_report.py")
SPEC = importlib.util.spec_from_file_location("generate_static_size_report", TOOL)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)

MAP = """Memory Configuration

Name             Origin             Length             Attributes
FLASH            0x10000000         0x01000000         xr
RAM              0x20000000         0x00080000         xrw
SCRATCH_X        0x20080000         0x00001000         xrw
SCRATCH_Y        0x20081000         0x00001000         xrw

Linker script and memory map
.text 0x10000000 0x100
"""
OBJDUMP = """Sections:
Idx Name          Size      VMA       LMA       File off  Algn
  0 .text         00000100  10000000  10000000  00001000  2**2
                  CONTENTS, ALLOC, LOAD, READONLY, CODE
  1 .data         00000020  20000000  10000100  00001100  2**2
                  CONTENTS, ALLOC, LOAD, DATA
  2 .bss          00000080  20000020  10000120  00001120  2**2
                  ALLOC
  3 .heap         00000010  200000a0  10000120  00001120  2**2
                  ALLOC, READONLY
  4 .scratch_x    00000010  20080000  10000120  00001120  2**2
                  CONTENTS, ALLOC, LOAD, DATA
  5 .stack_dummy  00000040  20081000  20081000  00001130  2**2
                  ALLOC, READONLY
  6 .debug_info   00001000  00000000  00000000  00002000  2**0
                  CONTENTS, READONLY, DEBUGGING, OCTETS
"""
NM = """20000100 00000100 b ucHeap
10000000 00000020 T main
"""


def expect_error(callable_, fragment: str) -> None:
    try:
        callable_()
    except MODULE.ReportError as exc:
        assert fragment in str(exc), (fragment, str(exc))
    else:
        raise AssertionError(f"expected ReportError containing {fragment!r}")


def main() -> None:
    profile, selector = MODULE.static_profile_id({
        "FIRMWARE_BENCHMARK_PROFILE": "resource-exhaustion",
        "FIRMWARE_DRIVER_PACK": "base", "PICO_BOARD": "reference",
        "CMAKE_BUILD_TYPE": "Debug",
    })
    assert selector == "resource-exhaustion"
    assert profile == "static-resource-exhaustion-base-reference-debug"
    profile, selector = MODULE.static_profile_id({
        "FIRMWARE_DRIVER_PACK": "base", "PICO_BOARD": "reference",
        "CMAKE_BUILD_TYPE": "Debug",
    })
    assert selector == "off"
    assert profile == "static-base-reference-debug"
    assert MODULE.benchmark_profile_id({}, "off") is None
    assert MODULE.benchmark_profile_id({
        "FIRMWARE_BENCHMARK_CHANNELS": "1",
        "FIRMWARE_BENCHMARK_RATE_HZ": "1",
        "FIRMWARE_BENCHMARK_CLIENTS": "1",
        "FIRMWARE_BENCHMARK_SECURITY": "basic256sha256-signandencrypt",
        "FIRMWARE_BENCHMARK_EVENTS": "OFF",
        "FIRMWARE_BENCHMARK_HISTORY": "OFF",
    }, "synthetic") == (
        "v052-syn-c1-ns0full-secbasic256sha256-signandencrypt-cl1-hz1-ev0-hist0"
    )
    expect_error(lambda: MODULE.benchmark_profile_id({}, "synthetic"),
                 "synthetic benchmark cache missing")
    assert MODULE.union_size([(0, 10), (5, 20), (30, 40)]) == 30
    regions = MODULE.parse_map_regions(MAP)
    assert [region.name for region in regions] == ["FLASH", "RAM", "SCRATCH_X", "SCRATCH_Y"]
    sections = MODULE.parse_objdump_sections(OBJDUMP)
    assert len(sections) == 7
    assert MODULE.parse_nm_symbols(NM)["ucHeap"] == 0x100

    with tempfile.TemporaryDirectory() as directory:
        root = pathlib.Path(directory)
        paths = {name: root / name for name in ("firmware.elf", "firmware.uf2", "firmware.bin")}
        for index, path in enumerate(paths.values(), start=1):
            path.write_bytes(bytes([index]) * (10 * index))
        map_file = root / "firmware.map"
        map_file.write_text(MAP, encoding="utf-8")
        report = MODULE.build_report(
            elf=paths["firmware.elf"], uf2=paths["firmware.uf2"],
            binary=paths["firmware.bin"], map_file=map_file,
            objdump_text=OBJDUMP, nm_text=NM,
            provenance={"profileId": "fixture", "gitDirty": False},
        )
        assert report["totals"]["ExternalFlashLoadBytes"] == 0x130
        assert report["totals"]["SramStaticTotalBytes"] == 0x100
        assert report["totals"]["SramDataBytes"] == 0x20
        assert report["totals"]["SramBssBytes"] == 0x80
        assert report["totals"]["SramReservedHeapBytes"] == 0x100
        assert report["totals"]["SramReservedTaskStackBytes"] is None
        assert report["reservations"]["freeRtosTaskStacks"]["state"] == "unavailable"
        assert report["artifacts"]["elf"]["sha256"] == MODULE.sha256(paths["firmware.elf"])

        thresholds = root / "thresholds.json"
        thresholds.write_text(json.dumps({"schemaVersion": 1, "maxima": {
            "ExternalFlashLoadBytes": 0x130,
            "SramStaticTotalBytes": 0x100,
        }}), encoding="utf-8")
        assert MODULE.check_thresholds(report, thresholds) == []
        thresholds.write_text(json.dumps({"schemaVersion": 1, "maxima": {
            "ExternalFlashLoadBytes": 0x12f,
        }}), encoding="utf-8")
        assert "ExternalFlashLoadBytes" in MODULE.check_thresholds(report, thresholds)[0]

        saved_argv = sys.argv
        saved_run = MODULE._run
        saved_collect_provenance = MODULE.collect_provenance
        try:
            MODULE._run = lambda _tool, *arguments: OBJDUMP if "-h" in arguments else NM
            output = root / "failed-threshold-report.json"
            sys.argv = [
                str(TOOL), "--elf", str(paths["firmware.elf"]),
                "--uf2", str(paths["firmware.uf2"]),
                "--bin", str(paths["firmware.bin"]),
                "--map", str(map_file), "--thresholds", str(thresholds),
                "--cmake-cache", str(root / "CMakeCache.txt"),
                "--output", str(output),
            ]
            (root / "CMakeCache.txt").write_text(
                "CMAKE_BUILD_TYPE:STRING=Debug\nCMAKE_C_COMPILER:FILEPATH=gcc\n"
                "FIRMWARE_DRIVER_PACK:STRING=base\nPICO_BOARD:STRING=fixture\n",
                encoding="utf-8",
            )
            MODULE.collect_provenance = lambda *_args: {"profileId": "fixture", "gitDirty": False}
            error_output = io.StringIO()
            with contextlib.redirect_stderr(error_output):
                assert MODULE.main() == 2
            assert "ExternalFlashLoadBytes" in error_output.getvalue()
            assert output.read_bytes().endswith(b"\n")
            assert b"\r\n" not in output.read_bytes()
            assert json.loads(output.read_text(encoding="utf-8"))["thresholds"]["state"] == "failed"
        finally:
            MODULE._run = saved_run
            MODULE.collect_provenance = saved_collect_provenance
            sys.argv = saved_argv

        thresholds.write_text(json.dumps({"schemaVersion": 1, "maxima": {
            "UnknownBytes": 1,
        }}), encoding="utf-8")
        expect_error(lambda: MODULE.check_thresholds(report, thresholds), "unknown metric")

        paths["firmware.bin"].write_bytes(b"")
        expect_error(lambda: MODULE.artifact(paths["firmware.bin"]), "empty artifact")

    expect_error(lambda: MODULE.parse_map_regions("broken"), "lacks Memory Configuration")
    expect_error(lambda: MODULE.parse_objdump_sections("broken"), "no section headers")
    expect_error(lambda: MODULE.parse_nm_symbols("broken"), "no sized symbols")
    duplicate_nm = NM + "20000200 00000020 b ucHeap\n"
    expect_error(lambda: MODULE.parse_nm_symbols(duplicate_nm), "duplicate symbol")
    print("static size report fixture suite: PASS")


if __name__ == "__main__":
    main()
