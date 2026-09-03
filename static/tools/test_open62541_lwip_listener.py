#!/usr/bin/env python3
"""Regression check for bounded LWIP accept retry behavior."""

from __future__ import annotations

import pathlib
import re


ROOT = pathlib.Path(__file__).resolve().parents[2]


def function_body(source: str, signature: str) -> str:
    """Extract one function's body by brace-matching, so an ordering check
    below can't be fooled by the same call names appearing in a doc comment
    that precedes the function (a real recurrence: `main()`'s own doc
    comment documents its call sequence in prose, and a naive whole-file
    substring search matches that prose before the real, later call)."""
    start = source.index(signature)
    body_start = source.index("{", start)
    depth = 0
    for index in range(body_start, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start:index + 1]
    raise SystemExit(f"lwip-listener: FAIL: unterminated function: {signature}")


def main() -> int:
    source = (ROOT / "port/open62541/open62541.c").read_text(encoding="utf-8")
    lwipopts = (ROOT / "port/lwip/lwipopts.h").read_text(encoding="utf-8")
    heap_source = (ROOT / "port/lwip/lwip_psram_heap.c").read_text(encoding="utf-8")
    app_main = (ROOT / "app/basic_profile/main.c").read_text(encoding="utf-8")
    model_limits = (ROOT / "opcua_model/src/opcua_model_internal.h").read_text(encoding="utf-8")
    if "#define LWIP_OPCUA_SECURE_SEND_MIN   16384u" not in lwipopts:
        raise SystemExit(
            "lwip-listener: FAIL: lwIP heap does not reserve the 16 KiB OPC UA secure-send bound"
        )
    if "#define MEM_SIZE                    LWIP_OPCUA_SECURE_SEND_MIN" not in lwipopts:
        raise SystemExit(
            "lwip-listener: FAIL: MEM_SIZE is detached from the OPC UA secure-send bound"
        )
    if "#define LWIP_RAM_HEAP_POINTER       g_lwip_ram_heap" not in lwipopts:
        raise SystemExit(
            "lwip-listener: FAIL: bounded lwIP arena is not redirected out of internal SRAM"
        )
    if "psram_calloc(1u, LWIP_PSRAM_HEAP_ALLOCATION_BYTES)" not in heap_source:
        raise SystemExit(
            "lwip-listener: FAIL: bounded lwIP PSRAM allocation is missing"
        )
    main_body = function_body(app_main, "int main(void)")
    psram_init = main_body.find("app_init_psram_allocator();")
    heap_init = main_body.find("lwip_psram_heap_init()")
    task_start = main_body.find("xTaskCreate(app_init_task")
    if psram_init < 0 or heap_init <= psram_init or task_start <= heap_init:
        raise SystemExit(
            "lwip-listener: FAIL: lwIP arena is not reserved after PSRAM and before network tasks"
        )
    if "#define OPCUA_SEND_BUFFER_SIZE 16384u" not in model_limits:
        raise SystemExit(
            "lwip-listener: FAIL: lwIP secure-send reservation no longer matches the OPC UA send buffer"
        )
    if "#define TCP_SND_BUF                 LWIP_OPCUA_SECURE_SEND_MIN" not in lwipopts or \
            "#define LWIP_OPCUA_CONFIG_EXPORT_SEND_MIN  LWIP_OPCUA_SECURE_SEND_MIN" not in lwipopts or \
            "#if TCP_SND_BUF < LWIP_OPCUA_CONFIG_EXPORT_SEND_MIN" not in lwipopts:
        raise SystemExit(
            "lwip-listener: FAIL: TCP send-window bound for ExportConfig is not enforced"
        )
    if "#define MEMP_NUM_TCP_SEG            48" not in lwipopts:
        raise SystemExit(
            "lwip-listener: FAIL: TCP segment pool is smaller than the 16 KiB send queue"
        )
    match = re.search(
        r"TCP_listenSocketCallback\([^\{]+\{(?P<body>.*?)"
        r"/\* Log the name of the remote host \*/",
        source,
        flags=re.DOTALL,
    )
    if match is None:
        raise SystemExit("lwip-listener: FAIL: listen callback not found")
    body = match.group("body")
    if "UA_ERRNO == 0" not in body:
        raise SystemExit(
            "lwip-listener: FAIL: errno-less lwIP accept failure closes listener"
        )
    for transient_error in (
        "UA_INTERRUPTED",
        "UA_WOULDBLOCK",
        "UA_AGAIN",
        "ENFILE",
        "ENOMEM",
        "ENOBUFS",
        "ECONNABORTED",
        "ECONNRESET",
    ):
        if transient_error not in body:
            raise SystemExit(
                f"lwip-listener: FAIL: accept closes listener on {transient_error}"
            )
    retry = re.search(
        r"if\(UA_ERRNO == 0\s*\|\|\s*"
        r"UA_ERRNO == UA_INTERRUPTED\s*\|\|\s*"
        r"UA_ERRNO == UA_WOULDBLOCK\s*\|\|\s*"
        r"UA_ERRNO == UA_AGAIN\s*\|\|\s*"
        r"UA_ERRNO == ENFILE\s*\|\|\s*"
        r"UA_ERRNO == ENOMEM\s*\|\|\s*"
        r"UA_ERRNO == ENOBUFS\s*\|\|\s*"
        r"UA_ERRNO == ECONNABORTED\s*\|\|\s*"
        r"UA_ERRNO == ECONNRESET\)\s*return;",
        body,
    )
    if retry is None:
        raise SystemExit(
            "lwip-listener: FAIL: transient accept errors are not one retry branch"
        )
    if body.find("TCP_shutdown(cm, conn)") < retry.end():
        raise SystemExit(
            "lwip-listener: FAIL: listener shutdown precedes transient retry"
        )
    print("open62541 LWIP listener transient/capacity-accept regression: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
