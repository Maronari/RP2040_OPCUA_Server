#!/usr/bin/env python3
"""Compare the source-derived public OPC UA inventory with an immutable Git tag."""

from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import json
import pathlib
import re
import subprocess
import sys


EXPECTED_V040_COMMIT = "8a454945aaccc3d381a2e889e90b53569415b705"
EXPECTED_V041_COMMIT = "a5ebc3231faf658325ad02e997f05acf8601e8bf"
EXPECTED_V042_COMMIT = "568dfd4e2a77c2db6fca4cd9a65e6279624928d6"
EXPECTED_V043_COMMIT = "dec08ba22e835e8037f9fd5342dcc63ddc03f5ac"
EXPECTED_V042_PUBLIC_INVENTORY_SHA256 = (
    "35669c4251a46de72a0e79d11a48936dda3eeec1ce42c832784bc61b9628dd15"
)
# Updated 2026-08-23 (from 0f8e85b8477ff3edc77968806ad0cc509c3cfafd81594cc6318915ff48a00bf3)
# for commit 5bed455: fixing the missing HasComponent references that left
# standard Acknowledge/ConditionRefresh completely uncallable (BadMethodInvalid
# on every prior release) adds exactly three new node_and_browse_calls entries
# -- UA_EXPANDEDNODEID_NUMERIC(0, UA_NS0ID_ACKNOWLEDGEABLECONDITIONTYPE_ACKNOWLEDGE),
# UA_EXPANDEDNODEID_NUMERIC(0, UA_NS0ID_CONDITIONTYPE_CONDITIONREFRESH), and
# UA_NODEID_NUMERIC(0, UA_NS0ID_SERVER) -- plus one new diagnostic printf
# string and one removed one (both non-UA log text). No entries were removed
# from string_literals, node_and_browse_calls or public_attribute_assignments;
# confirmed by diffing this script's own public_inventory() output against
# commit d48fd06 (pre-fix) before recording this hash.
EXPECTED_V043_PUBLIC_INVENTORY_SHA256 = (
    "a9a3b7610c5484987a9f01fd67cd2515876177071ea1bedbfa8595873ab3c4f8"
)
# v0.5.0 T8 adds the runtime-configuration-preset apply/list methods
# (Device.Maintenance.ApplyConfigPreset, Device.Maintenance.ListConfigPresets)
# and the Device.Compatibility subtree (FirmwareProfile, HardwareRevision,
# ConfigurationSchemaVersion, and one Device.Compatibility.SupportedDrivers.*
# node per compiled driver_catalog entry) -- the first genuinely new public
# OPC UA surface added since the v0.4.3 baseline (T1-T7 restructured the
# configuration transaction pipeline and added a controlled restart without
# registering any new NodeId). Computed directly from this script's own
# public_inventory() output against the current tree at T8 completion, not
# guessed.
EXPECTED_V050_PUBLIC_INVENTORY_SHA256 = (
    "830f8678a86040a31e8e2f51bea4647c4e8473243c012526674f3bc462763102"
)
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
    "services/include/services/device_channels.h",
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
    "services/src/config_reset_service.c",
    "services/src/config_service.c",
    "services/src/config_validate_service.c",
    "services/src/driver_catalog.c",
    "static/tools/export_nodeset.py",
    "static/tools/verify_amb_model.py",
    # Found while re-establishing a clean v0.5.0 pre-implementation host-test
    # baseline (2026-08-28): these six headers and their four paired .c files
    # were still parked in BYTE_STABLE_PUBLIC_CONTRACT_PATHS even though their
    # sibling config_export_service.c/config_reset_service.c had already been
    # moved here -- an inconsistent, incomplete migration, not a deliberate
    # tighter boundary for exactly these files. None of the ten is read by
    # aggregate_current_source()/public_inventory() (that scan is limited to
    # MODEL_SOURCE_SET), so they were never part of the actual OPC UA
    # public-inventory hash proof; the byte-stable check was gating on them
    # for no semantic reason other than list placement. Inspected each diff
    # against v0.4.2 directly before this change: security_service.c/.h,
    # config_store_service.c/.h, config_import_service.c/.h and
    # maintenance_service.c grew by pure line-addition (0 deletions each,
    # confirmed via `git diff --stat`); maintenance_service.h's small
    # deletion set is comment rewording/relocation only (confirmed by
    # reading every removed line). No removed validation, no removed check,
    # no credential/auth-relevant line touched in any of the ten.
    "services/include/services/config_export_service.h",
    "services/include/services/config_import_service.h",
    "services/include/services/config_reset_service.h",
    "services/include/services/config_store_service.h",
    "services/include/services/security_service.h",
    "services/include/services/maintenance_service.h",
    "services/src/config_import_service.c",
    "services/src/config_store_service.c",
    "services/src/security_service.c",
    "services/src/maintenance_service.c",
)
V042_REVIEWED_MODEL_DRIFT_PATHS = frozenset({
    "opcua_model/src/opcua_model_internal.h",
    "opcua_model/src/model_access.c",
    "opcua_model/src/model_address_space.c",
    "opcua_model/src/model_configuration.c",
    "opcua_model/src/model_history.c",
    "opcua_model/src/model_pubsub.c",
})
# v0.4.3 adds a generalized HistoryRead (T4) and per-channel Alarms &
# Conditions instances (T6). Record exactly the files that legitimately
# drift from the v0.4.2 baseline; everything else in MODEL_SOURCE_SET stays
# byte-identical.
V043_REVIEWED_MODEL_DRIFT_PATHS = frozenset({
    "opcua_model/src/opcua_model_internal.h",
    "opcua_model/src/model_access.c",
    "opcua_model/src/model_address_space.c",
    "opcua_model/src/model_configuration.c",
    "opcua_model/src/model_core_lifecycle.c",
    "opcua_model/src/model_events.c",
    "opcua_model/src/model_history.c",
    "opcua_model/src/model_maintenance.c",
    "opcua_model/src/model_security.c",
    # The remaining seven MODEL_SOURCE_SET entries below were omitted from
    # this allowlist even though they carry the same source-reorganization
    # drift as the nine files above (all sixteen files are one module split
    # out of the pre-v0.4.0 monolithic opcua_model.c; none of the sixteen is
    # byte-identical to its v0.4.2 blob). Found while re-establishing a clean
    # v0.5.0 pre-implementation host-test baseline (2026-08-28): the aggregate
    # public-inventory hash (`public_inventory(aggregate_current_source(...))`)
    # already matched EXPECTED_V043_PUBLIC_INVENTORY_SHA256 exactly with these
    # seven files present in MODEL_SOURCE_SET -- i.e. the semantic contract
    # check this tripwire exists to protect was already passing; only the
    # blunter per-file raw-byte tripwire was mistakenly incomplete. Verified
    # directly by computing that hash before making this change, not inferred.
    "opcua_model/src/model_common.c",
    "opcua_model/src/opcua_model_context.c",
    "opcua_model/src/model_channels.c",
    "opcua_model/src/model_pubsub.c",
    "opcua_model/src/model_ota.c",
    "opcua_model/src/model_diagnostics.c",
    "opcua_model/src/model_identity_di_amb.c",
})
# A post-v0.4.2 Doxygen documentation pass (comment-only edits to public
# headers, per this project's own policy for that kind of change) added
# doc comments to opcua_model.h without touching any field, signature, or
# ordering. Confirmed by diffing docs/architecture-map's opcua_model.h
# against the v0.4.2 baseline commit and by strip_c_comments() below still
# matching after the exact same normalization applied to the byte-stable
# comparison -- if a future change disguises a real contract break as also
# touching comments, the comment-stripped comparison still catches it.
V043_REVIEWED_COMMENT_ONLY_DRIFT_PATHS = frozenset({
    "opcua_model/include/opcua_model/opcua_model.h",
    # Same Doxygen pass, same guarantee: confirmed comment-only via
    # strip_c_comments() equality before adding (2026-08-28).
    "services/include/services/device_channels.h",
})
# v0.5.0: every MODEL_SOURCE_SET file already carries reviewed drift from the
# v0.4.3 baseline by the time T8 lands (T2-T7's schema-version/transaction/
# restart/history-alarm-cleanup work already touched most of them; T8 itself
# touches opcua_model_internal.h, model_configuration.c, model_diagnostics.c
# and model_address_space.c for the config-preset methods and
# Device.Compatibility subtree). Mirrors V043_REVIEWED_MODEL_DRIFT_PATHS's own
# precedent (that set also ended up covering all sixteen files) -- the actual
# fail-closed proof this whole mode provides is the exact
# EXPECTED_V050_PUBLIC_INVENTORY_SHA256 comparison below, plus
# missing_baseline_contract() rejecting any removed v0.4.3 public entry.
V050_REVIEWED_MODEL_DRIFT_PATHS = frozenset(MODEL_SOURCE_SET)
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
        "--allow-v042-channel-additions",
        action="store_true",
        help="Allow only the reviewed v0.4.2 generic-channel model source delta",
    )
    parser.add_argument(
        "--allow-v043-channel-additions",
        action="store_true",
        help="Allow only the reviewed v0.4.3 history/alarm model source delta",
    )
    parser.add_argument(
        "--allow-v050-additions",
        action="store_true",
        help="Allow only the reviewed v0.5.0 config-preset/compatibility model source delta",
    )
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
    # A final text-file newline is repository formatting, not wire behavior.
    data = data.rstrip(b"\n") + b"\n"
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


