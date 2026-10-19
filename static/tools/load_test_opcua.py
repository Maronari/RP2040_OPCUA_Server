#!/usr/bin/env python3
"""Bounded OPC UA staged-load and v0.5.2 benchmark client.

The default mode retains the staged, read-only load ramp. ``--benchmark`` is
strictly read-only: it verifies the selected T4 metadata and live ExportConfig
before collecting metrics. It never mutates configuration or channel values.
"""
from __future__ import annotations

import argparse
import asyncio
import getpass
import hashlib
import json
import math
import os
import statistics
import time
from dataclasses import asdict, dataclass, field
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Any
from importlib.metadata import PackageNotFoundError, version

import benchmark_profiles
import stage_config as opcua_common
import verify_v052_opcua_resources as resource_contract


CONTRACT_PATH = Path(__file__).resolve().parents[1] / "benchmarks/v0.5.2/measurement-contract.json"
MAX_METADATA_BYTES = 1024 * 1024
MAX_NATIVE_EVIDENCE_BYTES = 1024 * 1024
RESULT_STATES = {"measured", "unsupported", "unavailable", "exhausted", "failed", "not-run"}
CHANNEL_NAMESPACE_URI = "urn:maronari:rp2-industrial-opcua:channel-model"
MONITORED_NODE_IDS = (
    "ns=1;s=Device.Diagnostics.UptimeSec", "ns=1;s=Device.Diagnostics.FreeHeap",
    "ns=1;s=Device.Diagnostics.MinEverFreeHeap", "ns=1;s=Device.Diagnostics.PSRAM.UsedBytes",
    "ns=1;s=Device.Diagnostics.PSRAM.FreeBytes", "ns=1;s=Device.Diagnostics.PSRAM.LargestFreeBlockBytes",
    "ns=1;s=Device.Diagnostics.CpuLikeLoadApprox", "ns=1;s=Device.Network.LinkUp",
    "ns=1;s=Device.Network.IPv4", "ns=1;s=Device.Time.TimeSynced",
    "ns=1;s=Device.Time.LastSyncEpoch", "ns=1;s=Device.DeviceHealth",
    "ns=1;s=Device.Configuration.SamplingPeriodMs", "ns=1;s=Device.Configuration.PublishPeriodMs",
    "ns=1;s=Device.Security.FailedLoginCount", "ns=1;s=Device.Security.DaysUntilExpiry",
)
RESOURCE_DIAGNOSTIC_NODE_IDS = resource_contract.RESOURCE_NODE_IDS
RESOURCE_DIAGNOSTIC_PHASE = "post-steady-before-cleanup"
# Fixed-rate benchmark arrivals are bounded independently of the server's
# timeout. At the frozen maximum of four external clients this permits 64
# outstanding service calls. The clean maximum-load run peaked at 12 calls in
# flight despite a 1.184-second maximum latency; a saturated client records
# missed arrivals instead of growing without bound.
MAX_READS_IN_FLIGHT_PER_CLIENT = 16

DIAGNOSTIC_NODE_IDS = {
    "uptime_s": "ns=1;s=Device.Diagnostics.UptimeSec", "free_heap": "ns=1;s=Device.Diagnostics.FreeHeap",
    "min_free_heap": "ns=1;s=Device.Diagnostics.MinEverFreeHeap", "psram_used": "ns=1;s=Device.Diagnostics.PSRAM.UsedBytes",
    "psram_free": "ns=1;s=Device.Diagnostics.PSRAM.FreeBytes", "psram_largest": "ns=1;s=Device.Diagnostics.PSRAM.LargestFreeBlockBytes",
    "cpu_load_approx": "ns=1;s=Device.Diagnostics.CpuLikeLoadApprox",
}


def contract_bounds() -> dict[str, int]:
    return {key: int(value) for key, value in json.loads(CONTRACT_PATH.read_text(encoding="utf-8"))["bounds"].items()}


BOUNDS = contract_bounds()


def rounded(value: float | None) -> float | None:
    return round(value, 3) if value is not None else None


def percentile(values: list[float], fraction: float) -> float | None:
    if not values:
        return None
    ordered = sorted(values)
    return ordered[max(0, min(len(ordered) - 1, math.ceil(fraction * len(ordered)) - 1))]


class BoundedSamples:
    """No reservoir: overflow is explicitly accounted and fails the run."""
    def __init__(self, capacity: int) -> None:
        self.capacity, self.values, self.overflow = capacity, [], 0

    def append(self, value: float) -> bool:
        if not math.isfinite(value) or value < 0:
            raise ValueError("samples must be finite non-negative values")
        if len(self.values) == self.capacity:
            self.overflow += 1
            return False
        self.values.append(value)
        return True

    def report(self) -> dict[str, Any]:
        return {"sampleCount": len(self.values), "droppedSampleCount": self.overflow,
                "p50": rounded(statistics.median(self.values)) if self.values else None,
                "p95": rounded(percentile(self.values, .95)), "p99": rounded(percentile(self.values, .99)),
                "max": rounded(max(self.values)) if self.values else None}


class ErrorLog:
    def __init__(self, capacity: int, text_bytes: int) -> None:
        self.capacity, self.text_bytes, self.items, self.overflow = capacity, text_bytes, [], 0

    def add(self, context: str, exc: BaseException | str) -> None:
        if len(self.items) == self.capacity:
            self.overflow += 1
            return
        detail = exc if isinstance(exc, str) else safe_exception(exc)
        self.items.append(f"{context}: {detail}".encode("utf-8")[:self.text_bytes].decode("utf-8", "replace"))


def safe_exception(exc: BaseException) -> str:
    """Keep endpoint credentials and server-provided strings out of artifacts."""
    status = getattr(exc, "code", None)
    if status is None:
        return type(exc).__name__
    return f"{type(exc).__name__}:{getattr(status, 'name', getattr(status, 'value', type(status).__name__))}"


@dataclass
class StageResult:
    clients: int
    items_per_client: int
    duration_s: float
    subscriptions_created: int = 0
    monitored_items_created: int = 0
    notifications: int = 0
    read_operations: int = 0
    read_errors: int = 0
    connection_errors: int = 0
    latency_ms_p50: float | None = None
    latency_ms_p95: float | None = None
    latency_ms_p99: float | None = None
    latency_ms_max: float | None = None
    diagnostics_before: dict = field(default_factory=dict)
    diagnostics_after: dict = field(default_factory=dict)
    reboot_detected: bool = False
    sample_overflow: int = 0
    errors: list[str] = field(default_factory=list)


class SubscriptionHandler:
    def __init__(self) -> None:
        self.notifications = 0
    def datachange_notification(self, _node: Any, _value: Any, _data: Any) -> None:
        self.notifications += 1
    def status_change_notification(self, status: Any) -> None:
        print(f"[subscription] status changed: {status}")


async def configure_client(args: argparse.Namespace) -> Any:
    opcua_common.load_asyncua()
    client = opcua_common.Client(url=args.server_url, timeout=args.timeout, watchdog_intervall=args.timeout)
    if not getattr(args, "test_none", False):
        await opcua_common.configure_client_security(client, args)
    if args.username:
        client.set_user(args.username)
        client.set_password(args.password or "")
    return client


async def read_diagnostics(client: Any) -> dict[str, Any]:
    snapshot = {}
    for name, node_id in DIAGNOSTIC_NODE_IDS.items():
        try:
            snapshot[name] = await client.get_node(node_id).read_value()
        except Exception as exc:
            snapshot[name] = f"ERROR:{type(exc).__name__}:{exc}"
    return snapshot


