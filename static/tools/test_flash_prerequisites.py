#!/usr/bin/env python3
"""Focused static verification for the v0.3.3 Flash prerequisites."""

from pathlib import Path
import json
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]


class FlashPrerequisiteTests(unittest.TestCase):
    def test_reference_layout_and_all_pairs(self):
        regions = [
            ("bootloader", 0x000000, 0x010000),
            ("bank_a", 0x010000, 0x7B4000),
            ("bank_b", 0x7C4000, 0x7B4000),
            ("maintenance_journal", 0xF78000, 0x008000),
            ("config_0", 0xF80000, 0x010000),
            ("config_1", 0xF90000, 0x010000),
            ("identity_0", 0xFA0000, 0x010000),
            ("identity_1", 0xFB0000, 0x010000),
            ("trust_0", 0xFC0000, 0x010000),
            ("trust_1", 0xFD0000, 0x010000),
            ("security_state", 0xFE0000, 0x010000),
            ("ota_state", 0xFF0000, 0x010000),
        ]
        for name, offset, size in regions:
            self.assertEqual(offset % 4096, 0, name)
            self.assertEqual(size % 4096, 0, name)
        for index, (left_name, left_offset, left_size) in enumerate(regions):
            for right_name, right_offset, right_size in regions[index + 1 :]:
                self.assertTrue(
                    left_offset + left_size <= right_offset
                    or right_offset + right_size <= left_offset,
                    f"{left_name} overlaps {right_name}",
                )
        self.assertEqual(regions[-1][1] + regions[-1][2], 0x1000000)
        generated = ROOT / "build/generated/ota_layout/ota_layout.json"
        if generated.exists():
            actual = json.loads(generated.read_text())["regions"]
            expected = {
                "bank_a": (0x010000, 0x7B4000),
                "bank_b": (0x7C4000, 0x7B4000),
                "maintenance_journal": (0xF78000, 0x8000),
            }
            for name, (offset, size) in expected.items():
                self.assertEqual(actual[name]["offset_bytes"], offset)
                self.assertEqual(actual[name]["size_bytes"], size)

    def test_generated_templates_publish_journal(self):
        for relative in ("ota/cmake/ota_layout.h.in", "ota/cmake/ota_layout.json.in"):
            self.assertIn("MAINTENANCE_JOURNAL", (ROOT / relative).read_text().upper())

    def test_legacy_driver_is_read_only(self):
        header = (ROOT / "drivers/include/drivers/flash_storage_driver.h").read_text()
        source = (ROOT / "drivers/src/flash_storage_driver.c").read_text()
        self.assertNotIn("flash_storage_driver_save", header + source)
        self.assertNotRegex(source, r"flash_range_(erase|program)|flash_safe_execute")
        self.assertIn("FLASH_STORAGE_MAGIC", source)
        self.assertIn("flash_storage_crc32", source)

    def test_flash_mutation_call_sites_are_audited(self):
        allowed = ROOT / "drivers/src/flash_operation_coordinator.c"
        bootloader = ROOT / "bootloader/main.c"
        offenders = []
        paths = list(ROOT.glob("drivers/src/*.c"))
        paths += list(ROOT.glob("services/src/*.c"))
        paths += list(ROOT.glob("ota/src/*.c"))
        paths += list(ROOT.glob("bootloader/*.c"))
        for path in paths:
            if path not in (allowed, bootloader) and re.search(
                r"flash_range_(erase|program)|flash_safe_execute", path.read_text()
            ):
                offenders.append(str(path.relative_to(ROOT)))
        self.assertEqual(offenders, [])
        bootloader_source = bootloader.read_text()
        self.assertRegex(bootloader_source, r"flash_safe_execute\s*\(")
        self.assertRegex(bootloader_source, r"__not_in_flash_func\s*\(")

    def test_bootloader_is_independent_of_freertos_coordinator(self):
        source = (ROOT / "bootloader/main.c").read_text()
        cmake = (ROOT / "bootloader/CMakeLists.txt").read_text()
        forbidden = (
            "flash_operation_coordinator",
            "xSemaphoreCreateMutexStatic",
            "xSemaphoreTake",
            "xTaskGetCurrentTaskHandle",
            "FREERTOS_FILES",
            "FLASH_OPERATION_COORDINATOR",
        )
        for token in forbidden:
            self.assertNotIn(token, source + cmake)
        self.assertIn("ota_flash_backend_erase", source)
        self.assertIn("ota_flash_backend_program", source)
        self.assertIn("flash_safe_execute", source)

    def test_ota_targets_select_distinct_backends(self):
        ota_cmake = (ROOT / "ota/CMakeLists.txt").read_text()
        app_cmake = (ROOT / "app/basic_profile/CMakeLists.txt").read_text()
        bootloader_cmake = (ROOT / "bootloader/CMakeLists.txt").read_text()
        application_backend = (
            ROOT / "ota/src/ota_flash_backend_application.c"
        ).read_text()

        ota_shared_links = re.search(
            r"target_link_libraries\(OTA_SHARED PUBLIC(?P<body>[\s\S]*?)\)",
            ota_cmake,
        )
        self.assertIsNotNone(ota_shared_links)
        self.assertNotIn("FLASH_OPERATION_COORDINATOR", ota_shared_links.group("body"))
        self.assertIn("OTA_FLASH_BACKEND_APPLICATION", app_cmake)
        self.assertNotIn("OTA_FLASH_BACKEND_APPLICATION", bootloader_cmake)
        self.assertIn("FLASH_OPERATION_COORDINATOR", ota_cmake)
        self.assertIn("drivers/flash_operation_coordinator.h", application_backend)
        self.assertIn("flash_operation_program_page", application_backend)
        self.assertIn("flash_operation_erase_sector", application_backend)

    def test_application_writers_keep_shared_serialization(self):
        writers = (
            ROOT / "ota/src/ota_flash_backend_application.c",
            ROOT / "services/src/config_store_service.c",
            ROOT / "services/src/security_service.c",
            ROOT / "services/src/security_state_service.c",
        )
        for path in writers:
            source = path.read_text()
            self.assertRegex(
                source,
                r"flash_operation_(erase_sector|program_page)",
                str(path.relative_to(ROOT)),
            )

    def test_coordinator_failure_and_nesting_policy(self):
        source = (ROOT / "drivers/src/flash_operation_coordinator.c").read_text()
        self.assertIn("xSemaphoreCreateMutexStatic", source)
        self.assertNotIn("xSemaphoreCreateRecursiveMutex", source)
        self.assertIn("FLASH_OPERATION_NESTED", source)
        self.assertIn("FLASH_OPERATION_TIMEOUT", source)
        self.assertRegex(source, r"flash_safe_execute[\s\S]+flash_operation_release\(\)")
        self.assertRegex(source, r"SAFE_EXEC_FAILED[\s\S]+flash_operation_release\(\)")

    def test_program_page_source_residency_contract(self):
        header = (
            ROOT / "drivers/include/drivers/flash_operation_coordinator.h"
        ).read_text()
        source = (ROOT / "drivers/src/flash_operation_coordinator.c").read_text()
        self.assertIn("internal SRAM", header)
        self.assertRegex(header, r"XIP\s+\*\s+Flash is unavailable")
        self.assertRegex(header, r"PSRAM is not\s+\*\s+part of this contract")
        self.assertIn("caller-owned residency invariant", header)
        self.assertRegex(
            header,
            r"rejecting executable-Flash addresses does not prove"
            r" that any other pointer\s+\*\s+is internal SRAM",
        )
        self.assertRegex(header, r"journal page buffers")
        self.assertIn("flash_operation_source_is_executable_flash", source)
        self.assertIn("__flash_binary_start", source)
        self.assertIn("__flash_binary_end", source)

    def test_static_async_context_has_bounded_stack(self):
        source = (ROOT / "drivers/src/w5x00_ethernet_driver.c").read_text()
        self.assertIn("g_w5x00_async_context_stack", source)
        self.assertIn("async_config.task_stack = g_w5x00_async_context_stack", source)
        self.assertIn("async_context_freertos_init(", source)


if __name__ == "__main__":
    unittest.main()
