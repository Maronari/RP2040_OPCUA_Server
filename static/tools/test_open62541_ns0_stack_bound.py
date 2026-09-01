#!/usr/bin/env python3
"""Guard the bounded-stack construction of generated Namespace 0 node 78."""

from __future__ import annotations

import hashlib
import pathlib
import re
import sys


EXPECTED_REFERENCE_COUNT = 2345
EXPECTED_TARGET_ORDER_SHA256 = (
    "34ea97bf1230fd58b7aa92e8ddf6839a8dfb78b26425435e1f7b17a876c0642e"
)


def fail(message: str) -> int:
    print(f"[FAIL] {message}", file=sys.stderr)
    return 1


def main() -> int:
    root = pathlib.Path(__file__).resolve().parents[2]
    source = (root / "port" / "open62541" / "open62541.c").read_text(
        encoding="utf-8"
    )

    helper_name = "rp2_namespace0_addMandatoryReference"
    begin_marker = (
        "static UA_StatusCode function_namespace0_generated_4429_begin"
    )
    finish_marker = (
        "static UA_StatusCode function_namespace0_generated_4429_finish"
    )
    helper_start = source.find(f"{helper_name}(UA_Server *server")
    begin = source.find(begin_marker)
    finish = source.find(finish_marker, begin)
    if helper_start < 0 or begin < 0 or finish < 0:
        return fail("node 4429 helper/begin/finish markers are incomplete")

    helper = source[helper_start:begin]
    required_helper_fragments = (
        "UA_NODEID_NUMERIC(ns0, 78LU)",
        "UA_NODEID_NUMERIC(ns0, 37LU)",
        "UA_EXPANDEDNODEID_NUMERIC(ns0, targetId)",
        "false",
    )
    for fragment in required_helper_fragments:
        if fragment not in helper:
            return fail(f"bounded helper lost required semantics: {fragment}")

    generated = source[begin:finish]
    if "UA_Server_addReference(" in generated:
        return fail("node 4429 again constructs NodeId compound literals inline")

    targets = re.findall(
        rf"{helper_name}\(server, nsMapping, ([0-9]+)LU\);", generated
    )
    if len(targets) != EXPECTED_REFERENCE_COUNT:
        return fail(
            f"node 4429 reference count is {len(targets)}, "
            f"expected {EXPECTED_REFERENCE_COUNT}"
        )

    serialized = "".join(f"{target}\n" for target in targets).encode("ascii")
    digest = hashlib.sha256(serialized).hexdigest()
    if digest != EXPECTED_TARGET_ORDER_SHA256:
        return fail(
            "node 4429 target ID/order digest changed: "
            f"{digest} != {EXPECTED_TARGET_ORDER_SHA256}"
        )

    print(
        "open62541 Namespace 0 node 4429 bounded-stack/reference-order "
        "regression: PASS"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
