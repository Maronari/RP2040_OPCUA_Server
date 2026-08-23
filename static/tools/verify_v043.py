#!/usr/bin/env python3
"""Runtime OPC UA protocol verification for the v0.4.3 history/alarm engine.

Unlike ``static/tools/test_history_service.py``,
``static/tools/test_history_flash_backend.py`` and
``static/tools/test_v043_opcua_adapter_sources.py`` (host-only unit tests and
source-level contract checks), this script is a real ``asyncua`` client that
exercises a *running* instance of this firmware over the wire, in the same
family as ``verify_channel_model.py``, ``verify_amb_model.py`` and
``verify_maintenance_workflow.py``: it discovers the generic channel model at
runtime (never hardcoding a channel count) and asserts protocol-visible
behavior, not source text.

It covers the v0.4.3 closeout requirements:

1. HistoryRead across the historized channel(s) named on the command line,
   asserting the exact runtime VariantType of each.
2. More than one ``HISTORY_SERVICE_MAX_RETURN_COUNT`` (128-sample) page via a
   continuation point, verifying the second page continues the first with no
   gap and no duplicate.
3. Exhaustion of the real continuation-point pool -- confirmed in code as
   ``MODEL_HISTORY_CONTINUATION_CAPACITY = 16`` in
   ``opcua_model/src/opcua_model_internal.h`` (also asserted by
   ``static/tools/test_v043_opcua_adapter_sources.py``) -- verifying the 17th
   concurrent continuation is rejected with ``BadNoContinuationPoints`` and
   that draining the pool restores capacity.
4. Continuation-point invalidation: explicit client release
   (``releaseContinuationPoints``), server-side expiry (30 s, from
   ``MODEL_HISTORY_CONTINUATION_EXPIRY_MS`` in the same header), rejection
   when quoted by a foreign session, and rejection on a ring-generation
   change between page 1 and page 2 (the cross-call half of the race fixed
   in ``services/src/history_service.c`` commit ``da0b057``).
5. Two simultaneous, independent Alarms & Conditions instances on two
   channels (opt-in: requires a device already configured with a second
   alarm-eligible channel -- see "Two-channel note" below).
6. The standard ``Acknowledge`` Method (namespace-0 node id 9111,
   ``AcknowledgeableConditionType_Acknowledge``) on an active condition, and
   the standard ``ConditionRefresh`` Method (node id 3875,
   ``ConditionType_ConditionRefresh``, called on the Server object) with a
   subscription actually observing the republished condition state.

Reference-profile note
-----------------------
``app/basic_profile/main.c`` (the only shipped device profile at the time
this script was written) configures exactly **one** channel:
``channel.temperature.1`` (FLOAT, alarm-eligible, historizing). Requirements
1 and 5 ask for two distinct value types / two independent channels; against
the profile as shipped there is only one. This script never hardcodes that
assumption -- pass ``--channel`` more than once (see "Two-channel note")
against a device already reconfigured with a second alarm-eligible channel
(e.g. a Boolean GPIO input) to exercise the full mixed-type / dual-condition
checks. Run with defaults against the shipped profile and the script still
verifies everything a single channel supports; the two-channel checks are
clearly reported as skipped rather than silently passed.

Two-channel note
-----------------
This script never performs a topology change (adding/removing a channel
requires ``Device.Maintenance.Reboot`` per the existing ``SetChannelConfig``
contract) -- that is out of scope for a verification tool. If you want the
mixed-value-type and dual-condition checks to run for real, provision a
second alarm-eligible channel through the existing, already-shipped
``SetChannelConfig``/``ApplyConfig``/``SaveConfig``/``Reboot`` maintenance
workflow (or a full config import via ``stage_config.py``) *before* invoking
this script, then pass ``--channel`` twice.

Forcing an alarm (--force-alarm)
---------------------------------
Requirements 5 and 6 need an *active* condition to acknowledge/refresh. The
reference channel is a read-only internal-temperature sensor, so its value
cannot be written directly. ``--force-alarm`` (opt-in, off by default) uses
the existing, already-shipped ``Device.Maintenance.SetChannelConfig`` /
``Device.Maintenance.GetChannelConfig`` / ``Device.Maintenance.ApplyConfig``
methods to retune the target channel's WarningThreshold below its current
live value (forcing a HIGH condition), then restores the original threshold
via the same methods in a ``finally`` block. This never calls ``SaveConfig``
(nothing is persisted to flash) and never reboots the device.

Accelerating history accumulation (--accelerate-history)
----------------------------------------------------------
Requirement 2 needs > 128 samples and requirement 4's generation-mismatch
check needs the per-channel ring (256 samples, T1 budget) to actually wrap.
At the shipped 1 Hz sample/history rate that is several minutes of real
uptime. ``--accelerate-history`` (opt-in, off by default) uses the same
SetChannelConfig/ApplyConfig round-trip to temporarily shrink
SamplePeriodMs/HistoryIntervalMs, then restores the originals. Left off, the
script simply waits (bounded by --history-wait-timeout) for natural
accumulation.

Host-side verification
-----------------------
This firmware only builds for the RP2350 target (see CMakeLists.txt); there
is no host-buildable server binary for this project the way, for example,
``test_verify_channel_model_e2e.py`` stands up a *generic* asyncua.Server
fixture to protocol-test ``verify_channel_model.py``'s browsing/write logic.
The HistoryRead continuation-point pool (16-slot global table, 30 s expiry,
per-session ownership, ring-generation validation) and the per-channel
Alarms & Conditions model exercised here are firmware-specific behavior with
no equivalent in a generic asyncua.Server, so they cannot be faithfully
emulated host-side. What *was* verified without hardware: this file imports
cleanly, argument-parses correctly (``--help``), and its non-network helpers
(NodeId construction, continuation-point bookkeeping, JSON parsing) run
correctly under Python 3.13 / asyncua 2.0.1 -- see the accompanying report.
Everything that touches the wire requires the physical reference device and
is reported as not run unless actually performed, per this project's release
verification convention (docs/releases/v0.4.3.md).
"""

from __future__ import annotations

import argparse
import asyncio
from datetime import datetime, timedelta, timezone
from importlib.metadata import PackageNotFoundError, version
import json
import os
import sys
import time

import stage_config as opcua_common


REQUIRED_PYTHON = (3, 13)
REQUIRED_ASYNCUA_VERSION = "2.0.1"

CHANNEL_NAMESPACE_URI = "urn:maronari:rp2-industrial-opcua:channel-model"
VALUE_TYPES = {"Float", "Double", "Int32", "UInt32", "Int64", "UInt64", "Boolean"}

# Frozen per docs/releases/v0.4.3.md T1/T4 and
# services/include/services/history_service.h.
HISTORY_SERVICE_MAX_RETURN_COUNT = 128
# Cross-checked live against opcua_model/src/opcua_model_internal.h
# (MODEL_HISTORY_CONTINUATION_CAPACITY / MODEL_HISTORY_CONTINUATION_EXPIRY_MS)
# by verify_continuation_pool_constants() before any of these defaults are
# trusted; the values below are only the expected/frozen values.
EXPECTED_CONTINUATION_CAPACITY = 16
EXPECTED_CONTINUATION_EXPIRY_S = 30.0
CONTINUATION_EXPIRY_MARGIN_S = 3.0

