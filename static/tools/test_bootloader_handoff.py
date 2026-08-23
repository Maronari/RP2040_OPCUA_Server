#!/usr/bin/env python3
"""Structural regression checks for the bootloader-to-Bank-A handoff."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "bootloader" / "main.c").read_text(encoding="utf-8")


def require(pattern: str, description: str) -> None:
    if re.search(pattern, SOURCE, re.DOTALL) is None:
        raise AssertionError(description)


def main() -> int:
    require(
        r"bootloader_quiesce_interrupt_sources\s*\([^)]*\)\s*\{"
        r".*nvic_hw->icer\[bank\]\s*=\s*UINT32_MAX"
        r".*reset_block\s*\(\s*RESETS_RESET_USBCTRL_BITS\s*\)"
        r".*nvic_hw->icpr\[bank\]\s*=\s*UINT32_MAX",
        "handoff must disable NVIC IRQs, quiesce USBCTRL, then clear pending IRQs",
    )
    require(
        r"bootloader_jump_to_bank_a\s*\([^)]*\)\s*\{"
        r".*bootloader_disable_interrupts\s*\(\s*\)\s*;"
        r"\s*bootloader_quiesce_interrupt_sources\s*\(\s*\)\s*;"
        r".*scb_hw->vtor\s*=\s*OTA_LAYOUT_BANK_A_ORIGIN"
        r".*bootloader_set_main_stack_pointer\s*\(\s*stack_pointer\s*\)"
        r".*\(\(bootloader_entry_fn_t\)reset_handler\)\s*\(\s*\)",
        "handoff ordering must keep IRQs masked and quiesced before VTOR/MSP transfer",
    )

    jump_body = re.search(
        r"bootloader_jump_to_bank_a\s*\([^)]*\)\s*\{(?P<body>.*?)\n\}",
        SOURCE,
        re.DOTALL,
    )
    if jump_body is None:
        raise AssertionError("bootloader_jump_to_bank_a body not found")
    if "multicore_reset_core1" in jump_body.group("body"):
        raise AssertionError("handoff must not perform the redundant unbounded core1 reset")
    if SOURCE.count("multicore_reset_core1();") != 1:
        raise AssertionError("bootloader must reset core1 exactly once during startup")
    if "cpsie i" in jump_body.group("body"):
        raise AssertionError("bootloader must not re-enable interrupts before application startup")

    print("bootloader handoff structural regression: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
