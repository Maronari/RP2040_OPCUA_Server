#!/usr/bin/env python3
"""Bounded T0 evidence collection and fail-closed RAM-probe launcher.

No application flashing is implemented here. run-ram accepts only SRAM ELF
segments and requires an explicit execution command. The optional write probe
additionally requires a fresh full-device verified backup and idle/blank guards.
Backup files contain private device data: keep the output directory local.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import re
import struct
import subprocess
import time
import zlib

MAX_CAPTURE = 1024 * 1024
MAX_LINE = 2048
BOARD_SERIAL = "C3F9822180D224D1"
BASELINE_BYTES = 3586824
BASELINE_SHA256 = "9a7f875d20ed71bda98286a856400351058e179cd93545eb15777784225837ab"
FLASH_BYTES = 0x1000000
TEST_OFFSET = 0xF57000
TAIL_OFFSET = 0xF58000
STATE_OFFSET = 0xFF0000
PHASES = frozenset("main_enter console_ready scheduler_running flash_coordinator config_init "
                   "channel_init network_init history_init security_init init_complete opcua_task "
                   "waiting_ipv4 ipv4_ready server_created address_space server_startup "
                   "server_iterate init_stack opcua_stack".split())
REQUIRED = frozenset("main_enter scheduler_running flash_coordinator config_init network_init "
                     "history_init server_startup server_iterate init_stack opcua_stack".split())
CRYPTO_TESTS = frozenset("tail_before sha256_abc ecdsa_positive ecdsa_modified_hash_rejected "
                        "ecdsa_zero_r_rejected ecdsa_wrong_public_key_rejected flash_image_sha256 "
                        "flash_image_sha256_cpu_copy ecdsa_small_arena_rejected "
                        "ecdsa_positive_after_small_arena tail_after".split())
WRITE_GUARD_TESTS = frozenset("flash_write_guard_image flash_write_guard_tail flash_write_post_image".split())
# Execution pins are filled only after reviewing and building this exact
# diagnostic source. Offline preflight is not execution authorization.
APPROVED_RAM_ELFS: dict[str, str] = {
    "8122c5c40ab6e22e4a651f5656dd33708a0e5625be8be44484b3c3b21b4e9969": "read_only",
    "02ed70293ab1d7cc8b6d590f41e24d1de82d9c739e794f2fd370274a0665fbf5": "write_test",
}


class EvidenceError(ValueError):
    pass


def read_bounded(path: pathlib.Path, limit: int) -> bytes:
    with path.open("rb") as stream:
        data = stream.read(limit + 1)
    if len(data) > limit:
        raise EvidenceError("input exceeds its size bound")
    return data


def uint(text: str, maximum: int = (1 << 64) - 1) -> int:
    if not re.fullmatch(r"[0-9]{1,20}", text) or int(text) > maximum:
        raise EvidenceError("invalid unsigned measurement")
    return int(text)


def fields(line: str) -> dict[str, str]:
    result = {}
    for item in line.split():
        if "=" not in item:
            raise EvidenceError("malformed measurement field")
        key, value = item.split("=", 1)
        if key in result or not key or not value:
            raise EvidenceError("duplicate or empty measurement field")
        result[key] = value
    return result


def analyze(raw: bytes, kind: str) -> dict:
    if len(raw) > MAX_CAPTURE:
        raise EvidenceError("capture exceeds 1 MiB")
    text = raw.decode("utf-8", errors="replace")
    lines = text.splitlines()
    if any(len(line) > MAX_LINE for line in lines):
        raise EvidenceError("console line exceeds 2048 characters")
    if kind == "app":
        events = {}
        for line in lines:
            marker = "[OTA_MEASURE] "
            if marker not in line:
                continue
            row = fields(line.split(marker, 1)[1])
            if set(row) - {"phase", "t_us", "state", "stack_hwm_bytes"}:
                raise EvidenceError("unknown application measurement field")
            phase = row.get("phase")
            state = row.get("state")
            if phase not in PHASES or state not in {"ok", "failed", "unavailable"}:
                raise EvidenceError("unknown phase or measurement state")
            if phase in events:
                raise EvidenceError("duplicate phase: mixed boot/restart evidence")
            if not events and phase != "main_enter":
                raise EvidenceError("capture missed the application session start")
            stamp = uint(row.get("t_us", ""))
            stack = row.get("stack_hwm_bytes")
            if stack is not None and (phase not in {"init_stack", "opcua_stack"} or state != "ok"):
                raise EvidenceError("invalid stack watermark state")
            if phase in {"init_stack", "opcua_stack"} and state == "ok" and stack is None:
                raise EvidenceError("missing measured stack watermark")
            events[phase] = {"t_us": stamp, "state": state,
                             "stack_hwm_bytes": uint(stack) if stack is not None else None}
        main = events.get("main_enter")
        if main and any(row["t_us"] < main["t_us"] for row in events.values()):
            raise EvidenceError("device timestamp precedes main entry")
        for earlier, later in (("main_enter", "scheduler_running"),
                               ("scheduler_running", "config_init"),
                               ("server_startup", "server_iterate")):
            if earlier in events and later in events and events[earlier]["t_us"] > events[later]["t_us"]:
                raise EvidenceError("invalid startup milestone ordering")
        missing = sorted(REQUIRED - events.keys())
        failed = sorted(name for name, row in events.items() if row["state"] == "failed")
        unavailable = sorted(name for name in REQUIRED & events.keys()
                             if events[name]["state"] != "ok")
        status = "failed" if failed else "unavailable" if missing or unavailable else "pass"
        elapsed = None
        if main and main["state"] == "ok" and events.get("server_iterate", {}).get("state") == "ok":
            elapsed = events["server_iterate"]["t_us"] - main["t_us"]
        return {"kind": kind, "status": status, "events": events, "missing": missing,
                "failed": failed, "startup_to_first_iterate_us": elapsed,
                "ipv4_observed": "ipv4_ready" in events,
                "health_confirmation_tested": False,
                "time_origin": "device main entry, not power-on or bootloader jump"}
    if kind != "ram":
        raise EvidenceError("unsupported evidence kind")
    starts = [index for index, line in enumerate(lines) if line.strip().startswith("[CRYPTO_PROBE] start ")]
    terminals = [index for index, line in enumerate(lines)
                 if line.strip() in {"[CRYPTO_PROBE] PASS", "[CRYPTO_PROBE] FAIL"}]
    if len(starts) != 1 or len(terminals) != 1 or starts[0] >= terminals[0]:
        raise EvidenceError("capture requires one complete RAM-probe session")
    start, terminal = starts[0], terminals[0]
    if any("[CRYPTO_PROBE]" in line or "[FLASH_PROBE]" in line
           for line in lines[:start] + lines[terminal + 1:]):
        raise EvidenceError("mixed or stale RAM-probe observations")
    start_fields = fields(lines[start].strip().split("[CRYPTO_PROBE] start ", 1)[1])
    if uint(start_fields.get("clock_hz", "")) == 0 or start_fields.get("flash_write_enabled") not in {"0", "1"}:
        raise EvidenceError("invalid RAM-probe start metadata")
    tests = {}
    stack = None
    flash_mode = None
    flash_status = None
    flash_tests = {}
    for line in lines[start + 1:terminal]:
        if "[CRYPTO_PROBE] test=" in line:
            row = fields(line.split("[CRYPTO_PROBE] ", 1)[1])
            name = row.get("test")
            if name not in CRYPTO_TESTS | WRITE_GUARD_TESTS or name in tests:
                raise EvidenceError("unknown/duplicate crypto test")
            uint(row.get("elapsed_us", ""))
            for key in ("arena_bytes", "peak_allocator_used_excl_headers", "peak_header_count",
                        "current_used", "current_header_count"):
                uint(row.get(key, ""))
            if row["current_used"] != "0" or row["current_header_count"] != "0":
                raise EvidenceError("live crypto allocations remain")
            if row.get("clean") != "1":
                raise EvidenceError("allocator cleanup did not pass")
            result = row.get("result", "")
            if not re.fullmatch(r"-?[0-9]{1,10}", result):
                raise EvidenceError("invalid native result")
            rejection = name.endswith("_rejected")
            if (int(result) == 0) == rejection:
                raise EvidenceError("unexpected crypto test result")
            tests[name] = row
        elif "[CRYPTO_PROBE] stack " in line:
            if stack is not None:
                raise EvidenceError("duplicate stack observation")
            stack = fields(line.split("[CRYPTO_PROBE] stack ", 1)[1])
            if stack.get("state") not in {"ok", "unavailable", "saturated", "canary_failed"}:
                raise EvidenceError("invalid MSP stack state")
            reserved = uint(stack.get("reserved_bytes", ""))
            if stack["state"] == "ok":
                used = uint(stack.get("observed_used_bytes", ""))
                if used == 0 or used > reserved:
                    raise EvidenceError("invalid measured MSP stack extent")
            elif "observed_used_bytes" in stack:
                raise EvidenceError("unavailable stack represented as a number")
        elif "[FLASH_PROBE] mode=" in line:
            if flash_mode is not None:
                raise EvidenceError("duplicate Flash mode")
            flash_mode = fields(line.split("[FLASH_PROBE] ", 1)[1]).get("mode")
        elif "[FLASH_PROBE] status=" in line:
            if flash_status is not None:
                raise EvidenceError("duplicate Flash outcome")
            flash_status = fields(line.split("[FLASH_PROBE] ", 1)[1]).get("status")
        elif "[FLASH_PROBE] test=" in line:
            row = fields(line.split("[FLASH_PROBE] ", 1)[1])
            name = row.get("test")
            if name not in {"erase_initial", "program_pages", "readback", "erase_restore"} or name in flash_tests:
                raise EvidenceError("unknown/duplicate Flash operation")
            if row.get("state") == "unavailable":
                if "elapsed_us" in row or row.get("result") != "1":
                    raise EvidenceError("skipped Flash operation represented as measured")
            else:
                uint(row.get("elapsed_us", ""))
            if row.get("result") not in {"0", "1"}:
                raise EvidenceError("invalid Flash operation result")
            flash_tests[name] = row
    terminal_passes = [index for index, line in enumerate(lines) if line.strip() == "[CRYPTO_PROBE] PASS"]
    passed = len(terminal_passes) == 1 and not any(line.strip() == "[CRYPTO_PROBE] FAIL" for line in lines)
    if flash_mode not in {"read_only", "write_test"}:
        raise EvidenceError("missing or invalid Flash mode")
    if (flash_mode == "write_test") != (start_fields["flash_write_enabled"] == "1"):
        raise EvidenceError("inconsistent probe session mode")
    if flash_mode == "read_only" and WRITE_GUARD_TESTS & tests.keys():
        raise EvidenceError("unexpected destructive-probe observations in read-only evidence")
    required_tests = CRYPTO_TESTS | WRITE_GUARD_TESTS if flash_mode == "write_test" else CRYPTO_TESTS
    missing = sorted(required_tests - tests.keys())
    if flash_status != "pass":
        passed = False
    if flash_mode == "write_test" and flash_status == "pass":
        if set(flash_tests) != {"erase_initial", "program_pages", "readback", "erase_restore"}:
            raise EvidenceError("missing Flash timing observations")
        if any(row["result"] != "0" for row in flash_tests.values()):
            raise EvidenceError("Flash operation failed despite final pass")
    if stack is not None and stack.get("state") in {"saturated", "canary_failed"}:
        passed = False
    complete = passed and not missing and stack is not None and stack.get("state") == "ok"
    return {"kind": kind, "status": "pass" if complete else "failed" if not passed else "unavailable",
            "tests": tests, "missing": missing, "stack": stack, "flash_mode": flash_mode,
            "flash_status": flash_status, "flash_tests": flash_tests,
            "baseline_return_observed": any(
                re.search(r"Running FW=0\.6\.0(?:\s|$)", line)
                for line in lines[terminal + 1:]),
            "runtime_scope": "isolated RAM probe including harness/interrupts; not application task stacks"}


def elf_preflight(path: pathlib.Path, expected_sha256: str) -> dict:
    data = read_bounded(path, 32 * 1024 * 1024)
    digest = hashlib.sha256(data).hexdigest()
    if not re.fullmatch(r"[0-9a-f]{64}", expected_sha256) or digest != expected_sha256:
        raise EvidenceError("ELF does not match the pinned SHA-256")
    if len(data) < 52 or data[:6] != b"\x7fELF\x01\x01":
        raise EvidenceError("expected a little-endian ELF32")
    etype, machine = struct.unpack_from("<HH", data, 16)
    entry, phoff = struct.unpack_from("<II", data, 24)
    phsize, phcount = struct.unpack_from("<HH", data, 42)
    if etype != 2 or machine != 40 or not entry & 1 or phsize != 32 or not 0 < phcount <= 32:
        raise EvidenceError("unsupported RAM executable")
    if phoff < 52 or phoff + phsize * phcount > len(data):
        raise EvidenceError("invalid ELF program table")
    loads = []
    executable_entry = False
    for index in range(phcount):
        typ, offset, virtual, physical, filesz, memsz, flags, _align = struct.unpack_from(
            "<8I", data, phoff + index * phsize)
        if typ != 1:
            continue
        if (virtual != physical or filesz > memsz or offset + filesz > len(data) or
                not 0x20000000 <= virtual <= 0x20082000 or
                not 0x20000000 <= physical <= 0x20082000 or
                virtual + memsz > 0x20082000 or physical + memsz > 0x20082000):
            raise EvidenceError("refusing a non-SRAM or malformed ELF load range")
        if memsz and any(row["memory_bytes"] and virtual < row["virtual"] + row["memory_bytes"]
                         and row["virtual"] < virtual + memsz for row in loads):
            raise EvidenceError("overlapping RAM load ranges")
        executable_entry |= bool(flags & 1 and virtual <= (entry & ~1) < virtual + filesz)
        loads.append({"virtual": virtual, "physical": physical, "file_offset": offset,
                      "file_bytes": filesz, "memory_bytes": memsz})
    if not loads or not executable_entry:
        raise EvidenceError("RAM entry point is not in an executable load range")
    return {"elf_sha256": digest, "elf_bytes": len(data), "entry": entry, "ram_only": True, "loads": loads}


def probe_variant(data: bytes, loads: list[dict]) -> str:
    """Read the reviewed probe's allocated four-byte variant object, not DWARF."""
    shoff = struct.unpack_from("<I", data, 32)[0]
    shsize, shcount = struct.unpack_from("<HH", data, 46)
    if shsize != 40 or not 0 < shcount <= 4096 or shoff + shsize * shcount > len(data):
        raise EvidenceError("missing bounded probe section metadata")
    sections = [struct.unpack_from("<10I", data, shoff + i * shsize) for i in range(shcount)]
    found = []
    for section in sections:
        _name, typ, _flags, _address, offset, size, link, _info, _align, entsize = section
        if typ != 2:
            continue
        if (entsize != 16 or size % entsize or size // entsize > 65536 or
                offset + size > len(data) or link >= shcount):
            raise EvidenceError("invalid probe symbol table")
        strings = sections[link]
        if strings[1] != 3 or strings[4] + strings[5] > len(data):
            raise EvidenceError("invalid symbol name table")
        names = data[strings[4]:strings[4] + strings[5]]
        for pos in range(offset, offset + size, entsize):
            name, value, obj_size, info, _other, target = struct.unpack_from("<IIIBBH", data, pos)
            if name >= len(names):
                raise EvidenceError("invalid probe symbol name offset")
            if names[name:name + 30] != b"ota_probe_flash_write_enabled\0":
                continue
            if info != 0x11 or obj_size != 4 or target >= shcount:
                raise EvidenceError("invalid probe variant symbol")
            allocated = sections[target]
            delta = value - allocated[3]
            file_pos = allocated[4] + delta
            if (allocated[1] != 1 or not allocated[2] & 2 or delta < 0 or
                    delta + 4 > allocated[5] or file_pos + 4 > len(data) or
                    not any(row["virtual"] <= value and value + 4 <= row["virtual"] + row["file_bytes"]
                            and row["file_offset"] + value - row["virtual"] == file_pos for row in loads)):
                raise EvidenceError("variant symbol is not allocated loaded data")
            found.append(struct.unpack_from("<I", data, file_pos)[0])
    if len(found) != 1 or found[0] not in {0, 1}:
        raise EvidenceError("missing/ambiguous probe variant value")
    write = bool(found[0])
    if (b"DESTRUCTIVE_TEST" in data) != write:
        raise EvidenceError("probe variant and destructive marker disagree")
    return "write_test" if write else "read_only"