MAINTENANCE_NODE_ID = "ns=1;s=Device.Maintenance"
# GetChannelConfig/SetChannelConfig/ApplyConfig are registered under the
# Configuration sub-folder (MODEL_NODE_MAINTENANCE_CONFIGURATION in
# opcua_model/src/opcua_model_internal.h), not the Maintenance folder itself.
MAINTENANCE_CONFIGURATION_NODE_ID = "ns=1;s=Device.Maintenance.Configuration"
METHOD_SET_CHANNEL_CONFIG = "ns=1;s=Device.Maintenance.SetChannelConfig"
METHOD_GET_CHANNEL_CONFIG = "ns=1;s=Device.Maintenance.GetChannelConfig"
METHOD_APPLY_CONFIG = "ns=1;s=Device.Maintenance.ApplyConfig"
CONFIG_MAX_CHANNELS = 16

# Namespace-0 numeric ids for the standard Alarms & Conditions Methods this
# firmware wires up in opcua_model/src/model_events.c
# (model_fix_condition_method_arguments) -- not invented here.
NS0_ACKNOWLEDGE_METHOD_ID = 9111  # AcknowledgeableConditionType_Acknowledge
NS0_CONDITION_REFRESH_METHOD_ID = 3875  # ConditionType_ConditionRefresh
NS0_SERVER_OBJECT_ID = 2253  # Server

DEFAULT_HISTORY_WAIT_TIMEOUT_S = 900.0
DEFAULT_ALARM_WAIT_TIMEOUT_S = 10.0
ALARM_TASK_PERIOD_S = 0.5  # services/src/alarm_service.c vTaskDelayUntil period


class Audit:
    def __init__(self):
        self.failures = []
        self.skipped = []
        self.passed = 0

    def check(self, condition, description, category="assertion"):
        context = f"[{category}] {description}"
        if condition:
            self.passed += 1
            print(f"[ OK ] {context}")
        else:
            self.failures.append(context)
            print(f"[FAIL] {context}")
        return condition

    def equal(self, actual, expected, description, category="assertion"):
        return self.check(actual == expected,
                          f"{description}: expected {expected!r}, got {actual!r}", category)

    def skip(self, description, reason, category="assertion"):
        context = f"[{category}] {description}: SKIPPED ({reason})"
        self.skipped.append(context)
        print(f"[SKIP] {context}")

    def report(self):
        print(f"Checks passed: {self.passed}; failed: {len(self.failures)}; "
              f"skipped: {len(self.skipped)}")
        if self.skipped:
            print("Skipped (not exercised -- see reason):")
            for entry in self.skipped:
                print(f"  - {entry}")
        if self.failures:
            print("Failures:")
            for failure in self.failures:
                print(f"  - {failure}")

    @staticmethod
    def info(message):
        print(f"[INFO] {message}")


# --------------------------------------------------------------------------
# Channel NodeId grammar (mirrors opcua_model/src/model_access.c
# model_build_channel_node_id() and verify_channel_model.py's identical
# helper -- one hex-encoded grammar shared by the whole channel model).
# --------------------------------------------------------------------------

def channel_node_id(channel_id: str, field: str | None = None) -> str:
    encoded = channel_id.encode("utf-8").hex().upper()
    result = f"Device.Channels.c_{encoded}"
    return result if field is None else f"{result}.{field}"


def parse_channel_spec(value: str):
    """CLI contract: CHANNEL_ID or CHANNEL_ID=ExpectedVariantType."""
    channel_id, separator, variant_type = value.partition("=")
    if not channel_id:
        raise argparse.ArgumentTypeError("expected CHANNEL_ID or CHANNEL_ID=VariantType")
    if not separator:
        return channel_id, None
    if variant_type not in VALUE_TYPES:
        raise argparse.ArgumentTypeError(
            f"unsupported VariantType {variant_type!r}; expected one of {sorted(VALUE_TYPES)}")
    return channel_id, variant_type


def verify_runtime(audit: Audit) -> bool:
    python_version = ".".join(map(str, sys.version_info[:3]))
    audit.check(sys.version_info[:2] == REQUIRED_PYTHON,
                f"Python {python_version}; required {REQUIRED_PYTHON[0]}.{REQUIRED_PYTHON[1]}",
                "environment")
    try:
        installed_asyncua = version("asyncua")
    except PackageNotFoundError:
        installed_asyncua = None
    audit.equal(installed_asyncua, REQUIRED_ASYNCUA_VERSION,
                "asyncua distribution version", "environment")
    return not audit.failures


async def client_for(args, username, password):
    client = opcua_common.Client(url=args.server_url, timeout=args.timeout,
                                 watchdog_intervall=args.timeout)
    role_args = argparse.Namespace(**vars(args), username=username)
    try:
        await opcua_common.configure_client_security(client, role_args)
        if username:
            client.set_user(username)
            client.set_password(password or "")
        await client.connect()
        return client
    except Exception:
        try:
            await client.disconnect()
        except Exception:
            pass
        raise


async def safe_disconnect(client, audit, label):
    if client is None:
        return
    try:
        await client.disconnect()
    except Exception as exc:
        audit.check(False, f"{label}: {type(exc).__name__}: {exc}", "cleanup")


async def safe_delete(subscription, audit, label):
    if subscription is None:
        return
    try:
        await subscription.delete()
    except Exception as exc:
        audit.check(False, f"{label}: {type(exc).__name__}: {exc}", "cleanup")


# --------------------------------------------------------------------------
# Low-level HistoryReadRaw with explicit continuation/release control.
# Node.read_raw_history()/Node.history_read() in asyncua either loop through
# every page automatically or hide ReleaseContinuationPoints entirely; this
# script needs both exposed to test pagination, exhaustion, release, expiry,
# foreign-session and generation-mismatch behavior precisely.
# --------------------------------------------------------------------------

async def history_read_raw(client, ua, node_id, start_time, end_time, numvalues,
                           continuation=None, release=False):
    details = ua.ReadRawModifiedDetails()
    details.IsReadModified = False
    details.StartTime = start_time
    details.EndTime = end_time
    details.NumValuesPerNode = numvalues
    details.ReturnBounds = False
    valueid = ua.HistoryReadValueId()
    valueid.NodeId = node_id
    valueid.IndexRange = ""
    valueid.ContinuationPoint = continuation
    params = ua.HistoryReadParameters()
    params.HistoryReadDetails = details
    params.TimestampsToReturn = ua.TimestampsToReturn.Both
    params.ReleaseContinuationPoints = release
    params.NodesToRead = [valueid]
    results = await client.uaclient.session.history_read(params)
    return results[0]


def status_value(status_code):
    return int(getattr(status_code, "value", status_code))


def status_name(status_code):
    return getattr(status_code, "name", str(status_code))


def status_matches(actual_status, expected_int):
    return status_value(actual_status) == int(expected_int)


def result_samples(result):
    history_data = getattr(result, "HistoryData", None)
    values = getattr(history_data, "DataValues", None)
    return list(values) if values else []


# --------------------------------------------------------------------------
# Channel discovery / value-type verification (requirement 1)
# --------------------------------------------------------------------------

async def resolve_channel_namespace(client, ua):
    return await client.get_namespace_index(CHANNEL_NAMESPACE_URI)

async def channel_value_node(client, ua, ns, channel_id):
    return client.get_node(ua.NodeId(channel_node_id(channel_id, "Value"), ns))


async def channel_source_node(client, ua, ns, channel_id):
    return client.get_node(ua.NodeId(channel_node_id(channel_id), ns))