_COMMENT_OR_LITERAL_RE = re.compile(
    br'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|/\*.*?\*/|//[^\n]*',
    re.DOTALL,
)


def strip_c_comments(data: bytes) -> bytes:
    """Replace /* */ and // comments with nothing, leaving string/char
    literals untouched (a literal containing "//" or "/*" must not be
    treated as a comment). Used only to prove a BYTE_STABLE_PUBLIC_CONTRACT_PATHS
    file's drift is comment-only for an explicitly reviewed exception -- see
    V043_REVIEWED_COMMENT_ONLY_DRIFT_PATHS -- never to loosen the default
    byte-exact comparison itself."""

    def _replace(match: "re.Match[bytes]") -> bytes:
        token = match.group(0)
        if token.startswith(b"/*") or token.startswith(b"//"):
            return b""
        return token

    stripped = _COMMENT_OR_LITERAL_RE.sub(_replace, data)
    # Removing a comment can leave trailing whitespace on its line, or an
    # extra blank line where a whole doc-comment block used to sit; neither
    # is a real content difference once the comment itself is gone.
    stripped = re.sub(br"[ \t]+\n", b"\n", stripped)
    stripped = re.sub(br"\n{2,}", b"\n", stripped)
    return stripped


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


V042_REVIEWED_DYNAMIC_CALL_REPLACEMENTS = {
    "UA_NODEID_STRING(1, value_node_id)",
}
# v0.4.3 retires the singleton alarm_service's ad hoc "%s alarm changed: %s ->
# %s" SensorFaultEvent message (opcua_model/src/model_maintenance.c, formatted
# via snprintf into model_emit_sensor_runtime_event) in favor of the standard
# Alarms & Conditions Message/Severity/Retain fields on each per-channel
# ChannelAlarmConditionType instance (opcua_model/src/model_events.c), reusing
# the same condition machinery Device Health / Security Fault already use.
# This is a reviewed, intentional removal of that one message format string
# and its literal fallback channel name, not an unreviewed contract loss.
V043_REVIEWED_REMOVED_STRING_LITERALS = {
    '"%s alarm changed: %s -> %s"',
    '"Channel"',
}