async def validate_monitored_nodes(client: Any, node_ids: list[str] | tuple[str, ...]) -> list[str]:
    usable = []
    for node_id in node_ids:
        try:
            await client.get_node(node_id).read_value()
            usable.append(node_id)
        except Exception as exc:
            print(f"[WARN] Skipping {node_id}: {type(exc).__name__}: {exc}")
    if not usable:
        raise RuntimeError("No readable nodes are available for monitoring")
    return usable


def channel_value_node_id(channel_id: str, namespace_index: int) -> str:
    encoded = channel_id.encode("utf-8").hex().upper()
    return f"ns={namespace_index};s=Device.Channels.c_{encoded}.Value"


def resource_diagnostic_status(status: Any) -> str:
    """Return only the frozen status spellings that are safe for a report."""
    name = resource_contract.status_name(status)
    return name if name in {"Good", "BadNotSupported", "BadUserAccessDenied"} else "Unknown"


async def capture_resource_diagnostics(client: Any, timeout_s: float) -> dict[str, Any]:
    """Read the frozen T3 surface once after steady load, without a new session."""
    if len(RESOURCE_DIAGNOSTIC_NODE_IDS) != 22 or len(set(RESOURCE_DIAGNOSTIC_NODE_IDS)) != 22:
        raise RuntimeError("resource diagnostic contract is not the frozen 22-node set")

    async def read_one(node_id: str) -> tuple[dict[str, Any], Any | None]:
        try:
            data_value = await asyncio.wait_for(
                client.get_node(node_id).read_data_value(raise_on_bad_status=False), timeout_s)
        except asyncio.TimeoutError:
            return {"nodeId": node_id, "state": "failed", "status": "Timeout", "value": None}, None
        except Exception:
            return {"nodeId": node_id, "state": "failed", "status": "Exception", "value": None}, None
        status = resource_diagnostic_status(getattr(data_value, "StatusCode", None))
        if status == "BadNotSupported":
            return {"nodeId": node_id, "state": "unavailable", "status": status, "value": None}, data_value
        if status == "BadUserAccessDenied":
            return {"nodeId": node_id, "state": "authorization-denied", "status": status, "value": None}, data_value
        if status != "Good":
            return {"nodeId": node_id, "state": "failed", "status": status, "value": None}, data_value
        try:
            value = resource_contract.require_u64(data_value, node_id)
        except Exception:
            return {"nodeId": node_id, "state": "failed", "status": status, "value": None}, data_value
        if value >= 2**64:
            return {"nodeId": node_id, "state": "failed", "status": status, "value": None}, data_value
        return {"nodeId": node_id, "state": "measured", "status": status, "value": value}, data_value

    started = time.perf_counter()
    tasks = [asyncio.create_task(read_one(node_id)) for node_id in RESOURCE_DIAGNOSTIC_NODE_IDS]
    try:
        readings = await asyncio.wait_for(asyncio.gather(*tasks), timeout_s)
    except asyncio.TimeoutError:
        for task in tasks:
            task.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)
        readings = [({"nodeId": node_id, "state": "failed", "status": "Timeout", "value": None}, None)
                    for node_id in RESOURCE_DIAGNOSTIC_NODE_IDS]
    records = [record for record, _data_value in readings]
    values = {record["nodeId"]: data_value for record, data_value in readings
              if data_value is not None}
    states = [record["state"] for record in records]
    consistency = {"state": "not-run", "reason": "per-node-diagnostic-failure"}
    if all(item_state == "measured" for item_state in states):
        try:
            resource_contract.validate_resource_values(values)
        except Exception:
            consistency = {"state": "failed", "reason": "cross-node-invariant"}
            state = "failed"
        else:
            consistency = {"state": "measured"}
            state = "measured"
    elif "failed" in states:
        state = "failed"
    elif "authorization-denied" in states:
        state = "authorization-denied"
    else:
        state = "unavailable"
    return {
        "phase": RESOURCE_DIAGNOSTIC_PHASE,
        "state": state,
        "nodeCount": len(records),
        "measuredNodeCount": states.count("measured"),
        "unavailableNodeCount": states.count("unavailable"),
        "authorizationDeniedNodeCount": states.count("authorization-denied"),
        "failedNodeCount": states.count("failed"),
        "consistency": consistency,
        "elapsedMonotonicMs": int((time.perf_counter() - started) * 1000),
        "nodes": records,
    }


def resource_diagnostic_report(snapshots: list[dict[str, Any]]) -> dict[str, Any]:
    """Keep repetition evidence finite and classify a missing snapshot explicitly."""
    if not snapshots:
        return {"phase": RESOURCE_DIAGNOSTIC_PHASE, "state": "not-run", "snapshotCount": 0,
                "snapshots": []}
    states = [snapshot["state"] for snapshot in snapshots]
    if "failed" in states:
        state = "failed"
    elif "authorization-denied" in states:
        state = "authorization-denied"
    elif "unavailable" in states:
        state = "unavailable"
    else:
        state = "measured"
    return {"phase": RESOURCE_DIAGNOSTIC_PHASE, "state": state,
            "snapshotCount": len(snapshots), "snapshots": snapshots}


async def resolve_benchmark_value_nodes(client: Any, profile: dict[str, Any], timeout_s: float) -> list[str]:
    """Resolve the generic channel namespace per live session; it is never ns=1 by assumption."""
    namespace = await asyncio.wait_for(client.get_namespace_index(CHANNEL_NAMESPACE_URI), timeout_s)
    node_ids = [channel_value_node_id(f"benchmark.synthetic.{slot}", namespace)
                for slot in range(1, profile["channels"] + 1)]
    for node_id in node_ids:
        try:
            await asyncio.wait_for(client.get_node(node_id).read_value(), timeout_s)
        except Exception as exc:
            raise RuntimeError(f"required benchmark channel is unreadable ({safe_exception(exc)})") from exc
    return node_ids


async def create_monitored_items(client: Any, subscription: Any, nodes: list[str], count: int,
                                 batch_size: int, sampling_ms: float, timeout_s: float | None = None) -> list[Any]:
    handles = []
    expanded = [nodes[index % len(nodes)] for index in range(count)]
    for offset in range(0, len(expanded), batch_size):
        operation = subscription.subscribe_data_change(
            [client.get_node(node) for node in expanded[offset:offset + batch_size]],
            queuesize=1, sampling_interval=sampling_ms)
        result = await asyncio.wait_for(operation, timeout_s) if timeout_s is not None else await operation
        batch_handles = result if isinstance(result, list) else [result]
        expected = len(expanded[offset:offset + batch_size])
        if len(batch_handles) != expected or any(type(handle) is not int or handle < 0 for handle in batch_handles):
            raise RuntimeError("monitored-item creation returned a rejected or malformed per-item result")
        handles.extend(batch_handles)
    return handles


async def read_worker(client: Any, node_ids: list[str], rate_hz: float, stop_event: asyncio.Event,
                      samples: BoundedSamples, counters: dict[str, Any]) -> None:
    if rate_hz <= 0:
        await stop_event.wait()
        return
    index, interval = 0, 1.0 / rate_hz
    while not stop_event.is_set():
        started = time.perf_counter()
        try:
            await asyncio.wait_for(client.get_node(node_ids[index % len(node_ids)]).read_value(), counters["timeout"])
            samples.append((time.perf_counter() - started) * 1000)
            counters["reads"] += 1
        except Exception as exc:
            counters["errors"] += 1
            counters["log"].add("read", exc)
        index += 1
        try:
            await asyncio.wait_for(stop_event.wait(), max(0, interval - (time.perf_counter() - started)))
        except asyncio.TimeoutError:
            pass


