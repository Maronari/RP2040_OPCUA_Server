#!/usr/bin/env python3
"""Verify the RP2350 exact-semantics FreeRTOS HWM scan optimization."""

from __future__ import annotations

import pathlib
import subprocess
import tempfile


def main() -> int:
    root = pathlib.Path(__file__).resolve().parents[2]
    source = (root / "libraries" / "FreeRTOS-Kernel" / "tasks.c").read_text(
        encoding="utf-8"
    )
    start = source.index("static configSTACK_DEPTH_TYPE prvTaskCheckFreeStackSpace")
    end = source.index("#endif /* ( ( configUSE_TRACE_FACILITY", start)
    function = source[start:end]
    assert "defined( PICO_RP2350 )" in function
    assert "portSTACK_GROWTH == -1" in function
    assert "const StackType_t * pxStackWord" in function
    assert "( StackType_t ) tskSTACK_FILL_BYTE" in function
    assert "UINT8_MAX" in function
    assert "pxStackWord[ 3 ] ^ xFillWord" in function
    assert "pxStackLastWord - pxStackWord >= 3" in function
    assert "pxStackWord <= pxStackLastWord" in function
    assert "|" in function
    assert "pxStackWord += 4" in function
    assert "*pxStackWord == xFillWord" in function
    assert "portable byte loop below" in function
    assert "while( *pucStackByte == ( uint8_t ) tskSTACK_FILL_BYTE )" in function

    test = root / "static" / "tests" / "freertos_stack_hwm" / "stack_hwm_test.c"
    with tempfile.TemporaryDirectory(prefix="freertos-stack-hwm-") as directory:
        executable = pathlib.Path(directory) / "stack_hwm_test.exe"
        subprocess.run(
            ["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", str(test),
             "-o", str(executable)],
            check=True,
        )
        subprocess.run([str(executable)], check=True)
    print("FreeRTOS RP2350 HWM scan suite: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