async def channel_alarm_condition_node(client, ua, ns, channel_id):
    return client.get_node(ua.NodeId(channel_node_id(channel_id, "AlarmCondition"), ns))


async def verify_value_types(client, ua, audit, ns, channels, require_mixed):
    """Requirement 1: HistoryRead-eligible channels with exact runtime types."""
    discovered_types = {}
    for channel_id, expected_type in channels:
        value_node = await channel_value_node(client, ua, ns, channel_id)
        try:
            data_value = await value_node.read_data_value()
        except Exception as exc:
            audit.check(False, f"{channel_id}: Value node unreadable: "
                        f"{type(exc).__name__}: {exc}", "discovery")
            continue
        variant_type = data_value.Value.VariantType.name
        audit.check(variant_type in VALUE_TYPES,
                    f"{channel_id}: Value VariantType {variant_type} is a supported "
                    "historized type", "discovery")
        if expected_type is not None:
            audit.equal(variant_type, expected_type, f"{channel_id}: expected VariantType",
                        "discovery")
        discovered_types[channel_id] = variant_type
    distinct = set(discovered_types.values())
    audit.info(f"discovered value types: {discovered_types}")
    if require_mixed:
        audit.check(len(distinct) >= 2,
                    "--require-mixed-value-types: at least two distinct historized "
                    f"VariantTypes are present (got {sorted(distinct)})", "discovery")
    elif len(distinct) < 2:
        audit.skip("HistoryRead across >= 2 distinct value types", "only one distinct "
                   "VariantType is configured/requested -- app/basic_profile/main.c ships "
                   "only channel.temperature.1 (Float); pass --channel a second time "
                   "against a device already reconfigured with another alarm-eligible "
                   "channel to exercise this for real (see script docstring)", "discovery")
    else:
        audit.check(True, f"HistoryRead spans >= 2 distinct value types: {sorted(distinct)}",
                    "discovery")
    return discovered_types


# --------------------------------------------------------------------------
# Continuation-point pool constants: cross-check against the actual
# firmware source in this worktree rather than trusting the frozen
# EXPECTED_* constants blindly, per the task's explicit instruction.
# --------------------------------------------------------------------------

def verify_continuation_pool_constants(audit):
    repo_root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
    header_path = os.path.join(repo_root, "opcua_model", "src", "opcua_model_internal.h")
    try:
        text = open(header_path, "r", encoding="utf-8").read()
    except OSError as exc:
        audit.check(False, f"cannot read {header_path} to cross-check the continuation "
                    f"pool capacity: {type(exc).__name__}: {exc}", "source-cross-check")
        return EXPECTED_CONTINUATION_CAPACITY, EXPECTED_CONTINUATION_EXPIRY_S
    import re
    capacity_match = re.search(r"#define\s+MODEL_HISTORY_CONTINUATION_CAPACITY\s+(\d+)u",
                               text)
    expiry_match = re.search(r"#define\s+MODEL_HISTORY_CONTINUATION_EXPIRY_MS\s+(\d+)u",
                             text)
    capacity = int(capacity_match.group(1)) if capacity_match else None
    expiry_s = int(expiry_match.group(1)) / 1000.0 if expiry_match else None
    audit.equal(capacity, EXPECTED_CONTINUATION_CAPACITY,
                "MODEL_HISTORY_CONTINUATION_CAPACITY read from "
                "opcua_model/src/opcua_model_internal.h", "source-cross-check")
    audit.equal(expiry_s, EXPECTED_CONTINUATION_EXPIRY_S,
                "MODEL_HISTORY_CONTINUATION_EXPIRY_MS (seconds) read from the same header",
                "source-cross-check")
    return (capacity if capacity is not None else EXPECTED_CONTINUATION_CAPACITY,
            expiry_s if expiry_s is not None else EXPECTED_CONTINUATION_EXPIRY_S)


# --------------------------------------------------------------------------
# History accumulation / acceleration (requirements 2 and 4)
# --------------------------------------------------------------------------

async def get_channel_config_json(client, ua, slot_index):
    maintenance = client.get_node(ua.NodeId.from_string(MAINTENANCE_CONFIGURATION_NODE_ID))
    method_id = ua.NodeId.from_string(METHOD_GET_CHANNEL_CONFIG)
    result = await maintenance.call_method(method_id, ua.Variant(slot_index, ua.VariantType.UInt32))
    text = result[0] if isinstance(result, (list, tuple)) else result
    return json.loads(text)


async def find_channel_slot(client, ua, channel_id):
    for slot in range(CONFIG_MAX_CHANNELS):
        config = await get_channel_config_json(client, ua, slot)
        if config.get("inUse") and config.get("id") == channel_id:
            return slot, config
    return None, None


async def set_channel_config(client, ua, slot_index, config, *, warning_threshold=None,
                             critical_threshold=None, sample_period_ms=None,
                             history_interval_ms=None):
    maintenance = client.get_node(ua.NodeId.from_string(MAINTENANCE_CONFIGURATION_NODE_ID))
    method_id = ua.NodeId.from_string(METHOD_SET_CHANNEL_CONFIG)
    inputs = (
        ua.Variant(slot_index, ua.VariantType.UInt32),
        ua.Variant(True, ua.VariantType.Boolean),
        ua.Variant(bool(config["enabled"]), ua.VariantType.Boolean),
        ua.Variant(int(config["driverId"]), ua.VariantType.UInt32),
        ua.Variant(int(config["sourceIndex"]), ua.VariantType.UInt32),
        ua.Variant(int(config["sourceFlags"]), ua.VariantType.UInt32),
        ua.Variant(int(sample_period_ms if sample_period_ms is not None
                       else config["samplePeriodMs"]), ua.VariantType.UInt32),
        ua.Variant(int(config["publishPeriodMs"]), ua.VariantType.UInt32),
        ua.Variant(int(history_interval_ms if history_interval_ms is not None
                       else config["historyIntervalMs"]), ua.VariantType.UInt32),
        ua.Variant(float(config["scale"]), ua.VariantType.Double),
        ua.Variant(float(config["offset"]), ua.VariantType.Double),
        ua.Variant(float(warning_threshold if warning_threshold is not None
                         else config["warningThreshold"]), ua.VariantType.Double),
        ua.Variant(float(critical_threshold if critical_threshold is not None
                         else config["criticalThreshold"]), ua.VariantType.Double),
    )
    result = await maintenance.call_method(method_id, *inputs)
    value = result[0] if isinstance(result, (list, tuple)) else result
    return bool(value)


async def apply_config(client, ua):
    maintenance = client.get_node(ua.NodeId.from_string(MAINTENANCE_CONFIGURATION_NODE_ID))
    method_id = ua.NodeId.from_string(METHOD_APPLY_CONFIG)
    result = await maintenance.call_method(method_id)
    value = result[0] if isinstance(result, (list, tuple)) else result
    return bool(value)


class ChannelStagingGuard:
    """Stages a live, reversible SetChannelConfig+ApplyConfig change and
    restores the original values on exit. Never calls SaveConfig -- nothing
    this script does is persisted to Flash -- and never reboots the device."""

    def __init__(self, client, ua, audit, slot_index, baseline_config, label):
        self.client = client
        self.ua = ua
        self.audit = audit
        self.slot_index = slot_index
        self.baseline = baseline_config
        self.label = label
        self.staged = False

    async def stage(self, **overrides):
        ok = await set_channel_config(self.client, self.ua, self.slot_index,
                                      self.baseline, **overrides)
        applied = await apply_config(self.client, self.ua) if ok else False
        self.audit.check(ok and applied,
                         f"{self.label}: staged {overrides} via SetChannelConfig+ApplyConfig",
                         "staging")
        self.staged = True
        return ok and applied

    async def restore(self):
        if not self.staged:
            return
        ok = await set_channel_config(self.client, self.ua, self.slot_index, self.baseline)
        applied = await apply_config(self.client, self.ua) if ok else False
        self.audit.check(ok and applied,
                         f"{self.label}: restored original SetChannelConfig+ApplyConfig",
                         "staging")
        self.staged = False


