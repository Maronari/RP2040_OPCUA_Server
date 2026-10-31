#!/usr/bin/env python3
"""Check opt-in measurement restrictions and compile-out logging semantics."""
from __future__ import annotations

import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]


class MeasurementProfileTests(unittest.TestCase):
    def test_cmake_restricts_profile_and_defaults_off(self):
        cmake = shutil.which("cmake")
        self.assertIsNotNone(cmake, "cmake is required")
        valid = {"CMAKE_BUILD_TYPE": "Debug", "FIRMWARE_DRIVER_PACK": "base",
                 "PICO_BOARD": "waveshare_core2350b_16mb_psram8mb",
                 "FIRMWARE_BENCHMARK_PROFILE": "off",
                 "FIRMWARE_BENCHMARK_SECURITY": "basic256sha256-signandencrypt",
                 "W5X00_DIAGNOSTIC_NO_OPCUA": "OFF", "OTA_BUILD_BOOTLOADER": "OFF"}
        cases = [(False, {}, True), (True, valid, True)]
        for key, value in (("CMAKE_BUILD_TYPE", "Release"), ("FIRMWARE_DRIVER_PACK", "industrial"),
                           ("PICO_BOARD", "wrong_board"), ("FIRMWARE_BENCHMARK_PROFILE", "synthetic"),
                           ("FIRMWARE_BENCHMARK_SECURITY", "none"),
                           ("W5X00_DIAGNOSTIC_NO_OPCUA", "ON"), ("OTA_BUILD_BOOTLOADER", "ON")):
            cases.append((True, {**valid, key: value}, False))
        with tempfile.TemporaryDirectory() as tmp:
            script = pathlib.Path(tmp) / "profile.cmake"
            for enabled, values, expected in cases:
                with self.subTest(enabled=enabled, values=values):
                    text = "cmake_minimum_required(VERSION 3.20)\n"
                    text += "\n".join(f'set({key} "{value}")' for key, value in values.items())
                    if enabled:
                        text += "\nset(FIRMWARE_OTA_MEASUREMENT ON)"
                    text += f'\ninclude("{(ROOT / "cmake/ota_measurement.cmake").as_posix()}")\n'
                    script.write_text(text)
                    result = subprocess.run([cmake, "-P", str(script)], capture_output=True, text=True)
                    self.assertEqual(expected, result.returncode == 0, result.stdout + result.stderr)

    def test_disabled_macros_do_not_evaluate_arguments(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = pathlib.Path(tmp)
            source = directory / "disabled.c"
            exe = directory / "disabled.exe"
            source.write_text('#include "services/ota_measurement.h"\n'
                              'int main(void) {\n'
                              ' OTA_MEASUREMENT_EVENT(does_not_exist(), invalid_identifier);\n'
                              ' OTA_MEASUREMENT_EVENT_AT(bad, bad, bad);\n'
                              ' OTA_MEASUREMENT_STACK_EVENT(bad, bad);\n return 0; }\n')
            subprocess.run(["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            f"-I{ROOT / 'services/include'}", str(source), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)

    def test_enabled_preserves_observation_time_and_stack_units(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = pathlib.Path(tmp)
            (directory / "pico").mkdir()
            (directory / "pico/time.h").write_text('#include <stdint.h>\n'
                                                  'static inline uint64_t time_us_64(void) { return 900; }\n')
            (directory / "pico/stdio.h").write_text('static inline void stdio_flush(void) {}\n')
            source = directory / "enabled.c"
            exe = directory / "enabled.exe"
            source.write_text('#include "services/ota_measurement.h"\n'
                              'int main(void) {\n'
                              ' OTA_MEASUREMENT_EVENT_AT("main_enter", "ok", 25);\n'
                              ' OTA_MEASUREMENT_STACK_EVENT("init_stack", 1024);\n return 0; }\n')
            subprocess.run(["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-DFIRMWARE_OTA_MEASUREMENT=1", f"-I{directory}",
                            f"-I{ROOT / 'services/include'}", str(source), "-o", str(exe)], check=True)
            result = subprocess.run([str(exe)], capture_output=True, text=True, check=True)
            self.assertEqual(["[OTA_MEASURE] phase=main_enter t_us=25 state=ok",
                              "[OTA_MEASURE] phase=init_stack t_us=900 state=ok stack_hwm_bytes=1024"],
                             result.stdout.splitlines())


if __name__ == "__main__":
    unittest.main()