async def run_stage(args: argparse.Namespace, client_count: int, item_count: int) -> StageResult:
    result, clients, subscriptions, handlers, workers = StageResult(client_count, item_count, args.duration), [], [], [], []
    stop_event, samples = asyncio.Event(), BoundedSamples(args.max_operation_samples)
    log = ErrorLog(args.max_errors, BOUNDS["maxErrorTextBytes"])
    counters = {"reads": 0, "errors": 0, "timeout": args.timeout, "log": log}
    print(f"\n=== Stage: clients={client_count}, monitored-items/client={item_count}, duration={args.duration:.1f}s ===")
    try:
        for index in range(client_count):
            client = await configure_client(args)
            try:
                await asyncio.wait_for(client.connect(), BOUNDS["connectTimeoutMs"] / 1000)
            except Exception as exc:
                result.connection_errors += 1
                log.add(f"client {index + 1} connect", exc)
                raise
            clients.append(client)
        for index, client in enumerate(clients):
            try:
                await asyncio.wait_for(client.nodes.server_state.read_value(), args.timeout)
            except Exception as exc:
                result.connection_errors += 1
                log.add(f"client {index + 1} readiness", exc)
                raise RuntimeError("client readiness failed") from exc
        nodes = await validate_monitored_nodes(clients[0], args.monitor_node or MONITORED_NODE_IDS)
        result.diagnostics_before = await read_diagnostics(clients[0])
        for client in clients:
            handler = SubscriptionHandler()
            subscription = await client.create_subscription(args.publishing_ms, handler)
            handles = await create_monitored_items(client, subscription, nodes, item_count, args.create_batch, args.sampling_ms)
            handlers.append(handler); subscriptions.append(subscription)
            result.subscriptions_created += 1; result.monitored_items_created += len(handles)
        workers = [asyncio.create_task(read_worker(client, nodes, args.read_rate, stop_event, samples, counters)) for client in clients]
        await asyncio.sleep(args.duration); stop_event.set()
        await asyncio.gather(*workers, return_exceptions=True); workers = []
        result.notifications, result.read_operations, result.read_errors = sum(h.notifications for h in handlers), counters["reads"], counters["errors"]
        distribution = samples.report()
        result.latency_ms_p50, result.latency_ms_p95 = distribution["p50"], distribution["p95"]
        result.latency_ms_p99, result.latency_ms_max = distribution["p99"], distribution["max"]
        result.diagnostics_after = await read_diagnostics(clients[0])
        before, after = result.diagnostics_before.get("uptime_s"), result.diagnostics_after.get("uptime_s")
        result.reboot_detected = isinstance(before, (int, float)) and isinstance(after, (int, float)) and after < before
    except Exception as exc:
        log.add("stage", exc)
    finally:
        stop_event.set()
        if workers:
            await asyncio.gather(*workers, return_exceptions=True)
        for subscription in reversed(subscriptions):
            try: await subscription.delete()
            except Exception as exc: log.add("subscription cleanup", exc)
        for client in reversed(clients):
            try: await client.disconnect()
            except Exception as exc: log.add("client cleanup", exc)
        result.errors, result.sample_overflow = log.items, samples.overflow + log.overflow
    print_stage_result(result)
    return result


def diagnostic_delta(before: dict, after: dict, key: str) -> Any:
    return after[key] - before[key] if isinstance(before.get(key), (int, float)) and isinstance(after.get(key), (int, float)) else None


def print_stage_result(result: StageResult) -> None:
    print(f"Created: subscriptions={result.subscriptions_created}, monitored-items={result.monitored_items_created}")
    print(f"Traffic: notifications={result.notifications}, reads={result.read_operations}, read-errors={result.read_errors}, connection-errors={result.connection_errors}")
    print(f"Read latency ms: p50={result.latency_ms_p50}, p95={result.latency_ms_p95}, p99={result.latency_ms_p99}, max={result.latency_ms_max}")
    print(f"Memory delta: SRAM free={diagnostic_delta(result.diagnostics_before, result.diagnostics_after, 'free_heap')}, PSRAM used={diagnostic_delta(result.diagnostics_before, result.diagnostics_after, 'psram_used')}")
    print(f"Sample/error-record overflow: {result.sample_overflow}")
    for error in result.errors: print(f"[ERROR] {error}")


async def run_test(args: argparse.Namespace) -> int:
    stages = []
    for clients in args.clients:
        for items in args.items:
            if clients * items > args.max_total_items: continue
            stages.append(await run_stage(args, clients, items))
            if stages[-1].reboot_detected or stages[-1].connection_errors: break
            await asyncio.sleep(args.cooldown)
        if stages and (stages[-1].reboot_detected or stages[-1].connection_errors): break
    if args.output:
        write_bounded_json(Path(args.output), [asdict(stage) for stage in stages], BOUNDS["maxJsonBytes"])
    failed = [stage for stage in stages if stage.reboot_detected or stage.connection_errors or stage.read_errors or stage.sample_overflow or stage.errors]
    return 1 if not stages or failed else 0


@dataclass
class Metric:
    name: str
    capacity: int
    state: str = "not-run"
    reason: str | None = None
    operations: int = 0
    failures: int = 0
    timeouts: int = 0
    samples: BoundedSamples = field(init=False)
    def __post_init__(self) -> None: self.samples = BoundedSamples(self.capacity)
    def set_state(self, state: str, reason: str) -> None:
        if state not in RESULT_STATES or state == "measured": raise ValueError("invalid result state")
        self.state, self.reason = state, reason
    def success(self, elapsed_ms: float) -> None:
        self.operations += 1
        if not self.samples.append(elapsed_ms):
            self.set_state("failed", "sample capacity exceeded")
        elif self.state == "not-run":
            self.state = "measured"
    def failure(self, exc: BaseException) -> None:
        self.operations += 1; self.failures += 1; self.timeouts += isinstance(exc, asyncio.TimeoutError)
        self.set_state("failed", safe_exception(exc)[:BOUNDS["maxErrorTextBytes"]])
    def report(self) -> dict[str, Any]:
        result = {"state": self.state, "reason": self.reason, "unit": "milliseconds", "operationCount": self.operations,
                  "failureCount": self.failures, "timedOutOperationCount": self.timeouts}
        result.update(self.samples.report()); return result


class RawOperationCancelledError(RuntimeError):
    """The child OPC UA operation was cancelled; this is distinct from caller cancellation."""


def observe_quarantined_task(task: asyncio.Future[Any]) -> None:
    """Consume a detached task result; it must never update benchmark state."""
    try:
        task.result()
    except BaseException:
        pass


def quarantine_task(task: asyncio.Future[Any]) -> None:
    """Request cancellation without awaiting an untrusted coroutine indefinitely."""
    if not task.done():
        task.cancel()
    task.add_done_callback(observe_quarantined_task)


async def strict_bounded_call(operation: Any, timeout_s: float) -> tuple[bool, Any | None]:
    """Return at the deadline even when an asyncua coroutine suppresses cancellation.

    Python cannot terminate arbitrary coroutines.  A late operation is therefore
    quarantined, its eventual result is observed, and its caller must fence the
    affected client before issuing another OPC UA service request on it.
    """
    deadline = time.perf_counter() + timeout_s
    task = asyncio.ensure_future(operation)
    try:
        done, _pending = await asyncio.wait((task,), timeout=timeout_s)
    except asyncio.CancelledError:
        # This task's caller was cancelled.  Preserve cancellation semantics
        # while ensuring the child task cannot become unobserved.
        quarantine_task(task)
        raise
    except BaseException:
        quarantine_task(task)
        raise
    if not done:
        quarantine_task(task)
        return False, None

    # Read the terminal result even when the loop was blocked until after the
    # deadline.  It must be observed, but it is never a successful sample.
    late = time.perf_counter() > deadline
    try:
        result = task.result()
    except asyncio.CancelledError as exc:
        if late:
            return False, None
        raise RawOperationCancelledError("raw OPC UA operation was cancelled") from exc
    except BaseException:
        if late:
            return False, None
        raise
    return (False, None) if late else (True, result)