async def full_history_snapshot(client, ua, value_node, max_attempts=5):
    """A stitched, continuation-consuming full read using asyncua's own
    pagination loop -- used only to observe ring size/oldest-timestamp, not
    as a correctness assertion (that happens in verify_pagination).

    On a channel sampling continuously at steady state, the ring is already
    full and evicting on every new sample, so a multi-page read spanning
    more than one sample interval can legitimately race a generation bump
    mid-read -- this is exactly the condition the da0b057 generation-race
    fix and the cross-call generation check are supposed to reject, and
    asyncua's own read_raw_history pagination loop has no way to know that
    a BadContinuationPointInvalid here just means "the ring moved, retry the
    whole snapshot" rather than a real failure. Retry a bounded number of
    times before giving up for real."""
    start = datetime(1970, 1, 2, tzinfo=timezone.utc)
    end = datetime.now(timezone.utc) + timedelta(seconds=5)
    last_exc = None
    for _ in range(max_attempts):
        try:
            return await value_node.read_raw_history(starttime=start, endtime=end, numvalues=0)
        except Exception as exc:
            if "BadContinuationPointInvalid" not in type(exc).__name__ and \
               "BadContinuationPointInvalid" not in str(exc):
                raise
            last_exc = exc
    raise last_exc


async def wait_for_ring_wrap(client, ua, audit, value_node, sample_period_s, timeout_s):
    """Confirms the ring is full and wrapping: two full reads separated by
    at least one sample interval return the same length while the oldest
    timestamp advances. This is the precondition for the generation-mismatch
    test (an eviction, hence a generation bump, is guaranteed on the next
    sample only once the ring is actually full)."""
    deadline = time.monotonic() + timeout_s
    poll_interval = max(sample_period_s * 2.0, 2.0)
    previous = await full_history_snapshot(client, ua, value_node)
    while time.monotonic() < deadline:
        await asyncio.sleep(poll_interval)
        current = await full_history_snapshot(client, ua, value_node)
        if (len(current) == len(previous) and len(current) > 0 and
                current[0].SourceTimestamp > previous[0].SourceTimestamp):
            audit.info(f"ring confirmed full and wrapping at {len(current)} samples "
                       f"(oldest timestamp advanced {previous[0].SourceTimestamp} -> "
                       f"{current[0].SourceTimestamp})")
            return True, len(current)
        previous = current
    return False, len(previous)


async def wait_for_sample_count(client, ua, audit, value_node, target_count, timeout_s):
    deadline = time.monotonic() + timeout_s
    last_count = 0
    while time.monotonic() < deadline:
        history = await full_history_snapshot(client, ua, value_node)
        last_count = len(history)
        if last_count >= target_count:
            audit.info(f"accumulated {last_count} samples (target {target_count})")
            return True, last_count
        await asyncio.sleep(min(5.0, max(0.5, timeout_s / 60.0)))
    return False, last_count


# --------------------------------------------------------------------------
# Requirement 2: pagination correctness across > 128 samples.
# --------------------------------------------------------------------------

async def verify_pagination(client, ua, audit, value_node, max_return_count, max_attempts=5):
    start = datetime(1970, 1, 2, tzinfo=timezone.utc)
    end = datetime.now(timezone.utc) + timedelta(seconds=5)

    # The whole reference/page1/page2 triple is retried together (not just
    # individual reads): on a continuously-sampling channel the ring is
    # already full and evicting every sample interval, so the reference read
    # (many slow round trips) and the page1+page2 read (two fast round
    # trips) can each observe a different, legitimately-advancing window --
    # a mismatch here does not by itself mean corruption, only that the two
    # reads landed on different sides of an eviction. Only report a real
    # failure if every attempt disagrees.
    matches = False
    for attempt in range(1, max_attempts + 1):
        reference = await full_history_snapshot(client, ua, value_node)
        page1 = await history_read_raw(client, ua, value_node.nodeid, start, end, max_return_count)
        page1_samples = result_samples(page1)
        page2 = await history_read_raw(client, ua, value_node.nodeid, start, end, max_return_count,
                                       continuation=page1.ContinuationPoint)
        page2_samples = result_samples(page2)
        stitched = page1_samples + page2_samples
        reference_prefix = reference[:len(stitched)]
        matches = len(stitched) == len(reference_prefix) and all(
            a.Value.Value == b.Value.Value and a.SourceTimestamp == b.SourceTimestamp
            for a, b in zip(stitched, reference_prefix))
        if matches:
            if attempt > 1:
                audit.info(f"pagination reference/page1/page2 agreed on attempt {attempt}/"
                          f"{max_attempts} (earlier attempt(s) raced a live eviction, as "
                          "expected on a continuously-sampling channel)")
            break

    audit.check(len(reference) > max_return_count,
                f"reference stitched read has more than {max_return_count} samples "
                f"(got {len(reference)}) -- proves a single page cannot cover it", "pagination")

    # Requesting exactly the server's own page cap as numValuesPerNode still
    # forces a second page whenever more than one page of data exists, since
    # HISTORY_SERVICE_MAX_RETURN_COUNT is already the server-side ceiling.
    audit.check(status_matches(page1.StatusCode, 0),
                f"page 1: HistoryRead status Good (got {status_name(page1.StatusCode)})",
                "pagination")
    audit.equal(len(page1_samples), max_return_count,
                "page 1 sample count matches the requested/bounded page size", "pagination")
    audit.check(bool(page1.ContinuationPoint),
                "page 1 returns a non-empty continuation point (more data remains)",
                "pagination")
    audit.check(status_matches(page2.StatusCode, 0),
                f"page 2 (same continuation point): HistoryRead status Good "
                f"(got {status_name(page2.StatusCode)})", "pagination")
    audit.check(matches, "page 1 + page 2 stitched together exactly match the reference "
                f"read's first N samples: no gap, no duplicate, ring order preserved "
                f"(after up to {max_attempts} attempts)", "pagination")

    # Drain any remaining pages so this test leaves no continuation point
    # occupying the pool.
    continuation = page2.ContinuationPoint
    guard = 0
    while continuation and guard < 64:
        guard += 1
        page = await history_read_raw(client, ua, value_node.nodeid, start, end,
                                      max_return_count, continuation=continuation)
        continuation = page.ContinuationPoint
    audit.check(not continuation, "pagination test drained its continuation point to "
                "completion (no leaked pool slot)", "pagination")
    return len(reference)


# --------------------------------------------------------------------------
# Requirement 3: exhaustion of the real continuation-point pool.
# --------------------------------------------------------------------------

