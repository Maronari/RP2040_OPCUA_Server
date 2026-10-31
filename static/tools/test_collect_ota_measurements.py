#!/usr/bin/env python3
"""Offline safety/negative tests for T0 capture and RAM preflight."""
from __future__ import annotations

import contextlib
import hashlib
import importlib.util
import io
import json
import pathlib
import struct
import sys
import tempfile
import types
import unittest
import zlib
from unittest import mock

SPEC = importlib.util.spec_from_file_location("ota_measurements", pathlib.Path(__file__).with_name("collect_ota_measurements.py"))
M = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(M)


def elf_bytes(*, origin=0x20000000, entry=0x20000001, filesz=8, memsz=16, extra=b""):
    data = bytearray(104)
    data[:6] = b"\x7fELF\x01\x01"
    struct.pack_into("<HH", data, 16, 2, 40)
    struct.pack_into("<II", data, 24, entry, 52)
    struct.pack_into("<HH", data, 42, 32, 1)
    struct.pack_into("<8I", data, 52, 1, 96, origin, origin, filesz, memsz, 5, 4)
    return bytes(data) + extra


def app_log():
    phases = ["main_enter", "scheduler_running", "flash_coordinator", "config_init",
              "network_init", "history_init", "server_startup", "server_iterate",
              "init_stack", "opcua_stack"]
    return "\n".join(f"[OTA_MEASURE] phase={name} t_us={index * 100} state=ok" +
                     (" stack_hwm_bytes=1024" if name.endswith("_stack") else "")
                     for index, name in enumerate(phases)).encode()


def probe_elf_bytes(write=False):
    data = bytearray(elf_bytes())
    data.extend(bytes(416 - len(data)))
    struct.pack_into("<I", data, 32, 256)
    struct.pack_into("<HH", data, 46, 40, 4)
    struct.pack_into("<I", data, 96, int(write))
    names = b"\0ota_probe_flash_write_enabled\0"
    data[128:128 + len(names)] = names
    struct.pack_into("<IIIBBH", data, 208, 1, 0x20000000, 4, 0x11, 0, 1)
    struct.pack_into("<10I", data, 256 + 40, 0, 1, 3, 0x20000000, 96, 8, 0, 0, 4, 0)
    struct.pack_into("<10I", data, 256 + 80, 0, 3, 0, 0, 128, len(names), 0, 0, 1, 0)
    struct.pack_into("<10I", data, 256 + 120, 0, 2, 0, 0, 192, 32, 2, 1, 4, 16)
    return bytes(data) + (b"DESTRUCTIVE_TEST" if write else b"")


def ram_log(write=False):
    lines = [f"[CRYPTO_PROBE] start clock_hz=150000000 flash_write_enabled={int(write)}",
             "[FLASH_PROBE] mode=" + ("write_test" if write else "read_only")]
    for name in sorted(M.CRYPTO_TESTS | (M.WRITE_GUARD_TESTS if write else set())):
        result = -16 if name.endswith("_rejected") else 0
        lines.append(f"[CRYPTO_PROBE] test={name} elapsed_us=100 arena_bytes=65536 "
                     f"peak_allocator_used_excl_headers=3420 peak_header_count=62 "
                     f"current_used=0 current_header_count=0 result={result} clean=1")
    lines.append("[CRYPTO_PROBE] stack state=ok observed_used_bytes=1536 reserved_bytes=4096")
    if write:
        for name in ("erase_initial", "program_pages", "readback", "erase_restore"):
            lines.append(f"[FLASH_PROBE] test={name} elapsed_us=100 result=0")
        lines.append("[FLASH_PROBE] status=pass")
    else:
        lines.append("[FLASH_PROBE] status=pass")
    lines.extend(("[CRYPTO_PROBE] PASS", "[APP] Running FW=0.6.0 Build=test"))
    return "\n".join(lines).encode()