async def strict_timed_read(metric: Metric, operation: Any, timeout_s: float) -> Any:
    """Account a scheduled Read once, at its declared deadline, never later."""
    started = time.perf_counter()
    try:
        completed, result = await strict_bounded_call(operation, timeout_s)
    except asyncio.CancelledError:
        raise
    except BaseException as exc:
        metric.failure(exc)
        raise
    if not completed:
        exc = asyncio.TimeoutError("read exceeded declared operation timeout")
        metric.failure(exc)
        raise exc
    metric.success((time.perf_counter() - started) * 1000)
    return result


async def bounded_disconnect(client: Any, timeout_s: float, log: ErrorLog,
                             context: str) -> bool:
    """Fence a client with bounded host waiting; late disconnects stay quarantined."""
    try:
        completed, _result = await strict_bounded_call(client.disconnect(), timeout_s)
    except BaseException as exc:
        log.add(context, exc)
        return False
    if not completed:
        log.add(context, "TimeoutError")
        return False
    return True


async def timed(metric: Metric, operation: Any, timeout_s: float,
                deadline: float | None = None) -> Any:
    started = time.perf_counter()
    effective_timeout = timeout_s
    if deadline is not None:
        effective_timeout = min(timeout_s, max(0.0, deadline - started))
    if effective_timeout <= 0.0:
        if hasattr(operation, "close"):
            operation.close()
        exc = asyncio.TimeoutError("operation would start after the run deadline")
        metric.failure(exc)
        raise exc
    try: result = await asyncio.wait_for(operation, effective_timeout)
    except Exception as exc:
        metric.failure(exc); raise
    if deadline is not None and time.perf_counter() > deadline:
        exc = asyncio.TimeoutError("operation completed after the run deadline")
        metric.failure(exc)
        raise exc
    metric.success((time.perf_counter() - started) * 1000); return result


def read_bounded_json(path: Path, maximum: int = MAX_METADATA_BYTES) -> dict[str, Any]:
    with path.open("rb") as stream:
        raw = stream.read(maximum + 1)
    if len(raw) > maximum: raise ValueError(f"{path} exceeds {maximum} byte input bound")
    value = json.loads(raw.decode("utf-8"))
    if not isinstance(value, dict): raise ValueError(f"{path} must contain a JSON object")
    return value


def write_bounded_json(path: Path, document: Any, maximum: int) -> None:
    encoded = json.dumps(document, indent=2, sort_keys=True, allow_nan=False).encode("utf-8")
    if len(encoded) > maximum: raise ValueError(f"JSON output exceeds {maximum} bytes")
    path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(encoded + b"\n")


def require_benchmark_args(args: argparse.Namespace, profile: dict[str, Any]) -> None:
    finite = (args.timeout, args.duration, args.warmup, args.cooldown, args.publishing_ms, args.sampling_ms, args.read_rate)
    if any(not math.isfinite(value) or value < 0 for value in finite): raise ValueError("time and rate values must be finite non-negative")
    if not 0 < args.timeout <= BOUNDS["operationTimeoutMs"] / 1000: raise ValueError("operation timeout exceeds frozen bound")
    if not 0 < args.duration <= BOUNDS["maxRunSeconds"]: raise ValueError("duration exceeds frozen bound")
    if args.warmup > BOUNDS["maxWarmupSeconds"] or not 1 <= args.repetitions <= BOUNDS["maxRepetitionsPerProfile"]: raise ValueError("warmup/repetitions exceed frozen bound")
    if not 1 <= args.max_operation_samples <= BOUNDS["maxOperationSamplesPerMetric"] or not 1 <= args.max_errors <= BOUNDS["maxErrorsPerRun"]: raise ValueError("sample/error record bound invalid")
    if not 1 <= args.create_batch <= 128 or not 1 <= args.max_total_items <= 128: raise ValueError("monitored item bound invalid")
    profile_period = 1000 / profile["rateHz"]
    if args.clients != [profile["clients"]]: raise ValueError("--clients must equal the selected profile client count")
    if args.items != [profile["channels"]] or args.clients[0] * args.items[0] > args.max_total_items: raise ValueError("every profile channel must be monitored by every client")
    if args.read_node: raise ValueError("--read-node is legacy-only; benchmark resolves profile channel NodeIds live")
    if args.publishing_ms != profile_period or args.sampling_ms != profile_period or args.read_rate != profile["rateHz"]:
        raise ValueError("publishing/sampling/read rates must exactly match the selected profile")
    if not 1 <= args.history_max_values <= BOUNDS["maxHistoryContinuationPointsPerClient"]: raise ValueError("history page bound invalid")
    if profile["security"] == "basic256sha256-signandencrypt" and (args.security_policy, args.security_mode) != ("Basic256Sha256", "SignAndEncrypt"):
        raise ValueError("secure profile requires Basic256Sha256 SignAndEncrypt")
    if profile["security"] == "test-none" and not args.test_none:
        raise ValueError("test-none profile requires explicit --test-none")
    if not isinstance(args.git_commit, str) or len(args.git_commit) != 40 or any(character not in "0123456789abcdef" for character in args.git_commit.lower()):
        raise ValueError("--git-commit must be an exact 40-character SHA-1")
    for name in ("board_revision", "toolchain", "build_type", "build_flags", "network_setup", "time_synchronization"):
        value = getattr(args, name)
        if not isinstance(value, str) or not value or value == "unknown" or len(value.encode("utf-8")) > 256 or "@" in value:
            raise ValueError(f"--{name.replace('_', '-')} must be a bounded credential-free string")


def validate_benchmark_metadata(metadata: dict[str, Any], profile: dict[str, Any], git_commit: str) -> None:
    benchmark_profiles.verify_metadata(profile, metadata)
    required = ("firmwareVersion", "buildGitHash", "board", "driverPack", "namespaceZero", "security",
                "channels", "rateHz", "clients", "events", "history", "configuredCapacities")
    if any(key not in metadata for key in required):
        raise ValueError("benchmark metadata is incomplete")
    build_hash = metadata["buildGitHash"]
    if not isinstance(build_hash, str) or not 7 <= len(build_hash) <= 40 or any(character not in "0123456789abcdef" for character in build_hash.lower()):
        raise ValueError("benchmark metadata has an invalid build Git hash")
    if not git_commit.lower().startswith(build_hash.lower()):
        raise ValueError("exact --git-commit does not match benchmark metadata build Git hash")
    if metadata["security"] != profile["security"] or metadata["channels"] != profile["channels"] or metadata["rateHz"] != profile["rateHz"] or metadata["clients"] != profile["clients"] or metadata["events"] != profile["events"] or metadata["history"] != profile["history"]:
        raise ValueError("benchmark metadata does not match the selected profile")
    if not isinstance(metadata["configuredCapacities"], dict):
        raise ValueError("benchmark metadata capacities must be an object")