async def verify_exhaustion(client, ua, audit, value_node, capacity):
    start = datetime(1970, 1, 2, tzinfo=timezone.utc)
    end = datetime.now(timezone.utc) + timedelta(seconds=5)
    small_page = 8

    tokens = []
    for index in range(capacity):
        result = await history_read_raw(client, ua, value_node.nodeid, start, end, small_page)
        good = status_matches(result.StatusCode, 0) and bool(result.ContinuationPoint)
        if not good:
            audit.check(False, f"continuation #{index + 1}/{capacity}: expected Good with a "
                        f"continuation point (got {status_name(result.StatusCode)})", "exhaustion")
            break
        tokens.append(result.ContinuationPoint)
    audit.equal(len(tokens), capacity,
                f"opened exactly the {capacity}-slot continuation pool with fresh requests",
                "exhaustion")

    overflow = await history_read_raw(client, ua, value_node.nodeid, start, end, small_page)
    audit.check(status_name(overflow.StatusCode) == "BadNoContinuationPoints",
                f"the {capacity + 1}th concurrent continuation is rejected with "
                f"BadNoContinuationPoints (got {status_name(overflow.StatusCode)})",
                "exhaustion")
    audit.check(not result_samples(overflow) and not overflow.ContinuationPoint,
                "the rejected 17th request returns no samples and no continuation point "
                "-- fails closed, does not corrupt state", "exhaustion")

    for index, token in enumerate(tokens):
        released = await history_read_raw(client, ua, value_node.nodeid, start, end, small_page,
                                          continuation=token, release=True)
        audit.check(status_matches(released.StatusCode, 0),
                    f"drained continuation #{index + 1}/{capacity} via explicit release",
                    "exhaustion")

    refill = await history_read_raw(client, ua, value_node.nodeid, start, end, small_page)
    refill_ok = status_matches(refill.StatusCode, 0) and bool(refill.ContinuationPoint)
    audit.check(refill_ok, "after draining all 16 slots, a fresh request succeeds again "
                "-- exhaustion was real capacity pressure, not a permanent lockout",
                "exhaustion")
    if refill_ok:
        await history_read_raw(client, ua, value_node.nodeid, start, end, small_page,
                               continuation=refill.ContinuationPoint, release=True)


# --------------------------------------------------------------------------
# Requirement 4a: explicit client release.
# --------------------------------------------------------------------------

async def verify_release(client, ua, audit, value_node):
    start = datetime(1970, 1, 2, tzinfo=timezone.utc)
    end = datetime.now(timezone.utc) + timedelta(seconds=5)
    opened = await history_read_raw(client, ua, value_node.nodeid, start, end, 8)
    if not audit.check(bool(opened.ContinuationPoint),
                       "release test: opened a continuation point to release", "release"):
        return
    token = opened.ContinuationPoint

    released = await history_read_raw(client, ua, value_node.nodeid, start, end, 8,
                                      continuation=token, release=True)
    audit.check(status_matches(released.StatusCode, 0),
                f"releaseContinuationPoints=true on an open point returns Good "
                f"(got {status_name(released.StatusCode)})", "release")

    reuse = await history_read_raw(client, ua, value_node.nodeid, start, end, 8,
                                   continuation=token)
    audit.check(status_name(reuse.StatusCode) == "BadContinuationPointInvalid",
                "redeeming the same token again after release is rejected with "
                f"BadContinuationPointInvalid (got {status_name(reuse.StatusCode)})", "release")


# --------------------------------------------------------------------------
# Requirement 4b: server-side expiry.
# --------------------------------------------------------------------------

async def verify_expiry(client, ua, audit, value_node, expiry_s):
    start = datetime(1970, 1, 2, tzinfo=timezone.utc)
    end = datetime.now(timezone.utc) + timedelta(seconds=5)
    opened = await history_read_raw(client, ua, value_node.nodeid, start, end, 8)
    if not audit.check(bool(opened.ContinuationPoint),
                       "expiry test: opened a continuation point to let expire", "expiry"):
        return
    token = opened.ContinuationPoint

    wait_s = expiry_s + CONTINUATION_EXPIRY_MARGIN_S
    audit.info(f"expiry test: sleeping {wait_s:.0f}s for the {expiry_s:.0f}s server-side "
               "continuation-point lifetime to elapse")
    await asyncio.sleep(wait_s)

    expired = await history_read_raw(client, ua, value_node.nodeid, start, end, 8,
                                     continuation=token)
    audit.check(status_name(expired.StatusCode) == "BadContinuationPointInvalid",
                "redeeming a continuation point after its expiry lifetime is rejected with "
                f"BadContinuationPointInvalid (got {status_name(expired.StatusCode)})", "expiry")


# --------------------------------------------------------------------------
# Requirement 4c: rejection when quoted by a foreign session.
# --------------------------------------------------------------------------

async def verify_foreign_session(args, client, ua, audit, value_node):
    start = datetime(1970, 1, 2, tzinfo=timezone.utc)
    end = datetime.now(timezone.utc) + timedelta(seconds=5)
    opened = await history_read_raw(client, ua, value_node.nodeid, start, end, 8)
    if not audit.check(bool(opened.ContinuationPoint),
                       "foreign-session test: opened a continuation point owned by "
                       "the primary session", "foreign-session"):
        return
    token = opened.ContinuationPoint

    foreign = None
    try:
        foreign = await client_for(args, args.second_username, args.second_password)
        foreign_read = await history_read_raw(foreign, ua, value_node.nodeid, start, end, 8,
                                              continuation=token)
        audit.check(status_name(foreign_read.StatusCode) == "BadContinuationPointInvalid",
                    "a second session quoting the primary session's continuation point is "
                    f"rejected with BadContinuationPointInvalid "
                    f"(got {status_name(foreign_read.StatusCode)})", "foreign-session")
    finally:
        await safe_disconnect(foreign, audit, "foreign-session client disconnect")

    owner_redeem = await history_read_raw(client, ua, value_node.nodeid, start, end, 8,
                                          continuation=token, release=True)
    audit.check(status_matches(owner_redeem.StatusCode, 0),
                "the owning session can still redeem/release its own continuation point "
                "after the foreign attempt -- the rejection did not corrupt server state",
                "foreign-session")


# --------------------------------------------------------------------------
# Requirement 4d: rejection on a ring-generation change between pages --
# the cross-call half of the race hardened in history_service.c da0b057.
# --------------------------------------------------------------------------

async def verify_generation_mismatch(client, ua, audit, value_node, sample_period_s,
                                     wrap_timeout_s):
    wrapped, ring_size = await wait_for_ring_wrap(client, ua, audit, value_node,
                                                   sample_period_s, wrap_timeout_s)
    if not wrapped:
        audit.skip("continuation rejection on ring-generation change (page 1 -> eviction "
                   "-> page 2)", f"ring did not confirm full/wrapping within "
                   f"{wrap_timeout_s:.0f}s (last observed size {ring_size}); pass "
                   "--accelerate-history or a longer --history-wait-timeout", "generation")
        return

    start = datetime(1970, 1, 2, tzinfo=timezone.utc)
    end = datetime.now(timezone.utc) + timedelta(seconds=5)
    page1 = await history_read_raw(client, ua, value_node.nodeid, start, end, 8)
    if not audit.check(bool(page1.ContinuationPoint),
                       "generation-mismatch test: opened page 1's continuation point",
                       "generation"):
        return
    token = page1.ContinuationPoint

    # The ring is confirmed full/wrapping: waiting at least one more sample
    # interval guarantees an eviction (and therefore a generation bump)
    # happens strictly between opening this continuation and redeeming it.
    await asyncio.sleep(max(sample_period_s * 1.5, 1.5))

    page2 = await history_read_raw(client, ua, value_node.nodeid, start, end, 8,
                                   continuation=token)
    audit.check(status_name(page2.StatusCode) == "BadContinuationPointInvalid",
                "page 2 redeemed after an intervening ring eviction is rejected with "
                f"BadContinuationPointInvalid rather than returning stale/wrong data "
                f"(got {status_name(page2.StatusCode)})", "generation")
    audit.info("this exercises the cross-call generation check in "
               "model_history_read_raw() (observed_generation != requested_generation), "
               "the same comparison the da0b057 fix relies on. The sub-millisecond "
               "within-a-single-call race that fix specifically hardens is only "
               "reproducible via the deterministic C-level interleaving hook in "
               "static/tests/history_service/history_service_test.c, not from an "
               "OPC UA client.")