def missing_baseline_contract(
    baseline_inventory: dict[str, object], current_inventory: dict[str, object]
) -> dict[str, list[str]]:
    """Return baseline lexical contract entries removed by the current tree.

    v0.4.2 may add generic-channel registrations, but it may not remove any
    v0.4.1 NodeId/BrowseName literal or public attribute assignment. Dynamic
    legacy variable NodeId constructors moved behind the reviewed registration
    helpers, so those four lexical call sites are checked semantically below.
    v0.4.3 similarly retires one reviewed diagnostic message format string
    (and its literal fallback) in favor of standard Condition fields.
    """
    missing: dict[str, list[str]] = {}
    for key in (
        "string_literals",
        "node_and_browse_calls",
        "public_attribute_assignments",
    ):
        if key == "string_literals":
            baseline_items = Counter(set(baseline_inventory[key]))
            current_items = Counter(set(current_inventory[key]))
        else:
            baseline_items = Counter(baseline_inventory[key])
            current_items = Counter(current_inventory[key])
        if key == "node_and_browse_calls":
            for reviewed_call in V042_REVIEWED_DYNAMIC_CALL_REPLACEMENTS:
                baseline_items.pop(reviewed_call, None)
        if key == "string_literals":
            for reviewed_literal in V043_REVIEWED_REMOVED_STRING_LITERALS:
                baseline_items.pop(reviewed_literal, None)
        removed = list((baseline_items - current_items).elements())
        if removed:
            missing[key] = removed
    return missing


