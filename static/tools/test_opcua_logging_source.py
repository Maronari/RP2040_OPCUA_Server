#!/usr/bin/env python3
"""Fail-closing source contract for production OPC UA teardown logging."""

from __future__ import annotations

import pathlib
import sys


ROOT = pathlib.Path(__file__).resolve().parents[2]
COMMON = ROOT / "opcua_model" / "src" / "model_common.c"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"opcua-logging-source: FAIL: {message}")


def function_body(source: str, signature: str) -> str:
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
    raise SystemExit(f"opcua-logging-source: FAIL: unterminated function: {signature}")


def main() -> int:
    source = COMMON.read_text(encoding="utf-8")
    logger = function_body(source, "void model_ua_logger_log(")
    monitored_item_callback = function_body(source, "void model_monitored_item_register_callback(")
    server_notification_callback = function_body(source, "void model_server_notification_callback(")

    require("if (level > UA_LOGLEVEL_WARNING)" in logger,
            "production logger does not filter INFO/DEBUG/TRACE traffic")
    require(logger.index("if (level > UA_LOGLEVEL_WARNING)") < logger.index("UA_String_vformat"),
            "logger formats INFO/DEBUG/TRACE messages before filtering")

    for callback_name, callback in (
        ("logger", logger),
        ("monitored-item callback", monitored_item_callback),
        ("server-notification callback", server_notification_callback),
    ):
        require("fflush(" not in callback and "stdio_flush(" not in callback,
                f"{callback_name} performs synchronous stdout flushing")

    for callback_name, callback in (
        ("monitored-item callback", monitored_item_callback),
        ("server-notification callback", server_notification_callback),
    ):
        require("printf(" not in callback and "ua_port_alloc_log_stats(" not in callback,
                f"{callback_name} emits high-volume teardown diagnostics")

    print("opcua-logging-source: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