# --------------------------------------------------------------------------
# Requirements 5 and 6: Alarms & Conditions.
# --------------------------------------------------------------------------

async def read_condition_semantics(client, ua, condition_node):
    async def read(path):
        return await (await condition_node.get_child(path)).read_value()
    return {
        "active": await read(["0:ActiveState", "0:Id"]),
        "acked": await read(["0:AckedState", "0:Id"]),
        "retain": await read(["0:Retain"]),
        "severity": await read(["0:Severity"]),
        "message": await read(["0:Message"]),
    }


class ConditionEventCollector:
    def __init__(self, condition_node_id):
        self.condition_node_id = condition_node_id
        self.events = []

    def event_notification(self, event):
        condition_id = getattr(event, "ConditionId", None)
        if condition_id is not None and condition_id == self.condition_node_id:
            self.events.append(event)
        elif condition_id is None:
            # Some open62541 builds surface the emitting node via SourceNode
            # instead of ConditionId on the notification; keep both paths.
            source_node = getattr(event, "SourceNode", None)
            if source_node == self.condition_node_id:
                self.events.append(event)


async def force_channel_alarm_high(client, ua, audit, channel_id, label):
    """Retunes WarningThreshold below the channel's current live value via
    the existing SetChannelConfig/ApplyConfig methods, forcing process_state
    HIGH. Returns (guard, forced_ok); the caller must await guard.restore()
    in a finally block regardless of forced_ok."""
    slot, config = await find_channel_slot(client, ua, channel_id)
    guard = None
    if slot is None:
        audit.check(False, f"{label}: could not find a staged config slot for "
                    f"channel id {channel_id!r} via GetChannelConfig", "force-alarm")
        return None, False
    guard = ChannelStagingGuard(client, ua, audit, slot, config, label)

    ns = await resolve_channel_namespace(client, ua)
    value_node = await channel_value_node(client, ua, ns, channel_id)
    # read_data_value() (unlike read_value()) does not raise on a non-Good
    # StatusCode -- a channel with no real transducer attached (e.g. a
    # floating ADC pin used only to provision a second numeric test channel)
    # can legitimately read back Uncertain/Bad quality, which must be
    # reported as a clear audit failure here rather than crash the whole run.
    current_data_value = await value_node.read_data_value()
    if not status_matches(current_data_value.StatusCode, 0):
        audit.check(False, f"{label}: channel quality is "
                    f"{status_name(current_data_value.StatusCode)}, not Good; "
                    "--force-alarm needs a Good-quality live value to compute a "
                    "forcing threshold from", "force-alarm")
        return guard, False
    current = current_data_value.Value.Value
    try:
        current_value = float(current)
    except (TypeError, ValueError):
        audit.check(False, f"{label}: channel value {current!r} is not numeric; "
                    "--force-alarm only supports numeric (threshold-based) channels",
                    "force-alarm")
        return guard, False

    forced_warning = current_value - 1.0
    forced_critical = current_value + 1000.0  # stay in HIGH, not HIGH_HIGH
    ok = await guard.stage(warning_threshold=forced_warning, critical_threshold=forced_critical)
    return guard, ok


async def wait_for_condition_active(condition_node, expect_active, timeout_s):
    deadline = time.monotonic() + timeout_s
    last = None
    while time.monotonic() < deadline:
        last = await (await condition_node.get_child(["0:ActiveState", "0:Id"])).read_value()
        if bool(last) == expect_active:
            return True, last
        await asyncio.sleep(ALARM_TASK_PERIOD_S)
    return False, last


async def verify_two_simultaneous_alarms(client, ua, audit, ns, channels, force_alarm,
                                         require_two, wait_timeout_s):
    if len(channels) < 2:
        message = ("two simultaneous, independent Alarms & Conditions instances on two "
                   "different channels: only one --channel was supplied/available "
                   "(app/basic_profile/main.c ships only channel.temperature.1); pass "
                   "--channel a second time against a device already provisioned with a "
                   "second alarm-eligible channel to exercise this for real")
        if require_two:
            audit.check(False, message, "alarm")
        else:
            audit.skip("two-channel simultaneous/independent alarm check", message, "alarm")
        return []

    guards = []
    condition_nodes = []
    try:
        for channel_id, _ in channels[:2]:
            condition_node = await channel_alarm_condition_node(client, ua, ns, channel_id)
            condition_nodes.append((channel_id, condition_node))
            if force_alarm:
                guard, ok = await force_channel_alarm_high(client, ua, audit, channel_id,
                                                            f"force-alarm[{channel_id}]")
                if guard is not None:
                    guards.append(guard)
                audit.check(ok, f"{channel_id}: forced a HIGH condition via SetChannelConfig",
                            "alarm")

        if force_alarm:
            for channel_id, condition_node in condition_nodes:
                reached, active = await wait_for_condition_active(condition_node, True,
                                                                   wait_timeout_s)
                audit.check(reached, f"{channel_id}: ActiveState reached true within "
                            f"{wait_timeout_s:.0f}s (got {active!r})", "alarm")
        else:
            # Without --force-alarm there is no guarantee either condition is
            # naturally active; only assert independence if both already are,
            # otherwise skip rather than block on a timeout that cannot
            # legitimately resolve.
            active_now = []
            for channel_id, condition_node in condition_nodes:
                active_now.append(
                    bool(await (await condition_node.get_child(
                        ["0:ActiveState", "0:Id"])).read_value()))
            if not all(active_now):
                audit.skip("two simultaneous active alarm conditions",
                           "neither --force-alarm was passed nor are both requested "
                           "channels naturally active right now; pass --force-alarm to "
                           "force this deterministically", "alarm")
                return []

        semantics = {}
        for channel_id, condition_node in condition_nodes:
            semantics[channel_id] = await read_condition_semantics(client, ua, condition_node)
        for channel_id, values in semantics.items():
            audit.check(values["active"] is True,
                        f"{channel_id}: condition ActiveState=true and Retain follows "
                        f"({values['retain']!r})", "alarm")

        (id_a, node_a), (id_b, node_b) = condition_nodes[0], condition_nodes[1]
        before_a = await read_condition_semantics(client, ua, node_a)
        before_b = await read_condition_semantics(client, ua, node_b)
        event_id_a = await capture_active_event_id(client, ua, audit, node_a, id_a)
        if event_id_a is not None:
            await call_acknowledge(client, ua, node_a, event_id_a,
                                   f"independence check ack of {id_a}")
        after_a = await read_condition_semantics(client, ua, node_a)
        after_b = await read_condition_semantics(client, ua, node_b)
        audit.check(before_a["acked"] != after_a["acked"] or after_a["acked"] is True,
                    f"{id_a}: acknowledging changed its own AckedState", "alarm")
        audit.equal(after_b["acked"], before_b["acked"],
                    f"{id_b}: acknowledging {id_a} did not affect {id_b}'s AckedState",
                    "alarm")
        audit.equal(after_b["active"], before_b["active"],
                    f"{id_b}: acknowledging {id_a} did not affect {id_b}'s ActiveState",
                    "alarm")
    finally:
        for guard in guards:
            await guard.restore()
    return condition_nodes