async def history_read_once(client: Any, node_id: str, max_values: int,
                            timeout_s: float | None = None,
                            response_metric: Metric | None = None,
                            cleanup_metric: Metric | None = None,
                            deadline: float | None = None) -> Any:
    """Issue at most one raw page and release any continuation before return."""
    ua, now = opcua_common.ua, datetime.now(timezone.utc)
    details = ua.ReadRawModifiedDetails(); details.IsReadModified, details.ReturnBounds = False, False
    details.StartTime, details.EndTime, details.NumValuesPerNode = now - timedelta(minutes=5), now, max_values
    value = ua.HistoryReadValueId(); value.NodeId, value.IndexRange, value.ContinuationPoint = ua.NodeId.from_string(node_id), "", None
    request = ua.HistoryReadParameters(); request.HistoryReadDetails, request.TimestampsToReturn = details, ua.TimestampsToReturn.Both
    request.ReleaseContinuationPoints, request.NodesToRead = False, [value]
    continuation = None
    response_started = time.perf_counter()
    try:
        initial_timeout = timeout_s
        if deadline is not None:
            remaining = max(0.0, deadline - response_started)
            initial_timeout = remaining if timeout_s is None else min(timeout_s, remaining)
        if initial_timeout is not None and initial_timeout <= 0.0:
            raise asyncio.TimeoutError("HistoryRead would start after the run deadline")
        initial_operation = client.uaclient.session.history_read(request)
        initial_results = await asyncio.wait_for(initial_operation, initial_timeout) if initial_timeout is not None else await initial_operation
        result = initial_results[0]
        continuation = getattr(result, "ContinuationPoint", None)
        status = getattr(result, "StatusCode", None)
        if status is not None and not status.is_good():
            raise RuntimeError(f"HistoryRead status {getattr(status, 'name', getattr(status, 'value', 'bad'))}")
        if deadline is not None and time.perf_counter() > deadline:
            raise asyncio.TimeoutError("HistoryRead completed after the run deadline")
    except BaseException as exc:
        if response_metric is not None:
            response_metric.failure(exc)
        raise
    else:
        if response_metric is not None:
            response_metric.success((time.perf_counter() - response_started) * 1000.0)
        return result
    finally:
        if continuation:
            cleanup_started = time.perf_counter()
            cancellation = None
            try:
                release = ua.HistoryReadParameters(); release.HistoryReadDetails, release.TimestampsToReturn = details, ua.TimestampsToReturn.Both
                release.ReleaseContinuationPoints = True
                release_value = ua.HistoryReadValueId(); release_value.NodeId, release_value.IndexRange, release_value.ContinuationPoint = value.NodeId, "", continuation
                release.NodesToRead = [release_value]
                release_task = asyncio.create_task(client.uaclient.session.history_read(release))
                try:
                    release_wait = asyncio.shield(release_task)
                    release_results = await asyncio.wait_for(release_wait, timeout_s) if timeout_s is not None else await release_wait
                except asyncio.CancelledError as exc:
                    cancellation = exc
                    release_wait = asyncio.shield(release_task)
                    release_results = await asyncio.wait_for(release_wait, timeout_s) if timeout_s is not None else await release_wait
                release_result = release_results[0]
                release_status = getattr(release_result, "StatusCode", None)
                if release_status is not None and not release_status.is_good():
                    raise RuntimeError(f"HistoryRead release status {getattr(release_status, 'name', getattr(release_status, 'value', 'bad'))}")
            except BaseException as exc:
                if cleanup_metric is not None:
                    cleanup_metric.failure(exc)
                raise
            else:
                if cleanup_metric is not None:
                    cleanup_metric.success((time.perf_counter() - cleanup_started) * 1000.0)
            if cancellation is not None:
                raise cancellation


def validate_native_evidence(document: dict[str, Any], profile_id: str | None = None,
                             warmup_s: float | None = None, duration_s: float | None = None) -> dict[str, list[tuple[int, int]]]:
    if document.get("schemaVersion") != 1 or document.get("kind") != "v052-native-sampling-evidence": raise ValueError("unsupported native evidence schema")
    if document.get("state") != "complete": raise ValueError("native evidence state is not complete")
    clock, records = document.get("clock"), document.get("records")
    if not isinstance(clock, dict) or (clock.get("domain"), clock.get("unit"), clock.get("valid")) != ("device-monotonic", "microseconds", True): raise ValueError("invalid native clock")
    if type(document.get("captureId")) is not int or document["captureId"] < 1 or not isinstance(document.get("profileId"), str): raise ValueError("native capture identity invalid")
    if profile_id is not None and document["profileId"] != profile_id: raise ValueError("native profile does not match benchmark profile")
    if warmup_s is not None and document.get("warmupSeconds") != warmup_s: raise ValueError("native warmup does not match client setup")
    if duration_s is not None and document.get("durationSeconds") != duration_s: raise ValueError("native duration does not match client setup")
    times = (document.get("bootStartedMonotonicUs"), document.get("captureStartedMonotonicUs"), document.get("captureEndedMonotonicUs"))
    if any(type(value) is not int or value < 0 or value >= 2**64 for value in times) or not times[0] <= times[1] <= times[2]: raise ValueError("native capture window invalid")
    if not isinstance(records, list) or len(records) > 8192 or document.get("recordCapacity") != 8192: raise ValueError("native record capacity invalid")
    grouped, seen = {}, set()
    for item in records:
        if not isinstance(item, dict): raise ValueError("native record is not an object")
        channel, sequence, stamp = item.get("channelId"), item.get("sequence"), item.get("sampleMonotonicUs")
        if not isinstance(channel, str) or not channel or len(channel.encode()) > 64 or type(sequence) is not int or type(stamp) is not int or not 0 <= sequence < 2**64 or not 0 <= stamp < 2**64 or (channel, sequence) in seen: raise ValueError("invalid native sample")
        seen.add((channel, sequence)); grouped.setdefault(channel, []).append((sequence, stamp))
    for channel in grouped:
        grouped[channel].sort()
        if grouped[channel][0][0] != 1: raise ValueError(f"native sequence does not start at one for {channel}")
        if any(b[0] != a[0] + 1 or b[1] <= a[1] for a, b in zip(grouped[channel], grouped[channel][1:])): raise ValueError(f"non-contiguous native sequence/timestamp for {channel}")
        if any(stamp < times[1] or stamp > times[2] for _sequence, stamp in grouped[channel]): raise ValueError(f"native timestamp is outside capture window for {channel}")
    return grouped


def native_jitter_metrics(document: dict[str, Any], period_ms: float, capacity: int, profile_id: str | None = None,
                          warmup_s: float | None = None, duration_s: float | None = None,
                          expected_channels: set[str] | None = None) -> dict[str, Any]:
    if not math.isfinite(period_ms) or period_ms <= 0: raise ValueError("sampling period invalid")
    output = {}
    grouped = validate_native_evidence(document, profile_id, warmup_s, duration_s)
    if expected_channels is not None and set(grouped) != expected_channels:
        raise ValueError("native capture channel set does not exactly match the selected profile")
    for channel, samples in grouped.items():
        intervals, jitter = BoundedSamples(capacity), BoundedSamples(capacity)
        for first, second in zip(samples, samples[1:]):
            interval = (second[1] - first[1]) / 1000; intervals.append(interval); jitter.append(abs(interval - period_ms))
        output[channel] = ({"state": "unavailable", "reason": "fewer than two correlated samples"} if len(samples) < 2 else
                           {"state": "failed", "reason": "sample capacity exceeded"} if intervals.overflow else
                           {"state": "measured", "samplingIntervalMs": intervals.report(), "samplingJitterMs": jitter.report()})
    return output