def backup_preflight(data: bytes) -> dict:
    if len(data) != FLASH_BYTES:
        raise EvidenceError("backup is not the complete 16-MiB device")
    if hashlib.sha256(data[:BASELINE_BYTES]).hexdigest() != BASELINE_SHA256:
        raise EvidenceError("device is not the pinned legacy baseline")
    record = data[STATE_OFFSET:STATE_OFFSET + 64]
    magic, version, state = struct.unpack_from("<IHH", record)
    crc = struct.unpack_from("<I", record, 60)[0]
    if (magic != 0x3141544F or version != 1 or state != 0 or
            crc != zlib.crc32(record[:60]) or any(record[8:60])):
        raise EvidenceError("Flash experiment requires a valid canonical empty IDLE record")
    if data[TEST_OFFSET:TAIL_OFFSET] != b"\xff" * 4096:
        raise EvidenceError("legacy staging test sector is occupied")
    return {"full_flash_sha256": hashlib.sha256(data).hexdigest(),
            "preserved_tail_sha256": hashlib.sha256(data[TAIL_OFFSET:]).hexdigest(),
            "baseline_sha256": BASELINE_SHA256, "canonical_idle": True,
            "test_offset": TEST_OFFSET, "test_bytes": 4096, "test_sector_erased": True}


