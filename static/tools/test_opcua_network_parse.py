#!/usr/bin/env python3
"""Compile the exact legacy OPC UA network-parser implementations on the host."""

from __future__ import annotations

import argparse
import hashlib
import pathlib
import subprocess
import tempfile


MARKER = "/* MODEL_PARSE_IMPLEMENTATIONS */"
FUNCTION_SIGNATURES = (
    "static bool model_parse_ipv4_string(const char *text, uint8_t out_ipv4[4])",
    "static bool model_parse_mac_string(const char *text, uint8_t out_mac[6])",
)
# Updating either implementation requires consciously reviewing this extracted-source test.
EXPECTED_SHA256 = "57da5773e4afbd07b94f76862877ee573bf861e8c981340c6208a655c5a0560e"


def extract_function(source: str, signature: str) -> str:
    """Return one complete C function, rejecting ambiguous or malformed source."""
    if source.count(signature) != 1:
        raise RuntimeError(f"expected exactly one parser signature: {signature}")
    start = source.index(signature)
    brace = source.find("{", start)
    if brace < 0:
        raise RuntimeError(f"function has no opening brace: {signature}")

    depth = 0
    index = brace
    state = "code"
    while index < len(source):
        char = source[index]
        following = source[index + 1] if index + 1 < len(source) else ""
        if state == "code":
            if char == '"':
                state = "string"
            elif char == "'":
                state = "char"
            elif char == "/" and following == "/":
                state = "line-comment"
                index += 1
            elif char == "/" and following == "*":
                state = "block-comment"
                index += 1
            elif char == "{":
                depth += 1
            elif char == "}":
                depth -= 1
                if depth == 0:
                    return source[start:index + 1]
                if depth < 0:
                    break
        elif state in ("string", "char"):
            if char == "\\":
                index += 1
            elif (state == "string" and char == '"') or (state == "char" and char == "'"):
                state = "code"
        elif state == "line-comment":
            if char == "\n":
                state = "code"
        elif state == "block-comment" and char == "*" and following == "/":
            state = "code"
            index += 1
        index += 1
    raise RuntimeError(f"unbalanced braces while extracting: {signature}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", default="gcc", help="Host C compiler")
    args = parser.parse_args()
    root = pathlib.Path(__file__).resolve().parents[2]
    source_path = root / "opcua_model" / "src" / "opcua_model.c"
    template_path = root / "static" / "tests" / "opcua_network_parse" / "opcua_network_parse_test.c"
    source = source_path.read_text(encoding="utf-8")
    template = template_path.read_text(encoding="utf-8")
    if template.count(MARKER) != 1:
        raise RuntimeError(f"expected exactly one extraction marker in {template_path}")

    implementations = "\n\n".join(extract_function(source, signature) for signature in FUNCTION_SIGNATURES)
    digest = hashlib.sha256(implementations.encode("utf-8")).hexdigest()
    if digest != EXPECTED_SHA256:
        raise RuntimeError(
            "opcua_model network parser source changed; review and update "
            f"EXPECTED_SHA256 (got {digest})"
        )
    harness = template.replace(MARKER, implementations)

    with tempfile.TemporaryDirectory(prefix="opcua-network-parse-") as directory:
        directory_path = pathlib.Path(directory)
        harness_path = directory_path / "opcua_network_parse_test.c"
        executable = directory_path / "opcua_network_parse_test.exe"
        harness_path.write_text(harness, encoding="utf-8", newline="\n")
        subprocess.run([
            args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
            str(harness_path), "-o", str(executable),
        ], check=True)
        subprocess.run([str(executable)], check=True)

    print("OPC UA network parser extracted-source host suite: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