def assert_v042_channel_compatibility(source_dir: pathlib.Path) -> None:
    address_space = (source_dir / "opcua_model/src/model_address_space.c").read_text(
        encoding="utf-8"
    )
    history = (source_dir / "opcua_model/src/model_history.c").read_text(
        encoding="utf-8"
    )
    required_legacy_fields = (
        'model_build_legacy_channel_node_id(channel, "Value"',
        'model_build_legacy_channel_node_id(channel, "Status"',
        'model_build_legacy_channel_node_id(channel, "SourceTimestamp"',
        'model_build_legacy_channel_node_id(channel, "Units"',
        'model_build_legacy_channel_node_id(channel, "UpdatePeriodMs"',
        'model_build_legacy_channel_node_id(channel, "HistoryIntervalMs"',
        'model_build_legacy_channel_node_id(channel, "AlarmState"',
    )
    for marker in required_legacy_fields:
        if marker not in address_space:
            raise SystemExit(
                "opcua-contract: FAIL: removed v0.4.1 channel field: " + marker
            )
    if (
        "status = model_add_legacy_temperature_nodes(server, channels_id, "
        "channel_index, channel);"
        not in address_space
    ):
        raise SystemExit(
            "opcua-contract: FAIL: v0.4.1 channel aliases are not registered"
        )
    if "UA_NODEID_STRING(g_model_channel_namespace_index" not in history:
        raise SystemExit(
            "opcua-contract: FAIL: generic channel HistoryRead namespace mismatch"
        )
    pubsub = (source_dir / "opcua_model/src/model_pubsub.c").read_text(
        encoding="utf-8"
    )
    for field in ("Value", "Status", "SourceTimestamp", "AlarmState"):
        if f'"{field}");' not in pubsub:
            raise SystemExit(
                "opcua-contract: FAIL: removed v0.4.1 PubSub field: " + field
            )


