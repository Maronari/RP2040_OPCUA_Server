#!/usr/bin/env python3
"""Compare the source-derived public OPC UA inventory with an immutable Git tag."""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import re
import subprocess
import sys


EXPECTED_V033_COMMIT = "19bf3d24e80e217f960b251654c323eacfcf1c0f"
MODEL_SOURCE = "opcua_model/src/opcua_model.c"
BYTE_STABLE_CONTRACT_PATHS = (
    "opcua_model/include/opcua_model/opcua_model.h",
    "app/basic_profile/main.c",
    "app/basic_profile/generate_config_export.cmake",
    "services/include/services/config_service.h",
    "services/include/services/config_wire.h",
    "services/include/services/config_export_service.h",
    "services/include/services/config_import_service.h",
    "services/include/services/config_reset_service.h",
    "services/include/services/config_store_service.h",
    "services/include/services/config_validate_service.h",
    "services/include/services/security_service.h",
    "services/include/services/maintenance_service.h",
    "services/src/config_service.c",
    "services/src/config_export_service.c",
    "services/src/config_import_service.c",
    "services/src/config_reset_service.c",
    "services/src/config_store_service.c",
    "services/src/config_validate_service.c",
    "services/src/security_service.c",
    "services/src/maintenance_service.c",
    "port/open62541/nodesetinjector.c",
    "port/open62541/include/nodesetinjector.h",
    "port/open62541/include/open62541config.h",
    "static/tools/export_nodeset.py",
    "static/tools/export_nodeset.bat",
    "static/tools/verify_amb_model.py",
    "static/tools/verify_maintenance_workflow.py",
)
PUBLIC_CALL_PREFIXES = (
    "UA_NODEID_",
    "UA_EXPANDEDNODEID_",
    "UA_QUALIFIEDNAME",
    "UA_LOCALIZEDTEXT",
)
PUBLIC_ATTRIBUTE_FIELDS = (
    "accessLevel",
    "arrayDimensionsSize",
    "dataType",
    "eventNotifier",
    "executable",
    "historizing",
    "userAccessLevel",
    "userExecutable",
    "valueRank",
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", default="v0.3.3", help="Immutable Git baseline")
    parser.add_argument(
        "--source-dir",
        type=pathlib.Path,
        default=pathlib.Path(__file__).resolve().parents[2],
    )
    parser.add_argument("--output", type=pathlib.Path)
    return parser.parse_args()


def git(source_dir: pathlib.Path, *args: str) -> bytes:
    result = subprocess.run(
        ["git", *args], cwd=source_dir, capture_output=True, check=False
    )
    if result.returncode != 0:
        raise SystemExit(
            f"opcua-contract: git {' '.join(args)} failed: "
            f"{result.stderr.decode(errors='replace').strip()}"
        )
    return result.stdout


def baseline_bytes(source_dir: pathlib.Path, baseline: str, path: str) -> bytes:
    return git(source_dir, "show", f"{baseline}:{path}")


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def normalized_source(data: bytes) -> bytes:
    return data.replace(b"\r\n", b"\n")


def strip_comments(text: str) -> str:
    pattern = re.compile(r'("(?:\\.|[^"\\])*")|//[^\n]*|/\*.*?\*/', re.S)
    return pattern.sub(lambda match: match.group(1) or " ", text)


def balanced_calls(text: str) -> list[str]:
    calls: list[str] = []
    identifier = re.compile(r"\b(?:" + "|".join(PUBLIC_CALL_PREFIXES) + r")[A-Z0-9_]*\s*\(")
    for match in identifier.finditer(text):
        depth = 0
        in_string = False
        escaped = False
        end = match.end() - 1
        for index in range(end, len(text)):
            char = text[index]
            if in_string:
                if escaped:
                    escaped = False
                elif char == "\\":
                    escaped = True
                elif char == '"':
                    in_string = False
                continue
            if char == '"':
                in_string = True
            elif char == "(":
                depth += 1
            elif char == ")":
                depth -= 1
                if depth == 0:
                    calls.append(re.sub(r"\s+", " ", text[match.start():index + 1]).strip())
                    break
    return calls


def public_inventory(source: bytes) -> dict[str, object]:
    text = strip_comments(source.decode("utf-8"))
    strings = re.findall(r'"(?:\\.|[^"\\])*"', text)
    field_pattern = re.compile(
        r"\.(?:" + "|".join(PUBLIC_ATTRIBUTE_FIELDS) + r")\s*=\s*[^;]+;"
    )
    attributes = [re.sub(r"\s+", " ", item).strip() for item in field_pattern.findall(text)]
    inventory = {
        "string_literals": strings,
        "node_and_browse_calls": balanced_calls(text),
        "public_attribute_assignments": attributes,
    }
    canonical = json.dumps(inventory, sort_keys=True, separators=(",", ":")).encode()
    inventory["sha256"] = sha256(canonical)
    return inventory


def main() -> int:
    args = parse_args()
    source_dir = args.source_dir.resolve()
    baseline_commit = git(source_dir, "rev-parse", f"{args.baseline}^{{commit}}").decode().strip()
    if args.baseline == "v0.3.3" and baseline_commit != EXPECTED_V033_COMMIT:
        raise SystemExit(
            f"opcua-contract: FAIL: v0.3.3 resolves to {baseline_commit}, "
            f"expected {EXPECTED_V033_COMMIT}"
        )

    stable_files: list[dict[str, str]] = []
    for relative in BYTE_STABLE_CONTRACT_PATHS:
        baseline_data = normalized_source(baseline_bytes(source_dir, args.baseline, relative))
        current_data = normalized_source((source_dir / relative).read_bytes())
        if current_data != baseline_data:
            raise SystemExit(f"opcua-contract: FAIL: contract file drift: {relative}")
        stable_files.append({"path": relative, "sha256": sha256(current_data)})

    baseline_inventory = public_inventory(
        normalized_source(baseline_bytes(source_dir, args.baseline, MODEL_SOURCE))
    )
    current_inventory = public_inventory(
        normalized_source((source_dir / MODEL_SOURCE).read_bytes())
    )
    if current_inventory != baseline_inventory:
        raise SystemExit("opcua-contract: FAIL: public model inventory differs from baseline")

    evidence = {
        "baseline": args.baseline,
        "baseline_commit": baseline_commit,
        "model_source": MODEL_SOURCE,
        "public_inventory_sha256": current_inventory["sha256"],
        "string_literal_count": len(current_inventory["string_literals"]),
        "node_and_browse_call_count": len(current_inventory["node_and_browse_calls"]),
        "public_attribute_assignment_count": len(
            current_inventory["public_attribute_assignments"]
        ),
        "byte_stable_contract_files": stable_files,
    }
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(evidence, indent=2) + "\n", encoding="utf-8")

    print(
        "opcua-contract: PASS "
        f"({args.baseline}@{baseline_commit[:12]}, inventory {current_inventory['sha256']})"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