def capture(port: str, duration: float) -> bytes:
    import serial
    from serial.tools import list_ports
    raw = bytearray()
    deadline = time.monotonic() + duration
    while time.monotonic() < deadline:
        matching = any(item.device.casefold() == port.casefold() and
                       (item.serial_number or "").casefold() == BOARD_SERIAL.casefold()
                       for item in list_ports.comports())
        if not matching:
            time.sleep(0.1)
            continue
        try:
            with serial.Serial(port, baudrate=115200, timeout=0.1) as stream:
                while time.monotonic() < deadline:
                    chunk = stream.read(min(max(stream.in_waiting, 1), 4096))
                    if len(raw) + len(chunk) > MAX_CAPTURE:
                        raise EvidenceError("serial capture exceeded 1 MiB")
                    raw.extend(chunk)
        except (serial.SerialException, OSError):
            time.sleep(0.1)
    return bytes(raw)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    for command in ("parse", "capture"):
        child = sub.add_parser(command)
        child.add_argument("--kind", choices=("app", "ram"), required=True)
        child.add_argument("--output", type=pathlib.Path, required=True)
        if command == "parse":
            child.add_argument("--input", type=pathlib.Path, required=True)
        else:
            child.add_argument("--port", default="COM6")
            child.add_argument("--seconds", type=float, default=90)
    for command in ("preflight", "run-ram"):
        child = sub.add_parser(command)
        child.add_argument("--elf", type=pathlib.Path, required=True)
        child.add_argument("--sha256", required=True)
        child.add_argument("--output", type=pathlib.Path, required=True)
        if command == "run-ram":
            child.add_argument("--picotool", type=pathlib.Path, required=True)
            child.add_argument("--allow-flash-write", action="store_true")
            child.add_argument("--port", default="COM6")
            child.add_argument("--seconds", type=float, default=90)
    args = parser.parse_args()
    if hasattr(args, "seconds") and not 1 <= args.seconds <= 300:
        parser.error("capture duration must be 1 through 300 seconds")
    execution_attempted = False
    try:
        if args.output.suffix != ".json" or args.output.exists():
            raise EvidenceError("use a new .json evidence output path")
        if any(args.output.with_suffix(suffix).exists() for suffix in (".serial.log", ".backup.json")):
            raise EvidenceError("refusing to overwrite existing evidence")
        if args.command in {"parse", "capture"}:
            raw = read_bounded(args.input, MAX_CAPTURE) if args.command == "parse" else capture(args.port, args.seconds)
            if args.command == "capture":
                args.output.with_suffix(".serial.log").write_bytes(raw)
            result = analyze(raw, args.kind)
        else:
            result = elf_preflight(args.elf, args.sha256)
            # The write-only object has a fixed marker retained in its console
            # output. Both ELF selection and write execution must be intentional.
            write_variant = b"DESTRUCTIVE_TEST" in read_bounded(args.elf, 32 * 1024 * 1024)
            result["flash_write_variant"] = write_variant
            result["execution_approved"] = result["elf_sha256"] in APPROVED_RAM_ELFS
            if result["execution_approved"]:
                result["probe_mode"] = probe_variant(read_bounded(args.elf, 32 * 1024 * 1024), result["loads"])
                if result["probe_mode"] != APPROVED_RAM_ELFS[result["elf_sha256"]]:
                    raise EvidenceError("approved probe variant metadata differs")
            if args.command == "run-ram":
                if write_variant != args.allow_flash_write:
                    raise EvidenceError("RAM variant and explicit Flash-write selection differ")
                approved_mode = APPROVED_RAM_ELFS.get(result["elf_sha256"])
                if approved_mode is None:
                    raise EvidenceError("ELF is not an approved prepared measurement artifact")
                directory = args.output.parent
                if not directory.is_dir():
                    raise EvidenceError("create a dedicated local evidence directory first")
                frozen = directory / "ram-probe.elf"
                with frozen.open("xb") as stream:
                    stream.write(read_bounded(args.elf, 32 * 1024 * 1024))
                elf_preflight(frozen, args.sha256)
                mode = probe_variant(read_bounded(frozen, 32 * 1024 * 1024), result["loads"])
                if mode != approved_mode:
                    raise EvidenceError("probe symbol and approved artifact mode differ")
                write_variant = mode == "write_test"
                if write_variant != args.allow_flash_write:
                    raise EvidenceError("frozen RAM variant and explicit Flash-write selection differ")
                tool = str(args.picotool.resolve())
                selection = ["--ser", BOARD_SERIAL]
                subprocess.run([tool, "info", "-a", *selection], check=True, timeout=15,
                               capture_output=True)
                if write_variant:
                    backup = directory / "flash-before-measurement.bin"
                    if backup.exists():
                        raise EvidenceError("refusing to overwrite an existing recovery backup")
                    subprocess.run([tool, "save", "-r", "0x10000000", "0x11000000", "-v",
                                    str(backup.resolve()), *selection], check=True, timeout=180,
                                   capture_output=True)
                    result["verified_fresh_backup"] = backup_preflight(read_bounded(backup, FLASH_BYTES))
                    # Save recovery identity before any execution, even if the
                    # probe or host is subsequently interrupted.
                    args.output.with_suffix(".backup.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
                execution_attempted = True
                subprocess.run([tool, "load", "-v", "-x", str(frozen.resolve()), *selection],
                               check=True, timeout=30, capture_output=True)
                raw = capture(args.port, args.seconds)
                args.output.with_suffix(".serial.log").write_bytes(raw)
                observation = analyze(raw, "ram")
                if (observation["flash_mode"] == "write_test") != write_variant:
                    raise EvidenceError("observed Flash mode does not match the selected build")
                if not observation["baseline_return_observed"]:
                    observation["status"] = "failed"
                result["observation"] = observation
                result["status"] = observation["status"]
            else:
                result["status"] = "pass"
        args.output.write_text(json.dumps(result, indent=2), encoding="utf-8")
        print(json.dumps({"status": result["status"], "output": str(args.output)}))
        return {"pass": 0, "failed": 1, "unavailable": 2}[result["status"]]
    except (EvidenceError, OSError, subprocess.SubprocessError, ImportError) as error:
        if args.output.suffix == ".json" and not args.output.exists() and args.output.parent.is_dir():
            args.output.write_text(json.dumps({"status": "failed", "reason": str(error),
                                              "execution_attempted": execution_attempted}, indent=2), encoding="utf-8")
        print(f"measurement: FAILED: {error}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
