#!/usr/bin/env python3
"""Regression check for bounded LWIP accept retry behavior."""

from __future__ import annotations

import pathlib
import re


ROOT = pathlib.Path(__file__).resolve().parents[2]


def main() -> int:
    source = (ROOT / "port/open62541/open62541.c").read_text(encoding="utf-8")
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