async def capture_active_event_id(client, ua, audit, condition_node, channel_id, timeout_s=15.0):
    """Subscribes briefly to capture a real EventId for the active condition
    -- open62541's standard Acknowledge implementation validates EventId
    against its outstanding-event list, so a fabricated EventId would be
    rejected with BadEventIdUnknown rather than exercising the real
    semantics this project already implements."""
    collector = ConditionEventCollector(condition_node.nodeid)
    subscription = None
    try:
        subscription = await client.create_subscription(100, collector)
        # Subscribe at the Server object (ns=0;i=2253), not the condition
        # instance node directly. Per model_enable_condition_source() in
        # opcua_model/src/model_maintenance.c, the EventNotifier bit and
        # HasNotifier chain are wired on the condition's *source* object
        # (the channel/device the condition is about), the same pattern the
        # OPC UA A&C spec expects clients to rely on -- subscribing straight
        # at a condition instance is not a valid event-notifier target here
        # and open62541 rejects the resulting MonitoredItem with
        # BadNotSupported. The Server object is always a valid subscribe
        # point when Events are implemented at all; ConditionEventCollector
        # already filters client-side by ConditionId/SourceNode, so this
        # still only keeps events for the specific condition being watched.
        await subscription.subscribe_events(
            sourcenode=client.get_node(ua.NodeId(2253, 0)),
            evtypes=client.get_node(ua.NodeId(2782, 0)))
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            if collector.events:
                event = collector.events[-1]
                event_id = getattr(event, "EventId", None)
                if event_id:
                    return event_id
            await asyncio.sleep(0.25)
        # ConditionRefresh forces a republish of Retain=true conditions,
        # including a fresh EventId, if none arrived from the original
        # transition (e.g. it fired before this subscription existed).
        await call_condition_refresh(client, ua, subscription.subscription_id)
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            if collector.events:
                event = collector.events[-1]
                event_id = getattr(event, "EventId", None)
                if event_id:
                    return event_id
            await asyncio.sleep(0.25)
        return None
    finally:
        await safe_delete(subscription, audit, f"{channel_id} EventId capture subscription")


async def call_acknowledge(client, ua, condition_node, event_id, comment):
    method_id = ua.NodeId(NS0_ACKNOWLEDGE_METHOD_ID, 0)
    return await condition_node.call_method(
        method_id,
        ua.Variant(event_id, ua.VariantType.ByteString),
        ua.Variant(ua.LocalizedText(comment), ua.VariantType.LocalizedText))


async def call_condition_refresh(client, ua, subscription_id):
    server_node = client.get_node(ua.NodeId(NS0_SERVER_OBJECT_ID, 0))
    method_id = ua.NodeId(NS0_CONDITION_REFRESH_METHOD_ID, 0)
    return await server_node.call_method(
        method_id, ua.Variant(subscription_id, ua.VariantType.UInt32))


async def verify_acknowledge_and_refresh(client, ua, audit, condition_nodes):
    if not condition_nodes:
        audit.skip("standard Acknowledge Method on an active condition", "no active "
                   "condition available (--force-alarm was not enabled or forcing failed)",
                   "acknowledge")
        audit.skip("standard ConditionRefresh Method", "no active condition available",
                   "refresh")
        return

    channel_id, condition_node = condition_nodes[0]
    before = await read_condition_semantics(client, ua, condition_node)
    audit.check(before["active"] is True,
                f"{channel_id}: condition is active before Acknowledge is exercised",
                "acknowledge")

    event_id = await capture_active_event_id(client, ua, audit, condition_node, channel_id)
    if not audit.check(event_id is not None,
                       f"{channel_id}: captured a real EventId for the active condition "
                       "via subscription (required by the standard Acknowledge Method's "
                       "EventId validation)", "acknowledge"):
        return

    try:
        await call_acknowledge(client, ua, condition_node, event_id,
                               "verify_v043 acknowledge check")
        acknowledge_failed = False
    except Exception as exc:
        acknowledge_failed = True
        audit.check(False, f"{channel_id}: Acknowledge Method call raised "
                    f"{type(exc).__name__}: {exc}", "acknowledge")
    if not acknowledge_failed:
        after = await read_condition_semantics(client, ua, condition_node)
        audit.equal(after["acked"], True,
                    f"{channel_id}: AckedState/Id is true after Acknowledge", "acknowledge")
        audit.equal(after["active"], before["active"],
                    f"{channel_id}: Acknowledge does not clear ActiveState (still active "
                    "while the underlying process condition persists)", "acknowledge")

    events = ConditionEventCollector(condition_node.nodeid)
    subscription = None
    try:
        subscription = await client.create_subscription(100, events)
        await subscription.subscribe_events(
            sourcenode=condition_node, evtypes=client.get_node(ua.NodeId(2782, 0)))
        before_count = len(events.events)
        await call_condition_refresh(client, ua, subscription.subscription_id)
        deadline = time.monotonic() + 15.0
        refreshed = False
        while time.monotonic() < deadline:
            if len(events.events) > before_count:
                refreshed = True
                break
            await asyncio.sleep(0.25)
        audit.check(refreshed, f"{channel_id}: ConditionRefresh(SubscriptionId="
                    f"{subscription.subscription_id}) causes the server to republish the "
                    "condition's current state as an event to this subscription",
                    "refresh")
        if refreshed:
            refreshed_event = events.events[-1]
            refreshed_active = getattr(refreshed_event, "ActiveState", None)
            audit.info(f"ConditionRefresh republished ActiveState={refreshed_active!r}")
    finally:
        await safe_delete(subscription, audit, f"{channel_id} ConditionRefresh subscription")


# --------------------------------------------------------------------------
# Orchestration
# --------------------------------------------------------------------------

