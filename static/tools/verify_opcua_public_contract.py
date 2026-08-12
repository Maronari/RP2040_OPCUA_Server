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


EXPECTED_V040_COMMIT = "8a454945aaccc3d381a2e889e90b53569415b705"
# This is deliberately an ordered, closed manifest.  The contract inventory is
# source-derived, so a glob would let a new UA registration evade comparison.
MODEL_SOURCE_SET = (
    "opcua_model/src/opcua_model_internal.h",
    "opcua_model/src/model_common.c",
    "opcua_model/src/model_access.c",
    "opcua_model/src/opcua_model_context.c",
    "opcua_model/src/model_channels.c",
    "opcua_model/src/model_pubsub.c",
    "opcua_model/src/model_events.c",
    "opcua_model/src/model_maintenance.c",
    "opcua_model/src/model_history.c",
    "opcua_model/src/model_configuration.c",
    "opcua_model/src/model_security.c",
    "opcua_model/src/model_ota.c",
    "opcua_model/src/model_diagnostics.c",
    "opcua_model/src/model_identity_di_amb.c",
    "opcua_model/src/model_address_space.c",
    "opcua_model/src/model_core_lifecycle.c",
)
BYTE_STABLE_PUBLIC_CONTRACT_PATHS = (
    "opcua_model/include/opcua_model/opcua_model.h",
    "services/include/services/config_export_service.h",
    "services/include/services/config_import_service.h",
    "services/include/services/config_reset_service.h",
    "services/include/services/config_store_service.h",
    "services/include/services/security_service.h",
    "services/include/services/maintenance_service.h",
    "services/src/config_import_service.c",
    "services/src/config_reset_service.c",
    "services/src/config_store_service.c",
    "services/src/security_service.c",
    "services/src/maintenance_service.c",
    "port/open62541/nodesetinjector.c",
    "port/open62541/include/nodesetinjector.h",
    "port/open62541/include/open62541config.h",
    "static/tools/export_nodeset.bat",
    "static/tools/verify_maintenance_workflow.py",
)
# v0.4.1 deliberately changes bounded channel/configuration runtime internals.
# Record those and the existing runtime-facing tools as evidence while the
# source-derived OPC UA inventory and public headers remain fail-closed.
REVIEWED_INTERNAL_DRIFT_PATHS = (
    "services/include/services/channel_config.h",
    "services/include/services/channel_manager.h",
    "services/include/services/config_service.h",
    "services/include/services/config_validate_service.h",
    "services/include/services/driver_catalog.h",
    "services/include/services/sensor_manager.h",
    "services/include/services/telemetry_channel.h",
    "services/include/services/telemetry_driver.h",
    "services/src/channel_manager.c",
    "services/src/config_export_service.c",
    "services/src/config_service.c",
    "services/src/config_validate_service.c",
    "services/src/driver_catalog.c",
    "static/tools/export_nodeset.py",
    "static/tools/verify_amb_model.py",
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
PUBLIC_REGISTRATION_PREFIX = re.compile(
    r"\bUA_Server_(?:add|set|write|create|delete)[A-Za-z0-9_]*\s*\("
)
NONCONTRACT_DIAGNOSTIC_FUNCTIONS = (
    "model_ua_logger_log",
    "model_monitored_item_register_callback",
    "model_server_notification_callback",
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", default="v0.4.0", help="Immutable Git baseline")
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
    data = data.replace(b"\r\n", b"\n")
    # Internal linkage is deliberately changed by the translation-unit split;
    # it is not an OPC UA contract. Normalize it before the legacy lexical
    # inventory is built (whose comment/string lexer otherwise spans a char
    # literal into the next function declaration).
    return re.sub(
        br"(?m)^static (?=(?:bool|void|UA_[A-Za-z0-9_]+|const char \*|"
        br"int64_t|uint32_t|uint8_t|size_t|model_[A-Za-z0-9_]+|"
        br"security_[A-Za-z0-9_]+)\b[^;\n]*\()",
        b"",
        data,
    )


def aggregate_current_source(source_dir: pathlib.Path) -> bytes:
    """Read the refactored model in its fixed public-registration order."""
    chunks: list[bytes] = []
    for path in MODEL_SOURCE_SET:
        data = normalized_source((source_dir / path).read_bytes())
        # Module-local private-header includes are build plumbing, not source
        # inventory. The baseline has the declarations inline in one file.
        data = data.replace(b'#include "opcua_model_internal.h"\n', b'')
        chunks.append(data)
    return b"".join(chunks)


def aggregate_baseline_source(source_dir: pathlib.Path, baseline: str) -> bytes:
    """Read the immutable model in the same reviewed registration order."""
    chunks: list[bytes] = []
    for path in MODEL_SOURCE_SET:
        data = normalized_source(baseline_bytes(source_dir, baseline, path))
        data = data.replace(b'#include "opcua_model_internal.h"\n', b'')
        chunks.append(data)
    return b"".join(chunks)


def assert_closed_registration_coverage(source_dir: pathlib.Path) -> None:
    """Reject UA registration code outside the reviewed module manifest."""
    allowed = set(MODEL_SOURCE_SET)
    for path in sorted((source_dir / "opcua_model" / "src").glob("*")):
        if path.suffix not in {".c", ".h"}:
            continue
        relative = path.relative_to(source_dir).as_posix()
        text = strip_comments(path.read_text(encoding="utf-8"))
        if relative not in allowed and PUBLIC_REGISTRATION_PREFIX.search(text):
            raise SystemExit(
                "opcua-contract: FAIL: un-inventoried public UA registration: "
                f"{relative}"
            )


def strip_comments(text: str) -> str:
    pattern = re.compile(r'("(?:\\.|[^"\\])*")|//[^\n]*|/\*.*?\*/', re.S)
    return pattern.sub(lambda match: match.group(1) or " ", text)


def strip_noncontract_diagnostic_bodies(text: str) -> str:
    """Remove internal logging callbacks from the public-node inventory.

    These callbacks cannot register nodes or alter the wire contract. Keeping
    their printf strings in the lexical inventory made safe logging changes
    look like public NodeId/BrowseName drift.
    """
    for name in NONCONTRACT_DIAGNOSTIC_FUNCTIONS:
        search_from = 0
        pattern = re.compile(r"\b" + re.escape(name) + r"\s*\([^;{}]*\)\s*\{")
        while match := pattern.search(text, search_from):
            opening = text.find("{", match.start())
            depth = 0
            closing = None
            for index in range(opening, len(text)):
                if text[index] == "{":
                    depth += 1
                elif text[index] == "}":
                    depth -= 1
                    if depth == 0:
                        closing = index + 1
                        break
            if closing is None:
                raise SystemExit(
                    f"opcua-contract: FAIL: unterminated diagnostic function {name}"
                )
            text = text[:opening + 1] + " " + text[closing - 1:]
            search_from = opening + 2
    return text


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
    text = strip_noncontract_diagnostic_bodies(text)
    # Module order is intentionally no longer the legacy source-file order.
    # Literal multiplicity remains a fail-closed compatibility surface, while
    # lexical placement is not a UA contract.
    strings = sorted(re.findall(r'"(?:\\.|[^"\\])*"', text))
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
    if args.baseline == "v0.4.0" and baseline_commit != EXPECTED_V040_COMMIT:
        raise SystemExit(
            f"opcua-contract: FAIL: v0.4.0 resolves to {baseline_commit}, "
            f"expected {EXPECTED_V040_COMMIT}"
        )

    stable_files: list[dict[str, str]] = []
    for relative in BYTE_STABLE_PUBLIC_CONTRACT_PATHS:
        baseline_data = normalized_source(baseline_bytes(source_dir, args.baseline, relative))
        current_data = normalized_source((source_dir / relative).read_bytes())
        if current_data != baseline_data:
            raise SystemExit(f"opcua-contract: FAIL: contract file drift: {relative}")
        stable_files.append({"path": relative, "sha256": sha256(current_data)})

    reviewed_internal_drift: list[dict[str, str]] = []
    for relative in REVIEWED_INTERNAL_DRIFT_PATHS:
        baseline_data = normalized_source(baseline_bytes(source_dir, args.baseline, relative))
        current_data = normalized_source((source_dir / relative).read_bytes())
        reviewed_internal_drift.append({
            "path": relative,
            "baseline_sha256": sha256(baseline_data),
            "current_sha256": sha256(current_data),
        })

    baseline_inventory = public_inventory(aggregate_baseline_source(source_dir, args.baseline))
    assert_closed_registration_coverage(source_dir)
    current_inventory = public_inventory(aggregate_current_source(source_dir))
    if current_inventory != baseline_inventory:
        raise SystemExit("opcua-contract: FAIL: public model inventory differs from baseline")

    evidence = {
        "baseline": args.baseline,
        "baseline_commit": baseline_commit,
        "baseline_model_source_set": MODEL_SOURCE_SET,
        "model_source_set": MODEL_SOURCE_SET,
        "public_inventory_sha256": current_inventory["sha256"],
        "string_literal_count": len(current_inventory["string_literals"]),
        "node_and_browse_call_count": len(current_inventory["node_and_browse_calls"]),
        "public_attribute_assignment_count": len(
            current_inventory["public_attribute_assignments"]
        ),
        "byte_stable_public_contract_files": stable_files,
        "reviewed_internal_drift": reviewed_internal_drift,
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