class MeasurementTests(unittest.TestCase):
    def test_app_success_uses_device_time(self):
        result = M.analyze(app_log(), "app")
        self.assertEqual("pass", result["status"])
        self.assertEqual(700, result["startup_to_first_iterate_us"])
        self.assertFalse(result["health_confirmation_tested"])

    def test_no_dhcp_is_unavailable_not_zero_or_pass(self):
        result = M.analyze(b"[OTA_MEASURE] phase=main_enter t_us=100 state=ok\n"
                           b"[OTA_MEASURE] phase=waiting_ipv4 t_us=200 state=unavailable", "app")
        self.assertEqual("unavailable", result["status"])
        self.assertIsNone(result["startup_to_first_iterate_us"])

    def test_failed_initialization_is_failure(self):
        self.assertEqual("failed", M.analyze(app_log().replace(
            b"config_init t_us=300 state=ok", b"config_init t_us=300 state=failed"), "app")["status"])

    def test_reject_mixed_boot_duplicate_bad_time_and_unknown(self):
        bad = (app_log() + b"\n[OTA_MEASURE] phase=main_enter t_us=0 state=ok",
               b"[OTA_MEASURE] phase=main_enter t_us=-1 state=ok",
               b"[OTA_MEASURE] phase=typo t_us=1 state=ok",
               b"[OTA_MEASURE] phase=main_enter t_us=18446744073709551616 state=ok",
               b"[OTA_MEASURE] phase=main_enter t_us=1 t_us=2 state=ok",
               b"[OTA_MEASURE] phase=opcua_stack t_us=1 state=ok")
        for raw in bad:
            with self.subTest(raw=raw[-90:]), self.assertRaises(M.EvidenceError):
                M.analyze(raw, "app")

    def test_bounds(self):
        for raw in (b"x" * (M.MAX_CAPTURE + 1), b"x" * (M.MAX_LINE + 1)):
            with self.assertRaises(M.EvidenceError):
                M.analyze(raw, "app")

    def test_ram_positive_read_and_write(self):
        for write in (False, True):
            result = M.analyze(ram_log(write), "ram")
            self.assertEqual("pass", result["status"])
            self.assertTrue(result["baseline_return_observed"])

    def test_ram_reject_incomplete_negative_and_false_cleanup(self):
        for raw in (ram_log().replace(b"current_used=0", b"current_used=1", 1),
                    ram_log().replace(b"result=-16", b"result=0", 1),
                    ram_log(True).replace(b"test=readback", b"test=unknown"),
                    ram_log().replace(b"elapsed_us=100", b"elapsed_us=-1", 1)):
            with self.assertRaises(M.EvidenceError):
                M.analyze(raw, "ram")
        with self.assertRaises(M.EvidenceError):
            M.analyze(ram_log().replace(b"[CRYPTO_PROBE] PASS", b"[CRYPTO_PROBE] PASS_NOT"), "ram")
        incomplete = b"\n".join(line for line in ram_log().splitlines() if b"test=tail_after " not in line)
        self.assertEqual("unavailable", M.analyze(incomplete, "ram")["status"])

    def test_stack_missing_is_not_zero(self):
        raw = ram_log().replace(b"state=ok observed_used_bytes=1536", b"state=unavailable")
        result = M.analyze(raw, "ram")
        self.assertEqual("unavailable", result["status"])
        self.assertNotIn("observed_used_bytes", result["stack"])
        for state in (b"saturated", b"canary_failed"):
            changed = ram_log().replace(b"state=ok observed_used_bytes=1536", b"state=" + state)
            self.assertEqual("failed", M.analyze(changed, "ram")["status"])

    def test_baseline_must_follow_final_pass(self):
        raw = b"[APP] Running FW=0.6.0\n" + ram_log().split(b"\n[APP]")[0]
        self.assertFalse(M.analyze(raw, "ram")["baseline_return_observed"])

    def test_stale_or_multiple_ram_sessions_rejected(self):
        for raw in (ram_log() + b"\n" + ram_log(),
                    b"[CRYPTO_PROBE] test=tail_before elapsed_us=1\n" + ram_log(),
                    ram_log() + b"\n[FLASH_PROBE] mode=read_only"):
            with self.assertRaises(M.EvidenceError):
                M.analyze(raw, "ram")

    def test_elf_ram_only_and_hash_pinned(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp) / "probe.elf"
            data = elf_bytes()
            path.write_bytes(data)
            self.assertTrue(M.elf_preflight(path, hashlib.sha256(data).hexdigest())["ram_only"])
            mixed = bytearray(data)
            struct.pack_into("<I", mixed, 52 + 12, 0x10000000)
            for invalid in (elf_bytes(origin=0x10000000), elf_bytes(entry=0x10000001),
                            elf_bytes(entry=0x20000000), mixed,
                            elf_bytes(memsz=1), elf_bytes(origin=0x20081FF8), b"bad"):
                path.write_bytes(invalid)
                with self.assertRaises(M.EvidenceError):
                    M.elf_preflight(path, hashlib.sha256(invalid).hexdigest())
            path.write_bytes(data)
            with self.assertRaises(M.EvidenceError):
                M.elf_preflight(path, "0" * 64)

    def test_backup_requires_full_pinned_idle_and_blank(self):
        data = bytearray(b"\xff" * M.FLASH_BYTES)
        struct.pack_into("<IHH", data, M.STATE_OFFSET, 0x3141544F, 1, 0)
        data[M.STATE_OFFSET + 8:M.STATE_OFFSET + 60] = bytes(52)
        struct.pack_into("<I", data, M.STATE_OFFSET + 60, zlib.crc32(data[M.STATE_OFFSET:M.STATE_OFFSET + 60]))
        # The synthetic image is explicitly pinned only inside this unit test.
        with mock.patch.object(M, "BASELINE_SHA256", hashlib.sha256(data[:M.BASELINE_BYTES]).hexdigest()):
            self.assertTrue(M.backup_preflight(data)["canonical_idle"])
            data[M.TEST_OFFSET] = 0
            with self.assertRaises(M.EvidenceError):
                M.backup_preflight(data)
            data[M.TEST_OFFSET] = 255
            data[M.STATE_OFFSET + 6] = 1
            with self.assertRaises(M.EvidenceError):
                M.backup_preflight(data)
        with self.assertRaises(M.EvidenceError):
            M.backup_preflight(bytes(64))

    def test_variant_must_be_unique_allocated_loaded_value(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp) / "probe.elf"
            for write in (False, True):
                data = probe_elf_bytes(write)
                path.write_bytes(data)
                meta = M.elf_preflight(path, hashlib.sha256(data).hexdigest())
                self.assertEqual("write_test" if write else "read_only", M.probe_variant(data, meta["loads"]))
                bad = bytearray(data)
                struct.pack_into("<I", bad, 96, 2)
                with self.assertRaises(M.EvidenceError):
                    M.probe_variant(bad, meta["loads"])
                bad = bytearray(data)
                struct.pack_into("<I", bad, 256 + 40 + 8, 0)  # no SHF_ALLOC
                with self.assertRaises(M.EvidenceError):
                    M.probe_variant(bad, meta["loads"])

    def test_write_selection_refused_before_usb(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            path = root / "probe.elf"
            data = probe_elf_bytes(True)
            path.write_bytes(data)
            digest = hashlib.sha256(data).hexdigest()
            self.assertNotIn(digest, M.APPROVED_RAM_ELFS)
            self.assertEqual("write_test", M.probe_variant(data, M.elf_preflight(path, digest)["loads"]))
            for allow_write in (False, True):
                output = root / f"result-{allow_write}.json"
                argv = ["tool", "run-ram", "--elf", str(path), "--sha256", hashlib.sha256(data).hexdigest(),
                        "--picotool", "unused", "--output", str(output)]
                if allow_write:
                    argv.append("--allow-flash-write")
                with mock.patch.object(sys, "argv", argv), mock.patch.object(M.subprocess, "run") as run:
                    with contextlib.redirect_stdout(io.StringIO()):
                        self.assertEqual(1, M.main())
                    run.assert_not_called()
                self.assertFalse(json.loads(output.read_text())["execution_attempted"])

    def test_existing_output_preserved(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            out = root / "result.json"
            out.write_text("keep")
            with mock.patch.object(sys, "argv", ["tool", "parse", "--kind", "app", "--input", "unused", "--output", str(out)]):
                with contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(1, M.main())
            self.assertEqual("keep", out.read_text())

    def test_readonly_launcher_never_requests_backup_or_flash_load(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            path = root / "input.elf"
            data = probe_elf_bytes()
            path.write_bytes(data)
            argv = ["tool", "run-ram", "--elf", str(path), "--sha256", hashlib.sha256(data).hexdigest(),
                    "--picotool", "unused", "--output", str(root / "result.json")]
            with mock.patch.object(sys, "argv", argv), mock.patch.object(M.subprocess, "run") as run, \
                    mock.patch.dict(M.APPROVED_RAM_ELFS, {hashlib.sha256(data).hexdigest(): "read_only"}):
                with mock.patch.object(M, "capture", return_value=ram_log()), contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(0, M.main())
            self.assertEqual(2, run.call_count)
            load = run.call_args_list[1].args[0]
            self.assertEqual(["load", "-v", "-x"], load[1:4])
            self.assertEqual(str((root / "ram-probe.elf").resolve()), load[4])
            self.assertFalse((root / "flash-before-measurement.bin").exists())

    def test_write_launcher_refuses_nonidle_fresh_backup_before_execution(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            path = root / "input.elf"
            data = probe_elf_bytes(True)
            path.write_bytes(data)
            backup = b"\xff" * M.FLASH_BYTES
            argv = ["tool", "run-ram", "--elf", str(path), "--sha256", hashlib.sha256(data).hexdigest(),
                    "--picotool", "unused", "--allow-flash-write", "--output", str(root / "result.json")]

            def fake_tool(command, **_kwargs):
                if command[1] == "save":
                    self.assertIn("-v", command)
                    (root / "flash-before-measurement.bin").write_bytes(backup)

            with mock.patch.object(sys, "argv", argv), mock.patch.object(M.subprocess, "run", side_effect=fake_tool) as run, \
                    mock.patch.dict(M.APPROVED_RAM_ELFS, {hashlib.sha256(data).hexdigest(): "write_test"}):
                with mock.patch.object(M, "BASELINE_SHA256", hashlib.sha256(backup[:M.BASELINE_BYTES]).hexdigest()):
                    with contextlib.redirect_stdout(io.StringIO()):
                        self.assertEqual(1, M.main())
                self.assertEqual(["info", "save"], [call.args[0][1] for call in run.call_args_list])
            self.assertFalse(json.loads((root / "result.json").read_text())["execution_attempted"])
            self.assertEqual(M.FLASH_BYTES, (root / "flash-before-measurement.bin").stat().st_size)

    def test_capture_does_not_open_another_boards_port(self):
        serial = types.ModuleType("serial")
        serial.Serial = mock.Mock()
        serial.SerialException = OSError
        tools = types.ModuleType("serial.tools")
        tools.list_ports = types.SimpleNamespace(comports=lambda: [
            types.SimpleNamespace(device="COM6", serial_number="OTHER_BOARD")])
        with mock.patch.dict(sys.modules, {"serial": serial, "serial.tools": tools}), \
                mock.patch.object(M.time, "monotonic", side_effect=[0, 0, 2]), \
                mock.patch.object(M.time, "sleep"):
            self.assertEqual(b"", M.capture("COM6", 1))
        serial.Serial.assert_not_called()


if __name__ == "__main__":
    unittest.main()