async def benchmark_connect(args: argparse.Namespace, metric: Metric) -> Any:
    client = await configure_client(args)
    try:
        await timed(metric, client.connect(), BOUNDS["connectTimeoutMs"] / 1000)
    except BaseException:
        try:
            await asyncio.wait_for(client.disconnect(), args.timeout)
        except BaseException:
            pass
        raise
    return client


async def benchmark_cleanup(subscriptions: list[Any], clients: list[Any], timeout_s: float,
                            log: ErrorLog) -> None:
    for subscription in reversed(subscriptions):
        try:
            completed, _result = await strict_bounded_call(subscription.delete(), timeout_s)
            if not completed:
                log.add("subscription cleanup", "TimeoutError")
        except BaseException as exc:
            log.add("subscription cleanup", exc)
    subscriptions.clear()
    for client in reversed(clients):
        await bounded_disconnect(client, timeout_s, log, "client cleanup")
    clients.clear()


async def run_client_read_schedule(client: Any, nodes: list[str], client_index: int,
                                   rate_hz: float, end: float, timeout_s: float,
                                   metric: Metric, log: ErrorLog,
                                   max_in_flight: int = MAX_READS_IN_FLIGHT_PER_CLIENT) -> dict[str, Any]:
    """Dispatch one client's Reads at fixed arrivals with bounded outstanding work.

    Arrival slots are never shortened to the rate period: every dispatched
    call receives the declared operation timeout.  Slots made stale by event
    loop lag, or rejected because the per-client outstanding limit is full,
    are recorded as missed rather than silently reducing the declared load.
    A timed-out operation fences that logical client.  Python cannot terminate
    a coroutine which suppresses cancellation, so its result is quarantined and
    observed while no later benchmark RPC is allowed on the fenced client.
    """
    if rate_hz <= 0 or not math.isfinite(rate_hz):
        raise ValueError("read rate must be finite and positive")
    if timeout_s <= 0 or not math.isfinite(timeout_s):
        raise ValueError("read timeout must be finite and positive")
    if type(max_in_flight) is not int or not 1 <= max_in_flight <= MAX_READS_IN_FLIGHT_PER_CLIENT:
        raise ValueError("in-flight read limit is outside the fixed bound")
    if not nodes:
        raise ValueError("at least one benchmark value node is required")

    period = 1.0 / rate_hz
    next_arrival = time.perf_counter()
    index = client_index % len(nodes)
    in_flight: set[asyncio.Task[str]] = set()
    transport_fenced = False
    accounting: dict[str, Any] = {
        "scheduledOperationCount": 0, "completedOperationCount": 0,
        "missedDeadlineCount": 0, "deadlineExceededOperationCount": 0,
        "failedOperationCount": 0, "inFlightLimitExceededCount": 0,
        "peakInFlightCount": 0, "transportFenced": False,
    }

    async def fence_transport() -> None:
        nonlocal transport_fenced
        if not transport_fenced:
            transport_fenced = True
            accounting["transportFenced"] = True
            await bounded_disconnect(client, timeout_s, log,
                                     f"read client {client_index + 1} transport fence")

    async def read_once(node_id: str) -> str:
        try:
            # Deliberately no cadence deadline: the full declared timeout is
            # part of the measured operation contract, not the 1/rate period.
            await strict_timed_read(metric, client.get_node(node_id).read_value(), timeout_s)
        except asyncio.TimeoutError as exc:
            # A request that reaches its timeout may have retained the client
            # transport.  Fence it before this task completes; later arrivals
            # are accounted as missed and post-steady RPCs are prohibited.
            await fence_transport()
            log.add(f"read client {client_index + 1}", exc)
            return "deadline-exceeded"
        except RawOperationCancelledError as exc:
            # A completed child cancellation is an unsafe transport outcome,
            # not caller cancellation.  Merge it into accounting and fence.
            await fence_transport()
            log.add(f"read client {client_index + 1}", exc)
            return "failed"
        except Exception as exc:
            log.add(f"read client {client_index + 1}", exc)
            return "failed"
        return "completed"

    def account_terminal(task: asyncio.Task[str], *, forced_timeout: bool = False) -> None:
        if forced_timeout:
            metric.failure(asyncio.TimeoutError("read drain exceeded declared timeout"))
            accounting["deadlineExceededOperationCount"] += 1
            log.add(f"read client {client_index + 1} drain", "TimeoutError")
            return
        try:
            outcome = task.result()
        except asyncio.CancelledError:
            metric.failure(asyncio.TimeoutError("read cancelled during bounded drain"))
            accounting["deadlineExceededOperationCount"] += 1
            log.add(f"read client {client_index + 1} drain", "TimeoutError")
        except BaseException as exc:
            metric.failure(exc)
            accounting["failedOperationCount"] += 1
            log.add(f"read client {client_index + 1}", exc)
        else:
            if outcome == "completed":
                accounting["completedOperationCount"] += 1
            elif outcome == "deadline-exceeded":
                accounting["deadlineExceededOperationCount"] += 1
            else:
                accounting["failedOperationCount"] += 1

    def harvest_done() -> None:
        done = {task for task in in_flight if task.done()}
        for task in done:
            in_flight.remove(task)
            account_terminal(task)

    try:
        while next_arrival < end:
            now = time.perf_counter()
            if now >= end:
                # No operation starts after the steady interval.  Account the
                # remaining declared arrivals exactly once as scheduler misses.
                while next_arrival < end:
                    accounting["scheduledOperationCount"] += 1
                    accounting["missedDeadlineCount"] += 1
                    next_arrival += period
                break
            if now < next_arrival:
                await asyncio.sleep(max(0.0, min(next_arrival, end) - now))
                continue

            # Minor wake-up jitter still dispatches the current slot.  Once a
            # whole period is late, older slots are stale and are never caught
            # up in a burst that would distort the requested rate.
            stale_slots = int((now - next_arrival) // period)
            for _ in range(stale_slots):
                if next_arrival >= end:
                    break
                accounting["scheduledOperationCount"] += 1
                accounting["missedDeadlineCount"] += 1
                next_arrival += period
            if next_arrival >= end:
                break

            harvest_done()
            accounting["scheduledOperationCount"] += 1
            if transport_fenced:
                accounting["missedDeadlineCount"] += 1
            elif len(in_flight) >= max_in_flight:
                accounting["missedDeadlineCount"] += 1
                accounting["inFlightLimitExceededCount"] += 1
            else:
                task = asyncio.create_task(read_once(nodes[index]))
                in_flight.add(task)
                accounting["peakInFlightCount"] = max(accounting["peakInFlightCount"], len(in_flight))
                index = (index + 1) % len(nodes)
            next_arrival += period
    finally:
        # Give an arrival which started before the end its full declared
        # timeout.  Any task that will not finish is quarantined with its
        # exception observed; it cannot update the metric after accounting.
        if in_flight:
            done, pending = await asyncio.wait(in_flight, timeout=timeout_s)
            for task in done:
                account_terminal(task)
            if pending:
                # The strict read timer and this drain timer can expire in the
                # same loop turn.  Yield once so the read task can record its
                # timeout and fence the client before any outer cancellation.
                await asyncio.sleep(0)
                settled = {task for task in pending if task.done()}
                for task in settled:
                    account_terminal(task)
                pending -= settled
            if pending:
                # These tasks have already consumed their full operation
                # timeout.  Fence before cancelling their wrappers so a loop
                # blocked past the deadline cannot leave the session reusable.
                await fence_transport()
                for task in pending:
                    quarantine_task(task)
                cancelled, still_pending = await asyncio.wait(pending, timeout=timeout_s)
                for task in cancelled:
                    account_terminal(task)
                for task in still_pending:
                    account_terminal(task, forced_timeout=True)
                    quarantine_task(task)
            in_flight.clear()
    if accounting["scheduledOperationCount"] != (
            accounting["completedOperationCount"] + accounting["missedDeadlineCount"] +
            accounting["deadlineExceededOperationCount"] + accounting["failedOperationCount"]):
        raise RuntimeError("read schedule accounting invariant violated")
    return accounting


def merge_load_accounting(destination: dict[str, Any], source: dict[str, int]) -> None:
    for key in ("scheduledOperationCount", "completedOperationCount", "missedDeadlineCount",
                "deadlineExceededOperationCount", "failedOperationCount",
                "inFlightLimitExceededCount"):
        destination[key] += source[key]
    destination["peakInFlightCount"] = max(destination["peakInFlightCount"],
                                           source["peakInFlightCount"])
    destination["transportFencedClientCount"] += int(bool(source["transportFenced"]))


async def run_benchmark(args: argparse.Namespace) -> int:
    if not args.benchmark_profile_id or not args.benchmark_metadata: raise ValueError("benchmark profile ID and metadata are required")
    profile = benchmark_profiles.select(args.benchmark_profile_id); require_benchmark_args(args, profile)
    metadata = read_bounded_json(Path(args.benchmark_metadata)); validate_benchmark_metadata(metadata, profile, args.git_commit)
    metrics = {name: Metric(name, args.max_operation_samples) for name in ("StartupLatencyMs", "SecureSessionEstablishmentLatencyMs.Cold", "SecureSessionEstablishmentLatencyMs.Warm", "ReadLatencyMs", "WriteLatencyMs", "MethodLatencyMs", "HistoryReadLatencyMs", "EventDeliveryLatencyMs")}
    history_cleanup_metric = Metric("HistoryContinuationReleaseLatencyMs", args.max_operation_samples)
    metrics["StartupLatencyMs"].set_state("unavailable", "no device boot/readiness hook in the host monotonic clock domain")
    metrics["WriteLatencyMs"].set_state("unsupported", "synthetic benchmark channels are non-writable; no mutation is permitted")
    metrics["EventDeliveryLatencyMs"].set_state("unavailable", "no source timestamp/client receive correlation is available")
    if not profile["history"]: metrics["HistoryReadLatencyMs"].set_state("unsupported", "profile disables history")
    log, clients, subscriptions, handlers = ErrorLog(args.max_errors, BOUNDS["maxErrorTextBytes"]), [], [], []
    resource_snapshots: list[dict[str, Any]] = []
    verified, notifications = False, 0
    load_accounting = {"scheduledOperationCount": 0, "completedOperationCount": 0, "missedDeadlineCount": 0,
                       "deadlineExceededOperationCount": 0, "failedOperationCount": 0,
                       "inFlightLimitExceededCount": 0, "peakInFlightCount": 0,
                       "transportFencedClientCount": 0, "rateHzPerClient": args.read_rate}
    try:
        cold = await benchmark_connect(args, metrics["SecureSessionEstablishmentLatencyMs.Cold"]); clients.append(cold)
        export = await timed(metrics["MethodLatencyMs"], opcua_common.export_config(cold.get_node(opcua_common.CONFIGURATION_NODE_ID)), args.timeout)
        benchmark_profiles.verify_export(profile, export); verified = True
        await asyncio.wait_for(cold.disconnect(), args.timeout); clients.remove(cold)
        for _repetition in range(args.repetitions):
            for _ in range(profile["clients"]): clients.append(await benchmark_connect(args, metrics["SecureSessionEstablishmentLatencyMs.Warm"]))
            nodes = await resolve_benchmark_value_nodes(clients[0], profile, args.timeout)
            for client in clients:
                handler = SubscriptionHandler(); subscription = await asyncio.wait_for(client.create_subscription(args.publishing_ms, handler), args.timeout)
                subscriptions.append(subscription)
                await create_monitored_items(client, subscription, nodes, len(nodes), args.create_batch, args.sampling_ms, args.timeout)
                handlers.append(handler)
            if args.warmup: await asyncio.sleep(args.warmup)
            end = time.perf_counter() + args.duration
            schedules = [asyncio.create_task(
                run_client_read_schedule(client, nodes, client_index, args.read_rate, end,
                                         args.timeout, metrics["ReadLatencyMs"], log))
                         for client_index, client in enumerate(clients)]
            schedule_results = await asyncio.gather(*schedules, return_exceptions=True)
            transport_fenced = False
            for client_index, schedule_result in enumerate(schedule_results):
                if isinstance(schedule_result, BaseException):
                    log.add(f"read schedule client {client_index + 1}", schedule_result)
                    continue
                merge_load_accounting(load_accounting, schedule_result)
                transport_fenced = transport_fenced or bool(schedule_result["transportFenced"])

            if transport_fenced:
                # A timeout can leave an arbitrary asyncua coroutine alive.
                # Its client was fenced by the schedule, so this run must not
                # issue HistoryRead or diagnostics on any benchmark session.
                log.add("post-load operations", "transport fenced after non-cooperative read")
            else:
                # The frozen profile matrix has no HistoryRead request-rate axis;
                # it fixes only the per-client continuation cap. Run exactly one
                # bounded page per external client after the steady read interval,
                # so history measurement cannot serialize or lower the declared
                # per-client Read rate.
                if profile["history"]:
                    for client_index, client in enumerate(clients):
                        try:
                            await history_read_once(client, nodes[client_index % len(nodes)],
                                                    args.history_max_values, args.timeout,
                                                    metrics["HistoryReadLatencyMs"],
                                                    history_cleanup_metric)
                        except Exception as exc:
                            log.add(f"history client {client_index + 1}", exc)

                # This occurs only after the timed Read window and uses client 0,
                # which is already counted in the declared profile.  It therefore
                # neither adds a fifth session nor alters steady-load accounting.
                resource_snapshot = await capture_resource_diagnostics(clients[0], args.timeout)
                resource_snapshots.append(resource_snapshot)
                if resource_snapshot["state"] != "measured":
                    log.add("post-steady resource diagnostics", resource_snapshot["state"])
            notifications += sum(handler.notifications for handler in handlers)
            await benchmark_cleanup(subscriptions, clients, args.timeout, log)
            handlers.clear()
            if transport_fenced:
                break
    except Exception as exc:
        log.add("benchmark setup", exc)
        for metric in metrics.values():
            if metric.state == "not-run": metric.set_state("failed", "benchmark setup failed")
        notifications = sum(handler.notifications for handler in handlers)
    finally:
        await benchmark_cleanup(subscriptions, clients, args.timeout, log)
    jitter: Any = {"state": "unavailable", "reason": "native sampling evidence not supplied"}
    if args.native_sampling_evidence:
        try:
            evidence_path = Path(args.native_sampling_evidence)
            with evidence_path.open("rb") as stream:
                raw_evidence = stream.read(MAX_NATIVE_EVIDENCE_BYTES + 1)
            if len(raw_evidence) > MAX_NATIVE_EVIDENCE_BYTES:
                raise ValueError("native evidence exceeds input byte bound")
            evidence = json.loads(raw_evidence.decode("utf-8"))
            if not isinstance(evidence, dict):
                raise ValueError("native evidence must be one JSON object")
            channel_ids = {f"benchmark.synthetic.{slot}" for slot in range(1, profile["channels"] + 1)}
            measurements = native_jitter_metrics(evidence, 1000 / profile["rateHz"], args.max_operation_samples,
                                                  profile["profileId"], args.warmup, args.duration, channel_ids)
            jitter = {"state": "unavailable", "reason": "external native capture is not associated with this host client run",
                      "associationState": "unverified-external-capture", "externalCapture": {
                          "sha256": hashlib.sha256(raw_evidence).hexdigest(), "captureId": evidence["captureId"],
                          "profileId": evidence["profileId"], "state": "measured", "perChannel": measurements}}
        except Exception as exc: jitter = {"state": "failed", "reason": safe_exception(exc)}; log.add("native evidence", exc)
    accounted_metrics = [*metrics.values(), history_cleanup_metric]
    accounting = {"operationCount": sum(metric.operations for metric in accounted_metrics), "sampleCount": sum(len(metric.samples.values) for metric in accounted_metrics), "notificationCount": notifications, "errorCount": len(log.items) + log.overflow, "droppedSampleCount": sum(metric.samples.overflow for metric in accounted_metrics), "timedOutOperationCount": sum(metric.timeouts for metric in accounted_metrics)}
    setup = {"schemaVersion": 1, "profileSchemaVersion": profile["profileSchemaVersion"], "profileId": profile["profileId"],
             "firmwareVersion": metadata["firmwareVersion"], "gitCommit": args.git_commit, "gitDirty": args.git_dirty,
             "boardRevision": args.board_revision, "driverPack": metadata["driverPack"], "namespaceZero": metadata["namespaceZero"],
             "securityPolicy": "None" if args.test_none else args.security_policy, "securityMode": "None" if args.test_none else args.security_mode,
             "toolchain": args.toolchain, "buildType": args.build_type, "buildFlags": args.build_flags, "configuredCapacities": metadata["configuredCapacities"],
             "channelCount": profile["channels"], "samplePeriodMs": 1000 // profile["rateHz"], "publishPeriodMs": 1000 // profile["rateHz"],
             "clientCount": profile["clients"], "clientVersions": {"asyncua": asyncua_version()}, "eventsEnabled": profile["events"], "historyEnabled": profile["history"],
             "networkSetup": args.network_setup, "timeSynchronization": args.time_synchronization, "warmupSeconds": args.warmup,
             "durationSeconds": args.duration, "repetitions": args.repetitions}
    metric_report = {name: metric.report() for name, metric in metrics.items()}
    metric_report["MethodLatencyMs"]["phase"] = "setup-export-config-profile-verification"
    metric_report["HistoryReadLatencyMs"]["continuationRelease"] = history_cleanup_metric.report()
    resource_evidence = resource_diagnostic_report(resource_snapshots)
    report = {**setup, "profileVerification": {"metadata": "measured", "exportConfig": "measured" if verified else "failed"}, "clientLoad": {"clients": args.clients, "items": args.items, "channelIds": [f"benchmark.synthetic.{slot}" for slot in range(1, profile["channels"] + 1)], "readRateHzPerClient": args.read_rate, "publishingMs": args.publishing_ms, "samplingMs": args.sampling_ms, "timeoutSeconds": args.timeout, "maxInFlightReadsPerClient": MAX_READS_IN_FLIGHT_PER_CLIENT, "historyCadence": {"mode": "post-steady-per-client-per-repetition", "requestsPerClientPerRepetition": 1, "reason": "T0 fixes a HistoryRead continuation cap but defines no history request-rate axis; this avoids changing the declared Read workload"}}, "metrics": metric_report, "samplingJitter": jitter, "subscriptionEvidence": {"state": "unavailable", "reason": "notification count is uncorrelated; queue-one gaps cannot establish complete delivery"}, "resourceDiagnostics": resource_evidence, "runAccounting": accounting, "loadAccounting": load_accounting, "errors": log.items, "errorRecordOverflow": log.overflow}
    write_bounded_json(Path(args.output), report, BOUNDS["maxJsonBytes"])
    return 1 if log.items or log.overflow or accounting["droppedSampleCount"] or load_accounting["missedDeadlineCount"] or load_accounting["deadlineExceededOperationCount"] or load_accounting["failedOperationCount"] or any(metric.state in {"failed", "exhausted"} for metric in accounted_metrics) else 0


def asyncua_version() -> str:
    try: return version("asyncua")
    except PackageNotFoundError: return "unavailable"


def csv_ints(value: str) -> list[int]:
    try: result = [int(item.strip()) for item in value.split(",") if item.strip()]
    except ValueError as exc: raise argparse.ArgumentTypeError(str(exc)) from exc
    if not result or any(item <= 0 for item in result): raise argparse.ArgumentTypeError("provide positive comma-separated integers")
    return result


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("server_url"); parser.add_argument("--username", default=os.getenv("OPCUA_USERNAME")); parser.add_argument("--password", default=os.getenv("OPCUA_PASSWORD")); parser.add_argument("--timeout", type=float, default=15.0)
    parser.add_argument("--client-cert"); parser.add_argument("--client-key"); parser.add_argument("--server-cert"); parser.add_argument("--security-policy", default="Basic256Sha256", choices=["Basic256Sha256"]); parser.add_argument("--security-mode", default="SignAndEncrypt", choices=["Sign", "SignAndEncrypt"]); parser.add_argument("--no-auto-client-cert", action="store_true")
    parser.add_argument("--clients", type=csv_ints, default=[1, 2]); parser.add_argument("--items", type=csv_ints, default=[8, 32, 64]); parser.add_argument("--duration", type=float, default=15.0); parser.add_argument("--cooldown", type=float, default=3.0); parser.add_argument("--publishing-ms", type=float, default=500.0); parser.add_argument("--sampling-ms", type=float, default=250.0); parser.add_argument("--monitor-node", action="append"); parser.add_argument("--read-rate", type=float, default=5.0); parser.add_argument("--create-batch", type=int, default=32); parser.add_argument("--max-total-items", type=int, default=128); parser.add_argument("--max-operation-samples", type=int, default=BOUNDS["maxOperationSamplesPerMetric"]); parser.add_argument("--max-errors", type=int, default=BOUNDS["maxErrorsPerRun"]); parser.add_argument("--output", default="build/opcua_load_test.json")
    parser.add_argument("--benchmark", action="store_true"); parser.add_argument("--benchmark-profile-id"); parser.add_argument("--benchmark-metadata"); parser.add_argument("--read-node", action="append"); parser.add_argument("--history-node"); parser.add_argument("--history-max-values", type=int, default=16); parser.add_argument("--warmup", type=float, default=0.0); parser.add_argument("--repetitions", type=int, default=1); parser.add_argument("--native-sampling-evidence")
    parser.add_argument("--test-none", action="store_true", help="allow an explicit T4 test-none profile only")
    parser.add_argument("--git-commit", help="exact source commit used for this run")
    parser.add_argument("--git-dirty", action="store_true", help="record that the benchmark source tree was dirty")
    parser.add_argument("--board-revision", default="unknown")
    parser.add_argument("--toolchain", default="unknown")
    parser.add_argument("--build-type", default="unknown")
    parser.add_argument("--build-flags", default="unknown")
    parser.add_argument("--network-setup", default="unknown")
    parser.add_argument("--time-synchronization", default="unknown")
    return parser.parse_args(argv)


def main() -> None:
    args = parse_args()
    if args.username and args.password is None: args.password = getpass.getpass(f"Password for {args.username}: ")
    try: raise SystemExit(asyncio.run(run_benchmark(args) if args.benchmark else run_test(args)))
    except KeyboardInterrupt: raise SystemExit(130)
    except Exception as exc: print(f"ERROR: {type(exc).__name__}: {exc}"); raise SystemExit(1)


if __name__ == "__main__": main()