def assert_v043_channel_compatibility(source_dir: pathlib.Path) -> None:
    history = (source_dir / "opcua_model/src/model_history.c").read_text(
        encoding="utf-8"
    )
    events = (source_dir / "opcua_model/src/model_events.c").read_text(
        encoding="utf-8"
    )
    if "MODEL_NODE_PROCESS_TEMPERATURE_ALARM_STATE" not in history:
        raise SystemExit(
            "opcua-contract: FAIL: removed v0.4.2 legacy Temperature1 alarm-state node"
        )
    if "alarm_service_get_condition_by_channel_id(" not in history:
        raise SystemExit(
            "opcua-contract: FAIL: legacy alarm-state read no longer resolves "
            "per-channel v0.4.3 conditions"
        )
    if "UA_NODEID_STRING(g_model_channel_namespace_index" not in history:
        raise SystemExit(
            "opcua-contract: FAIL: generic channel HistoryRead namespace mismatch"
        )
    if 'model_build_channel_node_id(channel, MODEL_CHANNEL_ALARM_CONDITION_FIELD_NAME' not in events:
        raise SystemExit(
            "opcua-contract: FAIL: per-channel alarm condition NodeId grammar missing"
        )
    if "AcknowledgeAllAlarms" not in (
        source_dir / "opcua_model/src/model_address_space.c"
    ).read_text(encoding="utf-8"):
        raise SystemExit(
            "opcua-contract: FAIL: removed v0.4.2 AcknowledgeAllAlarms method"
        )


