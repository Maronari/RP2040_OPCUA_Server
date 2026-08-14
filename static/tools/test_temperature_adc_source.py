#!/usr/bin/env python3
"""Fail-closing source contract for package-correct internal-temperature ADC."""

from __future__ import annotations

import pathlib
import re


def main() -> int:
    root = pathlib.Path(__file__).resolve().parents[2]
    source = (root / "drivers" / "src" / "temp_sensor_driver.c").read_text(encoding="utf-8")
    sdk = (root / "libraries" / "pico-sdk" / "src" / "rp2_common" / "hardware_adc" /
           "include" / "hardware" / "adc.h").read_text(encoding="utf-8")
    board = (root / "boards" / "include" / "boards" /
             "waveshare_core2350b_16mb_psram8mb.h").read_text(encoding="utf-8")

    assert "adc_select_input(ADC_TEMPERATURE_CHANNEL_NUM);" in source
    assert not re.search(r"adc_select_input\(\s*4[uU]?\s*\)", source)
    assert "#define ADC_TEMPERATURE_CHANNEL_NUM (NUM_ADC_CHANNELS - 1)" in sdk
    assert "#define PICO_RP2350A 0" in board
    print("temperature-adc-source: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