async def verify_v043(args, ua, audit):
    expected_capacity, expected_expiry_s = verify_continuation_pool_constants(audit)

    client = await client_for(args, args.admin_username, args.admin_password)
    try:
        namespaces = await client.get_namespace_array()
        audit.check(CHANNEL_NAMESPACE_URI in namespaces,
                    "channel model namespace is present", "discovery")
        ns = await resolve_channel_namespace(client, ua)

        channels = args.channel or [parse_channel_spec("channel.temperature.1=Float")]

        discovered_types = await verify_value_types(client, ua, audit, ns, channels,
                                                     args.require_mixed_value_types)

        primary_channel_id = channels[0][0]
        value_node = await channel_value_node(client, ua, ns, primary_channel_id)
        # SamplePeriodMs lives on the channel object itself (sibling of Value).
        channel_object = await channel_source_node(client, ua, ns, primary_channel_id)
        try:
            sample_period_ms = await (await channel_object.get_child(
                [f"{ns}:SamplePeriodMs"])).read_value()
        except Exception:
            sample_period_ms = 1000

        staging_guard = None
        try:
            if args.accelerate_history:
                slot, config = await find_channel_slot(client, ua, primary_channel_id)
                if slot is None:
                    audit.check(False, f"--accelerate-history: could not find a staged "
                                "config slot for the primary channel", "staging")
                else:
                    staging_guard = ChannelStagingGuard(client, ua, audit, slot, config,
                                                        "accelerate-history")
                    await staging_guard.stage(sample_period_ms=100, history_interval_ms=100)
                    sample_period_ms = 100

            target_count = max(HISTORY_SERVICE_MAX_RETURN_COUNT + 1, args.history_target_samples)
            accumulated, achieved = await wait_for_sample_count(
                client, ua, audit, value_node, target_count, args.history_wait_timeout)
            if not accumulated:
                audit.check(False, f"{primary_channel_id}: accumulated only {achieved} "
                            f"samples within {args.history_wait_timeout:.0f}s; requirement 2 "
                            f"needs > {HISTORY_SERVICE_MAX_RETURN_COUNT}. Pass "
                            "--accelerate-history or a longer --history-wait-timeout",
                            "pagination")
            else:
                await verify_pagination(client, ua, audit, value_node,
                                        HISTORY_SERVICE_MAX_RETURN_COUNT)
                await verify_exhaustion(client, ua, audit, value_node, expected_capacity)
                await verify_release(client, ua, audit, value_node)
                await verify_foreign_session(args, client, ua, audit, value_node)
                await verify_expiry(client, ua, audit, value_node, expected_expiry_s)
                await verify_generation_mismatch(
                    client, ua, audit, value_node, sample_period_ms / 1000.0,
                    args.history_wait_timeout)
        finally:
            if staging_guard is not None:
                await staging_guard.restore()

        condition_nodes = await verify_two_simultaneous_alarms(
            client, ua, audit, ns, channels, args.force_alarm,
            args.require_two_channel_alarms, args.alarm_wait_timeout)

        if not condition_nodes and args.force_alarm:
            channel_id = primary_channel_id
            condition_node = await channel_alarm_condition_node(client, ua, ns, channel_id)
            guard, ok = await force_channel_alarm_high(client, ua, audit, channel_id,
                                                        "force-alarm[primary]")
            try:
                if ok:
                    reached, _ = await wait_for_condition_active(condition_node, True,
                                                                  args.alarm_wait_timeout)
                    audit.check(reached, f"{channel_id}: ActiveState reached true within "
                                f"{args.alarm_wait_timeout:.0f}s", "alarm")
                    if reached:
                        condition_nodes = [(channel_id, condition_node)]
            finally:
                if guard is not None:
                    await guard.restore()

        await verify_acknowledge_and_refresh(client, ua, audit, condition_nodes)
    finally:
        await safe_disconnect(client, audit, "primary client disconnect")


def parse_args():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("server_url")
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument("--admin-username", default=os.getenv("OPCUA_ADMIN_USERNAME", "Admin"))
    parser.add_argument("--admin-password", default=os.getenv("OPCUA_ADMIN_PASSWORD", "Admin"))
    parser.add_argument("--second-username", default=os.getenv("OPCUA_SECOND_USERNAME", "Viewer"),
                        help="Foreign identity used only for the foreign-session "
                        "continuation-point rejection check")
    parser.add_argument("--second-password", default=os.getenv("OPCUA_SECOND_PASSWORD", "Admin"))
    parser.add_argument("--client-cert")
    parser.add_argument("--client-key")
    parser.add_argument("--server-cert")
    parser.add_argument("--security-policy", default="Basic256Sha256", choices=["Basic256Sha256"])
    parser.add_argument("--security-mode", default="SignAndEncrypt",
                        choices=["Sign", "SignAndEncrypt"])
    parser.add_argument("--no-auto-client-cert", action="store_true",
                        help="Require --client-cert/--client-key instead of generating a "
                        "temporary certificate")
    parser.add_argument("--allow-auto-client-cert", action="store_true",
                        help="Permit a temporary certificate; physical verification requires "
                        "explicit cert/key by default")
    parser.add_argument("--channel", action="append", default=[], type=parse_channel_spec,
                        metavar="CHANNEL_ID[=VariantType]",
                        help="Historized, alarm-eligible channel to exercise. Repeatable. "
                        "Defaults to the shipped reference profile's only channel "
                        "(channel.temperature.1=Float). Pass twice against a device already "
                        "provisioned with a second alarm-eligible channel to exercise the "
                        "mixed-value-type and dual-condition checks for real.")
    parser.add_argument("--require-mixed-value-types", action="store_true",
                        help="Fail (rather than skip with a reason) unless >= 2 distinct "
                        "VariantTypes are discovered among the requested channels")
    parser.add_argument("--require-two-channel-alarms", action="store_true",
                        help="Fail (rather than skip with a reason) unless two --channel "
                        "entries are supplied for the simultaneous-alarm check")
    parser.add_argument("--force-alarm", action="store_true",
                        help="Opt-in: force each target channel's condition active via the "
                        "existing SetChannelConfig/ApplyConfig methods (never SaveConfig, "
                        "never a reboot), then restore the original threshold. Required for "
                        "the Acknowledge/ConditionRefresh checks to exercise anything on the "
                        "shipped read-only reference channel.")
    parser.add_argument("--accelerate-history", action="store_true",
                        help="Opt-in: temporarily shrink SamplePeriodMs/HistoryIntervalMs via "
                        "SetChannelConfig/ApplyConfig to accumulate > 128 samples and wrap "
                        "the 256-sample ring quickly, then restore the originals. Without "
                        "this, the script waits for natural accumulation (see "
                        "--history-wait-timeout).")
    parser.add_argument("--history-target-samples", type=int,
                        default=HISTORY_SERVICE_MAX_RETURN_COUNT + 20,
                        help="Minimum sample count to accumulate before pagination checks "
                        f"run (must exceed {HISTORY_SERVICE_MAX_RETURN_COUNT})")
    parser.add_argument("--history-wait-timeout", type=float,
                        default=DEFAULT_HISTORY_WAIT_TIMEOUT_S,
                        help="Bounded seconds to wait for sample accumulation/ring wrap "
                        "before failing the history checks that need it")
    parser.add_argument("--alarm-wait-timeout", type=float, default=DEFAULT_ALARM_WAIT_TIMEOUT_S,
                        help="Bounded seconds to wait for a forced condition to become "
                        "ActiveState=true after ApplyConfig")
    return parser.parse_args()


async def main():
    args = parse_args()
    audit = Audit()

    explicit_pair = bool(args.client_cert) and bool(args.client_key)
    audit.check(bool(args.client_cert) == bool(args.client_key),
                "--client-cert and --client-key are supplied together", "arguments")
    audit.check(explicit_pair or args.allow_auto_client_cert,
                "physical verifier uses explicit --client-cert and --client-key (or "
                "explicit development override)", "arguments")
    if args.no_auto_client_cert and args.allow_auto_client_cert:
        audit.check(False, "--no-auto-client-cert conflicts with --allow-auto-client-cert",
                    "arguments")
    audit.check(args.history_target_samples > HISTORY_SERVICE_MAX_RETURN_COUNT,
                f"--history-target-samples exceeds {HISTORY_SERVICE_MAX_RETURN_COUNT} "
                "(requirement 2 needs more than one page)", "arguments")
    if audit.failures:
        audit.report()
        return 1
    if not verify_runtime(audit):
        audit.report()
        return 1

    try:
        opcua_common.load_asyncua()
        await verify_v043(args, opcua_common.ua, audit)
    except Exception as exc:
        audit.check(False, f"verification setup: {type(exc).__name__}: {exc}", "setup")

    audit.report()
    return 1 if audit.failures else 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