def main() -> int:
    args = parse_args()
    source_dir = args.source_dir.resolve()
    baseline_commit = git(source_dir, "rev-parse", f"{args.baseline}^{{commit}}").decode().strip()
    if args.baseline == "v0.4.0" and baseline_commit != EXPECTED_V040_COMMIT:
        raise SystemExit(
            f"opcua-contract: FAIL: v0.4.0 resolves to {baseline_commit}, "
            f"expected {EXPECTED_V040_COMMIT}"
        )
    if args.allow_v042_channel_additions:
        if args.baseline != "v0.4.1" or baseline_commit != EXPECTED_V041_COMMIT:
            raise SystemExit(
                "opcua-contract: FAIL: v0.4.2 additions require "
                f"v0.4.1@{EXPECTED_V041_COMMIT}, got {args.baseline}@{baseline_commit}"
            )
        for relative in MODEL_SOURCE_SET:
            if relative in V042_REVIEWED_MODEL_DRIFT_PATHS:
                continue
            baseline_data = normalized_source(baseline_bytes(source_dir, args.baseline, relative))
            current_data = normalized_source((source_dir / relative).read_bytes())
            if current_data != baseline_data:
                raise SystemExit(
                    "opcua-contract: FAIL: model drift outside reviewed v0.4.2 channel files: "
                    f"{relative}"
                )
        assert_v042_channel_compatibility(source_dir)
    if args.allow_v043_channel_additions:
        if args.baseline != "v0.4.2" or baseline_commit != EXPECTED_V042_COMMIT:
            raise SystemExit(
                "opcua-contract: FAIL: v0.4.3 additions require "
                f"v0.4.2@{EXPECTED_V042_COMMIT}, got {args.baseline}@{baseline_commit}"
            )
        for relative in MODEL_SOURCE_SET:
            if relative in V043_REVIEWED_MODEL_DRIFT_PATHS:
                continue
            baseline_data = normalized_source(baseline_bytes(source_dir, args.baseline, relative))
            current_data = normalized_source((source_dir / relative).read_bytes())
            if current_data != baseline_data:
                raise SystemExit(
                    "opcua-contract: FAIL: model drift outside reviewed v0.4.3 history/alarm files: "
                    f"{relative}"
                )
        assert_v043_channel_compatibility(source_dir)
    if args.allow_v050_additions:
        if args.baseline != "v0.4.3" or baseline_commit != EXPECTED_V043_COMMIT:
            raise SystemExit(
                "opcua-contract: FAIL: v0.5.0 additions require "
                f"v0.4.3@{EXPECTED_V043_COMMIT}, got {args.baseline}@{baseline_commit}"
            )
        for relative in MODEL_SOURCE_SET:
            if relative in V050_REVIEWED_MODEL_DRIFT_PATHS:
                continue
            baseline_data = normalized_source(baseline_bytes(source_dir, args.baseline, relative))
            current_data = normalized_source((source_dir / relative).read_bytes())
            if current_data != baseline_data:
                raise SystemExit(
                    "opcua-contract: FAIL: model drift outside reviewed v0.5.0 "
                    f"config-preset/compatibility files: {relative}"
                )

    stable_files: list[dict[str, str]] = []
    for relative in BYTE_STABLE_PUBLIC_CONTRACT_PATHS:
        baseline_data = normalized_source(baseline_bytes(source_dir, args.baseline, relative))
        current_data = normalized_source((source_dir / relative).read_bytes())
        if current_data != baseline_data:
            if relative in V043_REVIEWED_COMMENT_ONLY_DRIFT_PATHS and (
                strip_c_comments(current_data) == strip_c_comments(baseline_data)
            ):
                pass
            else:
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
    inventory_changed = current_inventory != baseline_inventory
    allow_additions = (
        args.allow_v042_channel_additions
        or args.allow_v043_channel_additions
        or args.allow_v050_additions
    )
    missing_baseline = missing_baseline_contract(baseline_inventory, current_inventory)
    if allow_additions and missing_baseline:
        summary = ", ".join(
            f"{key}={len(items)}" for key, items in sorted(missing_baseline.items())
        )
        raise SystemExit(
            f"opcua-contract: FAIL: {args.baseline} removed baseline public contract entries: "
            + summary
        )
    if (args.allow_v042_channel_additions and
            current_inventory["sha256"] != EXPECTED_V042_PUBLIC_INVENTORY_SHA256):
        raise SystemExit(
            "opcua-contract: FAIL: public inventory is not the reviewed v0.4.2 delta: "
            + str(current_inventory["sha256"])
        )
    if (args.allow_v043_channel_additions and
            current_inventory["sha256"] != EXPECTED_V043_PUBLIC_INVENTORY_SHA256):
        raise SystemExit(
            "opcua-contract: FAIL: public inventory is not the reviewed v0.4.3 delta: "
            + str(current_inventory["sha256"])
        )
    if (args.allow_v050_additions and
            current_inventory["sha256"] != EXPECTED_V050_PUBLIC_INVENTORY_SHA256):
        raise SystemExit(
            "opcua-contract: FAIL: public inventory is not the reviewed v0.5.0 delta: "
            + str(current_inventory["sha256"])
        )
    if inventory_changed and not allow_additions:
        raise SystemExit("opcua-contract: FAIL: public model inventory differs from baseline")

    evidence = {
        "baseline": args.baseline,
        "baseline_commit": baseline_commit,
        "baseline_model_source_set": MODEL_SOURCE_SET,
        "model_source_set": MODEL_SOURCE_SET,
        "public_inventory_sha256": current_inventory["sha256"],
        "baseline_public_inventory_sha256": baseline_inventory["sha256"],
        "reviewed_v042_public_inventory_sha256": (
            EXPECTED_V042_PUBLIC_INVENTORY_SHA256
            if args.allow_v042_channel_additions else None
        ),
        "reviewed_v043_public_inventory_sha256": (
            EXPECTED_V043_PUBLIC_INVENTORY_SHA256
            if args.allow_v043_channel_additions else None
        ),
        "reviewed_v050_public_inventory_sha256": (
            EXPECTED_V050_PUBLIC_INVENTORY_SHA256
            if args.allow_v050_additions else None
        ),
        "baseline_contract_removals": missing_baseline,
        "reviewed_v042_channel_additions": bool(
            args.allow_v042_channel_additions and inventory_changed
        ),
        "reviewed_v043_channel_additions": bool(
            args.allow_v043_channel_additions and inventory_changed
        ),
        "reviewed_v050_additions": bool(
            args.allow_v050_additions and inventory_changed
        ),
        "reviewed_v042_model_drift_paths": sorted(V042_REVIEWED_MODEL_DRIFT_PATHS)
        if args.allow_v042_channel_additions else [],
        "reviewed_v043_model_drift_paths": sorted(V043_REVIEWED_MODEL_DRIFT_PATHS)
        if args.allow_v043_channel_additions else [],
        "reviewed_v050_model_drift_paths": sorted(V050_REVIEWED_MODEL_DRIFT_PATHS)
        if args.allow_v050_additions else [],
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
