#!/usr/bin/env python3
"""Generate a deterministic v0.5.2 ELF/map/artifact size report.

The report separates load bytes in external Flash from run-time reservations
in internal SRAM. PSRAM is not a linker region in this firmware; runtime
PSRAM arenas are therefore recorded as unavailable here and measured by T2.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import re
import subprocess
import sys
from dataclasses import dataclass
from typing import Any


SCHEMA_VERSION = 1
CONTRACT_ID = "rp2-opcua-v0.5.2-resource-benchmark"
MAX_MAP_BYTES = 64 * 1024 * 1024
MAX_TOOL_OUTPUT_BYTES = 16 * 1024 * 1024
MAX_SECTIONS = 512
MAX_REPORT_BYTES = 8 * 1024 * 1024
SECTION_LINE = re.compile(
    r"^\s*\d+\s+(\S+)\s+([0-9A-Fa-f]+)\s+([0-9A-Fa-f]+)\s+"
    r"([0-9A-Fa-f]+)\s+[0-9A-Fa-f]+\s+2\*\*\d+\s*$"
)
MAP_REGION_LINE = re.compile(
    r"^([A-Za-z_][A-Za-z0-9_]*)\s+(0x[0-9A-Fa-f]+)\s+"
    r"(0x[0-9A-Fa-f]+)\s*([A-Za-z]*)\s*$"
)
NM_LINE = re.compile(
    r"^([0-9A-Fa-f]+)\s+([0-9A-Fa-f]+)\s+([A-Za-z])\s+(\S+)\s*$"
)


class ReportError(ValueError):
    """Raised when build evidence is missing, malformed or inconsistent."""


@dataclass(frozen=True)
class Region:
    name: str
    origin: int
    length: int
    attributes: str

    def contains(self, address: int) -> bool:
        return self.origin <= address < self.origin + self.length


@dataclass(frozen=True)
class Section:
    name: str
    size: int
    vma: int
    lma: int
    flags: tuple[str, ...]


def parse_map_regions(text: str) -> list[Region]:
    if "Memory Configuration" not in text or "Linker script and memory map" not in text:
        raise ReportError("map file lacks Memory Configuration or linker map")
    region_text = text.split("Memory Configuration", 1)[1].split(
        "Linker script and memory map", 1
    )[0]
    regions: list[Region] = []
    for line in region_text.splitlines():
        match = MAP_REGION_LINE.match(line.strip())
        if not match or match.group(1) == "Name":
            continue
        name, origin, length, attributes = match.groups()
        if name == "default":
            continue
        size = int(length, 16)
        if size <= 0:
            raise ReportError(f"map region {name} has no capacity")
        regions.append(Region(name, int(origin, 16), size, attributes))
    if not regions:
        raise ReportError("map file contains no named memory regions")
    names = {region.name for region in regions}
    if "FLASH" not in names or not ({"RAM", "SCRATCH_X", "SCRATCH_Y"} & names):
        raise ReportError("map regions do not identify FLASH and internal SRAM")
    return regions


def parse_objdump_sections(text: str) -> list[Section]:
    lines = text.splitlines()
    sections: list[Section] = []
    index = 0
    while index < len(lines):
        match = SECTION_LINE.match(lines[index])
        if not match:
            index += 1
            continue
        if index + 1 >= len(lines):
            raise ReportError(f"objdump flags missing for section {match.group(1)}")
        flags = tuple(flag.strip() for flag in lines[index + 1].split(",") if flag.strip())
        if not flags:
            raise ReportError(f"objdump flags malformed for section {match.group(1)}")
        sections.append(
            Section(
                name=match.group(1),
                size=int(match.group(2), 16),
                vma=int(match.group(3), 16),
                lma=int(match.group(4), 16),
                flags=flags,
            )
        )
        if len(sections) > MAX_SECTIONS:
            raise ReportError(f"objdump section count exceeds {MAX_SECTIONS}")
        index += 2
    if not sections:
        raise ReportError("objdump output contains no section headers")
    if not any(section.name == ".text" and section.size for section in sections):
        raise ReportError("objdump output has no non-empty .text section")
    return sections


def parse_nm_symbols(text: str) -> dict[str, int]:
    symbols: dict[str, int] = {}
    for line in text.splitlines():
        match = NM_LINE.match(line.strip())
        if not match:
            continue
        _address, size, _kind, name = match.groups()
        if name in symbols and name == "ucHeap":
            raise ReportError(f"nm output contains duplicate symbol {name}")
        symbols.setdefault(name, int(size, 16))
    if not symbols:
        raise ReportError("nm output contains no sized symbols")
    return symbols


def region_for(address: int, regions: list[Region]) -> str | None:
    matches = [region.name for region in regions if region.contains(address)]
    if len(matches) > 1:
        raise ReportError(f"address 0x{address:x} belongs to overlapping regions {matches}")
    return matches[0] if matches else None


def union_size(intervals: list[tuple[int, int]]) -> int:
    if not intervals:
        return 0
    ordered = sorted(intervals)
    total = 0
    start, end = ordered[0]
    for next_start, next_end in ordered[1:]:
        if next_start <= end:
            end = max(end, next_end)
        else:
            total += end - start
            start, end = next_start, next_end
    return total + end - start


def sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def artifact(path: pathlib.Path) -> dict[str, Any]:
    if not path.is_file():
        raise ReportError(f"missing artifact: {path}")
    size = path.stat().st_size
    if size <= 0:
        raise ReportError(f"empty artifact: {path}")
    return {"name": path.name, "sizeBytes": size, "sha256": sha256(path)}


def _run(tool: str, *arguments: str) -> str:
    result = subprocess.run(
        [tool, *arguments], text=True, encoding="utf-8", errors="replace",
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
    )
    if result.returncode != 0:
        raise ReportError(f"{tool} exited {result.returncode}: {result.stderr.strip()}")
    if len(result.stdout.encode("utf-8")) > MAX_TOOL_OUTPUT_BYTES:
        raise ReportError(f"{tool} output exceeds {MAX_TOOL_OUTPUT_BYTES} bytes")
    return result.stdout


def parse_cmake_cache(path: pathlib.Path) -> dict[str, str]:
    if not path.is_file() or path.stat().st_size <= 0:
        raise ReportError(f"missing or empty CMake cache: {path}")
    values: dict[str, str] = {}
    for raw_line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if not raw_line or raw_line.startswith(("#", "//")) or "=" not in raw_line:
            continue
        key_and_type, value = raw_line.split("=", 1)
        key = key_and_type.split(":", 1)[0]
        values[key] = value
    required = {"CMAKE_BUILD_TYPE", "CMAKE_C_COMPILER", "FIRMWARE_DRIVER_PACK", "PICO_BOARD"}
    missing = required - values.keys()
    if missing:
        raise ReportError(f"CMake cache missing {sorted(missing)}")
    return values


def static_profile_id(cache: dict[str, str]) -> tuple[str, str]:
    benchmark_profile = cache.get("FIRMWARE_BENCHMARK_PROFILE", "off").strip().lower() or "off"
    profile_parts = ["static"]
    if benchmark_profile != "off":
        profile_parts.append(benchmark_profile)
    profile_parts.extend((cache["FIRMWARE_DRIVER_PACK"], cache["PICO_BOARD"],
                          cache["CMAKE_BUILD_TYPE"]))
    profile_id = re.sub(r"[^a-z0-9._-]+", "-", "-".join(profile_parts).lower()).strip("-")
    return profile_id, benchmark_profile


def benchmark_profile_id(cache: dict[str, str], benchmark_profile: str) -> str | None:
    if benchmark_profile != "synthetic":
        return None
    required = (
        "FIRMWARE_BENCHMARK_CHANNELS", "FIRMWARE_BENCHMARK_RATE_HZ",
        "FIRMWARE_BENCHMARK_CLIENTS", "FIRMWARE_BENCHMARK_SECURITY",
        "FIRMWARE_BENCHMARK_EVENTS", "FIRMWARE_BENCHMARK_HISTORY",
    )
    missing = [key for key in required if key not in cache]
    if missing:
        raise ReportError(f"synthetic benchmark cache missing {missing}")
    enabled = {"1", "on", "true", "yes"}
    events = int(cache["FIRMWARE_BENCHMARK_EVENTS"].strip().lower() in enabled)
    history = int(cache["FIRMWARE_BENCHMARK_HISTORY"].strip().lower() in enabled)
    return (
        f"v052-syn-c{cache['FIRMWARE_BENCHMARK_CHANNELS']}-ns0full-"
        f"sec{cache['FIRMWARE_BENCHMARK_SECURITY'].strip().lower()}-"
        f"cl{cache['FIRMWARE_BENCHMARK_CLIENTS']}-"
        f"hz{cache['FIRMWARE_BENCHMARK_RATE_HZ']}-ev{events}-hist{history}"
    )


def collect_provenance(source_dir: pathlib.Path, cmake_cache: pathlib.Path) -> dict[str, Any]:
    cache = parse_cmake_cache(cmake_cache)
    version_file = source_dir / "cmake" / "firmware_version.cmake"
    try:
        version_text = version_file.read_text(encoding="utf-8")
    except OSError as exc:
        raise ReportError(f"cannot read firmware version source: {exc}") from exc
    version_match = re.search(r'set\(RP2_FIRMWARE_VERSION\s+"([0-9]+\.[0-9]+\.[0-9]+)"', version_text)
    if not version_match:
        raise ReportError("firmware version source is malformed")

    header = source_dir / "port" / "open62541" / "include" / "open62541.h"
    try:
        header_text = header.read_text(encoding="utf-8", errors="replace")
    except OSError as exc:
        raise ReportError(f"cannot read open62541 configuration: {exc}") from exc
    if "#define UA_GENERATED_NAMESPACE_ZERO_FULL" in header_text:
        namespace_zero = "full"
    elif "#define UA_NAMESPACE_ZERO_MINIMAL" in header_text:
        namespace_zero = "minimal"
    else:
        raise ReportError("open62541 Namespace 0 profile is not identifiable")

    git_commit = _run("git", "-C", str(source_dir), "rev-parse", "HEAD").strip()
    if not re.fullmatch(r"[0-9a-fA-F]{40}", git_commit):
        raise ReportError("git commit is not a full SHA")
    git_status = _run("git", "-C", str(source_dir), "status", "--porcelain").strip()
    build_flags = {
        key: cache[key]
        for key in sorted(cache)
        if key in {
            "CMAKE_C_FLAGS", "CMAKE_C_FLAGS_DEBUG", "CMAKE_EXE_LINKER_FLAGS",
            "OTA_ROM_PRIMARY_APP", "OTA_BUILD_BOOTLOADER", "WIZNET_CHIP",
            "FIRMWARE_DRIVER_PACK", "PICO_BOARD", "FIRMWARE_BENCHMARK_PROFILE",
            "FIRMWARE_BENCHMARK_SECURITY", "FIRMWARE_BENCHMARK_CHANNELS",
            "FIRMWARE_BENCHMARK_RATE_HZ", "FIRMWARE_BENCHMARK_CLIENTS",
            "FIRMWARE_BENCHMARK_EVENTS", "FIRMWARE_BENCHMARK_HISTORY",
        }
    }
    profile_id, benchmark_profile = static_profile_id(cache)
    if benchmark_profile == "resource-exhaustion":
        security_policy = "not-applicable"
        security_mode = "not-applicable"
    elif cache.get("FIRMWARE_BENCHMARK_SECURITY", "").strip().lower() == "test-none":
        security_policy = "None"
        security_mode = "None"
    else:
        security_policy = "Basic256Sha256"
        security_mode = "SignAndEncrypt"
    return {
        "profileSchemaVersion": 1,
        "profileId": profile_id,
        "benchmarkProfileId": benchmark_profile_id(cache, benchmark_profile),
        "benchmarkProfile": benchmark_profile,
        "firmwareVersion": version_match.group(1),
        "gitCommit": git_commit.lower(),
        "gitDirty": bool(git_status),
        "boardRevision": cache["PICO_BOARD"],
        "driverPack": cache["FIRMWARE_DRIVER_PACK"],
        "namespaceZero": namespace_zero,
        "securityPolicy": security_policy,
        "securityMode": security_mode,
        "toolchain": cache["CMAKE_C_COMPILER"],
        "buildType": cache["CMAKE_BUILD_TYPE"],
        "buildFlags": build_flags,
    }


def build_report(
    *, elf: pathlib.Path, uf2: pathlib.Path, binary: pathlib.Path,
    map_file: pathlib.Path, objdump_text: str, nm_text: str,
    provenance: dict[str, Any],
) -> dict[str, Any]:
    if not map_file.is_file() or map_file.stat().st_size <= 0:
        raise ReportError(f"missing or empty map file: {map_file}")
    if map_file.stat().st_size > MAX_MAP_BYTES:
        raise ReportError(f"map file exceeds {MAX_MAP_BYTES} bytes")
    map_text = map_file.read_text(encoding="utf-8", errors="replace")
    regions = parse_map_regions(map_text)
    sections = parse_objdump_sections(objdump_text)
    symbols = parse_nm_symbols(nm_text)

    flash_regions = {region.name for region in regions if region.name == "FLASH"}
    sram_regions = {region.name for region in regions if region.name in {"RAM", "SCRATCH_X", "SCRATCH_Y"}}
    section_rows: list[dict[str, Any]] = []
    flash_load_intervals: list[tuple[int, int]] = []
    sram_static_bytes = 0
    psram_static_bytes = 0
    for section in sections:
        run_region = region_for(section.vma, regions) if section.size else None
        load_region = region_for(section.lma, regions) if section.size else None
        alloc = "ALLOC" in section.flags
        load = "LOAD" in section.flags
        if alloc and run_region in sram_regions:
            sram_static_bytes += section.size
        if alloc and section.vma >= 0x11000000 and section.vma < 0x12000000:
            psram_static_bytes += section.size
        if alloc and load and load_region in flash_regions:
            flash_load_intervals.append((section.lma, section.lma + section.size))
        section_rows.append({
            "name": section.name,
            "sizeBytes": section.size,
            "vma": f"0x{section.vma:08x}",
            "lma": f"0x{section.lma:08x}",
            "flags": list(section.flags),
            "runRegion": run_region,
            "loadRegion": load_region,
        })

    by_name = {section.name: section for section in sections}
    if len(by_name) != len(sections):
        raise ReportError("objdump output contains duplicate section names")
    heap_bytes = symbols.get("ucHeap")
    if heap_bytes is None or heap_bytes <= 0:
        raise ReportError("nm output lacks the bounded FreeRTOS ucHeap symbol")
    linker_stack_bytes = by_name.get(".stack_dummy", Section("", 0, 0, 0, ())).size
    if linker_stack_bytes <= 0:
        raise ReportError("ELF lacks the bounded linker .stack_dummy reservation")

    artifacts = {
        "elf": artifact(elf),
        "uf2": artifact(uf2),
        "bin": artifact(binary),
        "map": artifact(map_file),
    }
    totals = {
        "ElfFileBytes": artifacts["elf"]["sizeBytes"],
        "Uf2FileBytes": artifacts["uf2"]["sizeBytes"],
        "BinFileBytes": artifacts["bin"]["sizeBytes"],
        "ExternalFlashLoadBytes": union_size(flash_load_intervals),
        "SramDataBytes": by_name.get(".data", Section("", 0, 0, 0, ())).size,
        "SramBssBytes": by_name.get(".bss", Section("", 0, 0, 0, ())).size,
        "SramStaticTotalBytes": sram_static_bytes,
        "SramReservedHeapBytes": heap_bytes,
        "SramReservedTaskStackBytes": None,
        "PsramStaticBytes": psram_static_bytes,
    }
    return {
        "schemaVersion": SCHEMA_VERSION,
        "contractId": CONTRACT_ID,
        "measurementKind": "static-build-layout",
        "provenance": provenance,
        "artifacts": artifacts,
        "memoryRegions": [
            {"name": region.name, "origin": f"0x{region.origin:08x}",
             "lengthBytes": region.length, "attributes": region.attributes}
            for region in regions
        ],
        "sections": section_rows,
        "totals": totals,
        "reservations": {
            "freeRtosHeap": {"state": "measured", "bytes": heap_bytes,
                              "source": "ELF-symbol-ucHeap", "includedInSramStaticTotal": True},
            "linkerStack": {"state": "measured", "bytes": linker_stack_bytes,
                            "source": "ELF-section-.stack_dummy", "includedInSramStaticTotal": True},
            "freeRtosTaskStacks": {"state": "unavailable", "bytes": None,
                                   "reason": "dynamically-allocated-from-FreeRTOS-heap",
                                   "includedInSramStaticTotal": False},
            "lwipPsramArena": {"state": "unavailable", "bytes": None,
                               "reason": "runtime-allocated-from-PSRAM; measured-by-T2",
                               "includedInSramStaticTotal": False},
        },
        "notes": [
            "ExternalFlashLoadBytes is the union of ALLOC+LOAD section LMA intervals in FLASH.",
            "SramStaticTotalBytes includes linker reservations and must not be added to its heap/stack subsets.",
            "ELF debug metadata and runtime PSRAM use are not external-Flash load bytes.",
        ],
    }


def check_thresholds(report: dict[str, Any], threshold_path: pathlib.Path | None) -> list[str]:
    if threshold_path is None:
        return []
    try:
        document = json.loads(threshold_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ReportError(f"invalid threshold file: {exc}") from exc
    if document.get("schemaVersion") != 1 or not isinstance(document.get("maxima"), dict):
        raise ReportError("threshold file requires schemaVersion=1 and maxima object")
    failures: list[str] = []
    totals = report["totals"]
    for metric, maximum in document["maxima"].items():
        if metric not in totals:
            raise ReportError(f"threshold refers to unknown metric {metric}")
        if not isinstance(maximum, int) or isinstance(maximum, bool) or maximum < 0:
            raise ReportError(f"threshold {metric} must be a non-negative integer")
        actual = totals[metric]
        if actual is None:
            raise ReportError(f"threshold {metric} cannot target an unavailable metric")
        if actual > maximum:
            failures.append(f"{metric}: actual {actual} > maximum {maximum}")
    return failures


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--elf", required=True, type=pathlib.Path)
    parser.add_argument("--uf2", required=True, type=pathlib.Path)
    parser.add_argument("--bin", dest="binary", required=True, type=pathlib.Path)
    parser.add_argument("--map", dest="map_file", required=True, type=pathlib.Path)
    parser.add_argument("--objdump", default="arm-none-eabi-objdump")
    parser.add_argument("--nm", default="arm-none-eabi-nm")
    parser.add_argument("--thresholds", type=pathlib.Path)
    parser.add_argument("--source-dir", type=pathlib.Path,
                        default=pathlib.Path(__file__).resolve().parents[2])
    parser.add_argument("--cmake-cache", required=True, type=pathlib.Path)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        report = build_report(
            elf=args.elf.resolve(), uf2=args.uf2.resolve(), binary=args.binary.resolve(),
            map_file=args.map_file.resolve(),
            objdump_text=_run(args.objdump, "-h", str(args.elf.resolve())),
            nm_text=_run(args.nm, "--print-size", "--defined-only", str(args.elf.resolve())),
            provenance=collect_provenance(args.source_dir.resolve(), args.cmake_cache.resolve()),
        )
        failures = check_thresholds(report, args.thresholds)
        report["thresholds"] = {
            "state": "failed" if failures else ("measured" if args.thresholds else "not-run"),
            "source": str(args.thresholds.resolve()) if args.thresholds else None,
            "failures": failures,
        }
        args.output.parent.mkdir(parents=True, exist_ok=True)
        encoded = (json.dumps(report, indent=2, sort_keys=True) + "\n").encode("utf-8")
        if len(encoded) > MAX_REPORT_BYTES:
            raise ReportError(f"report exceeds {MAX_REPORT_BYTES} bytes")
        args.output.write_bytes(encoded)
    except (OSError, ReportError) as exc:
        print(f"static-size-report: FAIL: {exc}", file=sys.stderr)
        return 1
    if failures:
        print("static-size-report: FAIL: " + "; ".join(failures), file=sys.stderr)
        return 2
    print(
        "static-size-report: PASS "
        f"flash={report['totals']['ExternalFlashLoadBytes']} "
        f"sram={report['totals']['SramStaticTotalBytes']} "
        f"elf={report['totals']['ElfFileBytes']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
