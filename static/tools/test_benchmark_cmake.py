#!/usr/bin/env python3
"""Exercise the real CMake T4 selector and generated export without the SDK."""
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

import benchmark_profiles as profiles

ROOT = Path(__file__).resolve().parents[2]
CMAKE = shutil.which("cmake") or "C:/msys64/mingw64/bin/cmake.EXE"


class CMakeTests(unittest.TestCase):
    def run_selector(self, settings, output):
        script = output.parent / "test.cmake"
        script.write_text(f'include("{ROOT.as_posix()}/cmake/benchmark_profile.cmake")\n'
                          'if(DEFINED RP2_RESOURCE_EXHAUSTION_PROFILE_ID)\n'
                          f'  file(WRITE "{output.as_posix()}" "${{RP2_RESOURCE_EXHAUSTION_PROFILE_ID}}")\n'
                          'else()\n'
                          f'  file(WRITE "{output.as_posix()}" "${{RP2_BENCHMARK_PROFILE_ID}}")\n'
                          'endif()\n',
                          encoding="utf-8")
        defaults = {"FIRMWARE_DRIVER_PACK": "base",
                    "PICO_BOARD": "waveshare_core2350b_16mb_psram8mb", "WIZNET_CHIP": "W5500"}
        return subprocess.run([CMAKE, *(f"-D{k}={v}" for k, v in (defaults | settings).items()),
                               "-P", str(script)], capture_output=True, text=True)

    def test_all_profile_ids_and_exports(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "id.txt"
            export = Path(directory) / "config.json"
            for profile in profiles.profiles():
                result = self.run_selector(profiles.cmake_settings(profile), output)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(output.read_text(), profile["profileId"])
                result = subprocess.run([CMAKE, f"-DOUTPUT_PATH={export}", "-DFW_REVISION=0.5.2",
                                         "-DBUILD_TIME=test", "-DGIT_HASH=test", "-DHW_REVISION=test",
                                         "-DBENCHMARK_PROFILE=synthetic",
                                         f"-DBENCHMARK_CHANNELS={profile['channels']}",
                                         f"-DBENCHMARK_RATE_HZ={profile['rateHz']}",
                                         f"-DBENCHMARK_EVENTS={int(profile['events'])}",
                                         f"-DBENCHMARK_HISTORY={int(profile['history'])}",
                                         "-P", str(ROOT / "app/basic_profile/generate_config_export.cmake")],
                                        capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                profiles.verify_export(profile, json.loads(export.read_text()))

    def test_rejections_and_default(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "id.txt"
            self.assertEqual(self.run_selector({}, output).returncode, 0)
            self.assertEqual(output.read_text(), "")
            base = {"FIRMWARE_BENCHMARK_PROFILE": "synthetic"}
            for bad in ({"FIRMWARE_BENCHMARK_CHANNELS": "17"},
                        {"FIRMWARE_BENCHMARK_RATE_HZ": "2"},
                        {"FIRMWARE_BENCHMARK_CLIENTS": "5"},
                        {"FIRMWARE_BENCHMARK_SECURITY": "none"},
                        {"FIRMWARE_DRIVER_PACK": "industrial"},
                        {"OTA_BUILD_BOOTLOADER": "ON"},
                        {"W5X00_DIAGNOSTIC_NO_OPCUA": "ON"},
                        {"FIRMWARE_BENCHMARK_PROFILE": "off", "FIRMWARE_BENCHMARK_SECURITY": "test-none"}):
                self.assertNotEqual(self.run_selector(base | bad, output).returncode, 0, bad)

    def test_resource_exhaustion_profile_is_reference_only(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "id.txt"
            resource = {"FIRMWARE_BENCHMARK_PROFILE": "resource-exhaustion"}
            result = self.run_selector(resource, output)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(output.read_text(), "v052-resource-exhaustion-reference")
            for bad in ({"FIRMWARE_DRIVER_PACK": "industrial"},
                        {"OTA_BUILD_BOOTLOADER": "ON"},
                        {"W5X00_DIAGNOSTIC_NO_OPCUA": "ON"},
                        {"FIRMWARE_BENCHMARK_SECURITY": "test-none"}):
                self.assertNotEqual(self.run_selector(resource | bad, output).returncode, 0, bad)


if __name__ == "__main__":
    unittest.main()
