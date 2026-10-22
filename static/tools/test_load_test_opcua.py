#!/usr/bin/env python3
"""Deterministic fake-asyncua coverage for the bounded T5 client."""
from __future__ import annotations

import asyncio
import contextlib
import io
import json
import math
import tempfile
from datetime import datetime, timezone
from pathlib import Path
from types import SimpleNamespace

import benchmark_profiles
import load_test_opcua as subject


def test_runner_readiness_marker() -> None:
    output = io.StringIO()
    with contextlib.redirect_stdout(output):
        subject.emit_runner_readiness("READY\n")
        subject.emit_runner_readiness("READY\n")
        subject.emit_runner_readiness(None)
    assert output.getvalue() == "READY\nREADY\n"
    try:
        subject.emit_runner_readiness("не-ascii")
    except ValueError:
        pass
    else:
        raise AssertionError("non-ASCII runner marker must be rejected")


def profile(history: bool = False, events: bool = False) -> dict:
    return next(item for item in benchmark_profiles.profiles()
                if item["channels"] == 1 and item["clients"] == 1 and item["rateHz"] == 1
                and item["security"] == "test-none" and item["events"] is events
                and item["history"] is history)


def metadata(item: dict) -> dict:
    return {"profileSchemaVersion": 1, "profileId": item["profileId"], "firmwareVersion": "0.5.2",
            "buildGitHash": "a" * 8, "board": "waveshare", "driverPack": "base",
            "namespaceZero": "full", "security": item["security"], "channels": item["channels"],
            "rateHz": item["rateHz"], "clients": item["clients"], "events": item["events"],
            "history": item["history"], "testOnly": True,
            "configuredCapacities": {"sessions": 4, "secureChannels": 4, "subscriptions": 4,
                                      "monitoredItems": 128, "publishRequestsPerSession": 4}}


def args(item: dict, directory: Path) -> SimpleNamespace:
    period = 1000 / item["rateHz"]
    path = directory / "metadata.json"
    path.write_text(json.dumps(metadata(item)), encoding="utf-8")
    return SimpleNamespace(server_url="opc.tcp://device.invalid:4840", username=None, password=None,
        timeout=1.0, cooldown=0.0, duration=1.0, warmup=0.0, repetitions=1,
        publishing_ms=period, sampling_ms=period, read_rate=float(item["rateHz"]),
        clients=[item["clients"]], items=[item["channels"]], create_batch=1, max_total_items=128,
        max_operation_samples=16, max_errors=subject.BOUNDS["maxErrorsPerRun"], monitor_node=None,
        benchmark_profile_id=item["profileId"], benchmark_metadata=str(path), read_node=None,
        history_node=None, history_max_values=1, native_sampling_evidence=None, test_none=True,
        git_commit="a" * 40, git_dirty=False, board_revision="reference-a", toolchain="gcc-13",
        build_type="Debug", build_flags="benchmark", network_setup="isolated-reference-lan",
        time_synchronization="not-synchronized", output=str(directory / "report.json"))


class FakeSubscription:
    def __init__(self, client, handler):
        self.client, self.handler, self.deleted, self.events_enabled = client, handler, False, False
    async def subscribe_data_change(self, nodes, **_kwargs):
        self.handler.datachange_notification(nodes[0], 1, None)
        return list(range(len(nodes)))
    async def subscribe_events(self, _source, **_kwargs):
        self.events_enabled = True
        return 1
    async def delete(self): self.deleted = True


class FakeStatus:
    def __init__(self, name="Good"): self.name = name
    def is_good(self): return self.name == "Good"


class FakeBadInvalidArgument(Exception):
    def __init__(self):
        super().__init__("expected validation rejection")
        self.code = 0x80AB0000


def resource_data_value(value=1, status="Good", variant_type="UInt64"):
    return SimpleNamespace(StatusCode=FakeStatus(status),
        Value=SimpleNamespace(Value=value, VariantType=SimpleNamespace(name=variant_type)))


class FakeNode:
    def __init__(self, client, node_id): self.client, self.node_id = client, node_id
    async def read_value(self):
        self.client.read_node_ids.append(self.node_id)
        if self.client.reject_reads: raise PermissionError("password=do-not-record")
        return 1
    async def write_value(self, value):
        self.client.writes.append((self.node_id, value))
    async def call_method(self, _method_id, *_arguments):
        if self.node_id != subject.MAINTENANCE_NODE_ID:
            raise AssertionError("unexpected fake method target")
        self.client.emit_audit(subject.EVENT_CORRELATION_SUBJECT, False)
        raise FakeBadInvalidArgument()
    async def read_data_value(self, raise_on_bad_status=False):
        self.client.resource_node_ids.append(self.node_id)
        if self.client.resource_exception:
            raise self.client.resource_exception
        return resource_data_value(status=self.client.resource_status,
                                   variant_type=self.client.resource_variant_type)


class FakeClient:
    def __init__(self, reject_reads=False, resource_status="Good", resource_exception=None,
                 resource_variant_type="UInt64"):
        self.reject_reads, self.read_node_ids, self.disconnected = reject_reads, [], False
        self.resource_status, self.resource_exception = resource_status, resource_exception
        self.resource_variant_type, self.resource_node_ids = resource_variant_type, []
        self.writes, self.subscriptions, self.next_event_id = [], [], 1
        self.delayed_duplicate = False
        self.nodes = SimpleNamespace(server_state=FakeNode(self, "server"))
    async def connect(self): return None
    async def disconnect(self): self.disconnected = True
    async def get_namespace_index(self, uri):
        assert uri == subject.CHANNEL_NAMESPACE_URI
        return 9
    def get_node(self, node_id): return FakeNode(self, node_id)
    async def create_subscription(self, _period, handler):
        subscription = FakeSubscription(self, handler)
        self.subscriptions.append(subscription)
        return subscription
    def emit_audit(self, subject, result):
        event = SimpleNamespace(Subject=subject, Result=result,
            EventId=self.next_event_id.to_bytes(4, "big"),
            Time=datetime.now(timezone.utc), SourceNode="ns=1;s=Device.Security")
        self.next_event_id += 1
        for subscription in self.subscriptions:
            if subscription.events_enabled:
                subscription.handler.event_notification(event)
        if self.delayed_duplicate:
            async def emit_duplicate():
                await asyncio.sleep(0.01)
                for subscription in self.subscriptions:
                    if subscription.events_enabled:
                        subscription.handler.event_notification(event)
            asyncio.create_task(emit_duplicate())


async def fake_export(maintenance):
    del maintenance
    return EXPORT_DOCUMENT


async def test_full_run() -> None:
    global EXPORT_DOCUMENT
    item = profile(events=True)
    EXPORT_DOCUMENT = {"device": {"fwRevision": "0.5.2"}, "mqtt": {"enabled": False},
        "diagnostics": {"enableHeapDiagnostics": True, "enableTaskDiagnostics": True}, "channels": []}
    period = 1000 // item["rateHz"]
    flags = 1 | 8 | 4
    for slot in range(16):
        active = slot < item["channels"]
        EXPORT_DOCUMENT["channels"].append({"slot": slot, "inUse": active, "enabled": active,
            "driverId": 10, "sourceIndex": slot % 8, "sourceFlags": 0, "kind": 0, "valueType": 1,
            "id": f"benchmark.synthetic.{slot + 1}", "name": f"BenchmarkSynthetic{slot + 1}", "unit": "",
            "flags": flags, "samplePeriodMs": period, "publishPeriodMs": period, "historyIntervalMs": period,
            "scale": 1.0, "offset": 0.0})
    with tempfile.TemporaryDirectory() as temporary:
        options = args(item, Path(temporary))
        old_configure, old_export = subject.configure_client, subject.opcua_common.export_config
        clients = []
        async def configure(_args):
            client = FakeClient(); clients.append(client); return client
        subject.configure_client, subject.opcua_common.export_config = configure, fake_export
        try:
            result = await subject.run_benchmark(options)
        finally:
            subject.configure_client, subject.opcua_common.export_config = old_configure, old_export
        report = json.loads(Path(options.output).read_text(encoding="utf-8"))
        assert result == 0, report
        assert report["metrics"]["ReadLatencyMs"]["state"] == "measured"
        assert report["metrics"]["MethodLatencyMs"]["state"] == "measured"
        assert report["metrics"]["WriteLatencyMs"]["state"] == "measured"
        assert report["metrics"]["WriteLatencyMs"]["operationCount"] == 1
        assert report["metrics"]["EventDeliveryLatencyMs"]["state"] == "measured"
        assert report["subscriptionEvidence"]["state"] == "measured"
        assert report["subscriptionEvidence"]["correlationCount"] == 1
        correlation = report["subscriptionEvidence"]["correlations"][0]
        assert correlation["triggerCount"] == correlation["matchedCount"] == 1
        assert correlation["duplicateCount"] == correlation["invalidCount"] == 0
        assert correlation["eventQueueDropsBefore"] == correlation["eventQueueDropsAfter"] == 1
        assert correlation["sourceNode"] == subject.SECURITY_EVENT_SOURCE_ID
        assert correlation["observationWindowMs"] == 1000.0
        assert report["clientLoad"]["historyCadence"]["mode"] == "post-steady-per-client-per-repetition"
        assert report["clientLoad"]["readArrivalSchedule"] == {
            "policy": subject.READ_ARRIVAL_PHASE_POLICY, "phaseSpacingMs": 1000.0,
            "phaseOffsetFormula": "clientIndex * (1 / rateHz) / clientCount",
            "windowClock": "host-monotonic"}
        assert report["clientLoad"]["maxInFlightReadsPerClient"] == subject.MAX_READS_IN_FLIGHT_PER_CLIENT
        assert report["loadAccounting"]["inFlightLimitExceededCount"] == 0
        assert report["loadAccounting"]["peakInFlightCount"] == 1
        assert report["loadAccounting"]["transportFencedClientCount"] == 0
        diagnostics = report["resourceDiagnostics"]
        assert diagnostics["phase"] == "post-steady-before-cleanup"
        assert diagnostics["state"] == "measured" and diagnostics["snapshotCount"] == 1
        snapshot = diagnostics["snapshots"][0]
        assert snapshot["nodeCount"] == 22 and snapshot["measuredNodeCount"] == 22
        assert snapshot["consistency"] == {"state": "measured"}
        assert len(snapshot["nodes"]) == 22
        assert {entry["nodeId"] for entry in snapshot["nodes"]} == set(subject.RESOURCE_DIAGNOSTIC_NODE_IDS)
        assert all(entry["status"] == "Good" and entry["value"] == 1 for entry in snapshot["nodes"])
        assert any("ns=9;s=Device.Channels.c_" in node for client in clients for node in client.read_node_ids)
        assert sum(len(client.writes) for client in clients) == 1
        # cold + the one declared warm client only: the diagnostic snapshot opens no session.
        assert len(clients) == 2
        assert all(len(client.resource_node_ids) in (0, 22) for client in clients)
        assert all(client.disconnected for client in clients)


async def test_event_handler_rejects_wrong_source_and_bounds_notifications() -> None:
    handler = subject.SubscriptionHandler(event_capacity=1)
    future = handler.arm_event_correlation(subject.EVENT_CORRELATION_SUBJECT, False)
    invalid = SimpleNamespace(
        Subject=subject.EVENT_CORRELATION_SUBJECT, Result=False, EventId=b"id",
        Time=datetime.now(timezone.utc), SourceNode="ns=1;s=Wrong.Source")
    handler.event_notification(invalid)
    try:
        await future
    except ValueError:
        pass
    else:
        raise AssertionError("wrong audit SourceNode must fail correlation")
    assert handler.event_invalid == 1
    handler.event_notification(invalid)
    assert handler.event_overflow == 1

    duplicate_handler = subject.SubscriptionHandler(event_capacity=2)
    matched = duplicate_handler.arm_event_correlation(subject.EVENT_CORRELATION_SUBJECT, False)
    valid = SimpleNamespace(
        Subject=subject.EVENT_CORRELATION_SUBJECT, Result=False, EventId=b"id",
        Time=datetime.now(timezone.utc), SourceNode=subject.SECURITY_EVENT_SOURCE_ID)
    duplicate_handler.event_notification(valid)
    assert (await matched)["sourceNode"] == subject.SECURITY_EVENT_SOURCE_ID
    duplicate_handler.event_notification(valid)
    assert duplicate_handler.event_duplicates == 1


async def test_correlated_event_rejects_delayed_duplicate() -> None:
    client = FakeClient()
    client.delayed_duplicate = True
    handler = subject.SubscriptionHandler()
    subscription = await client.create_subscription(20.0, handler)
    await subscription.subscribe_events(None)
    metric = subject.Metric("EventDeliveryLatencyMs", 4)
    try:
        await subject.measure_correlated_event(client, handler, 0.2, 20.0, metric)
    except RuntimeError as exc:
        assert "duplicate" in str(exc)
    else:
        raise AssertionError("delayed duplicate event must fail one-to-one correlation")
    assert handler.event_duplicates == 1 and metric.state == "failed"


def test_bounds_sticky_failure_and_native() -> None:
    sample = subject.BoundedSamples(1); assert sample.append(1.0) and not sample.append(2.0)
    assert sample.report()["droppedSampleCount"] == 1
    metric = subject.Metric("read", 4); metric.failure(PermissionError("secret")); metric.success(1)
    assert metric.state == "failed" and "secret" not in metric.reason
    evidence = {"schemaVersion": 1, "kind": "v052-native-sampling-evidence", "state": "complete",
        "clock": {"domain": "device-monotonic", "unit": "microseconds", "valid": True}, "captureId": 1,
        "profileId": "profile", "warmupSeconds": 0.0, "durationSeconds": 1.0, "recordCapacity": 8192,
        "bootStartedMonotonicUs": 1, "captureStartedMonotonicUs": 2, "captureEndedMonotonicUs": 3000,
        "records": [{"channelId": "benchmark.synthetic.1", "sequence": 1, "sampleMonotonicUs": 1000},
                    {"channelId": "benchmark.synthetic.1", "sequence": 2, "sampleMonotonicUs": 2000}]}
    assert subject.native_jitter_metrics(evidence, 1.0, 4, "profile", 0.0, 1.0)["benchmark.synthetic.1"]["state"] == "measured"
    evidence["records"][1]["sequence"] = 3
    try: subject.validate_native_evidence(evidence)
    except ValueError: pass
    else: raise AssertionError("gapped native sequence must fail")
    evidence["records"][1]["sequence"] = 2
    evidence["records"][0]["sequence"] = 2
    evidence["records"][1]["sequence"] = 3
    try: subject.validate_native_evidence(evidence)
    except ValueError: pass
    else: raise AssertionError("native sequence missing its first sample must fail")
    evidence["records"][0]["sequence"] = 1
    evidence["records"][1]["sequence"] = 2
    try: subject.native_jitter_metrics(evidence, 1.0, 4, "profile", 0.0, 1.0, {"missing.channel"})
    except ValueError: pass
    else: raise AssertionError("missing/extra channel set must fail")
    evidence["records"][1]["sampleMonotonicUs"] = 3001
    try: subject.validate_native_evidence(evidence)
    except ValueError: pass
    else: raise AssertionError("timestamp after capture end must fail")


async def test_partial_monitored_item_rejection() -> None:
    class BadStatus:
        def is_good(self): return False
    class Subscription:
        async def subscribe_data_change(self, _nodes, **_kwargs): return [7, BadStatus()]
    client = FakeClient()
    try:
        await subject.create_monitored_items(client, Subscription(), ["a", "b"], 2, 2, 1000, 1.0)
    except RuntimeError:
        pass
    else:
        raise AssertionError("partial monitored-item rejection must fail setup")


async def test_failed_connect_is_cleaned_up() -> None:
    class FailedClient(FakeClient):
        async def connect(self): raise ConnectionError("secret endpoint")
    client = FailedClient()
    old_configure = subject.configure_client
    async def configure(_args): return client
    subject.configure_client = configure
    try:
        try: await subject.benchmark_connect(SimpleNamespace(timeout=0.1), subject.Metric("connect", 1))
        except ConnectionError: pass
        else: raise AssertionError("failed connection must propagate")
    finally:
        subject.configure_client = old_configure
    assert client.disconnected


async def test_read_timeout_fails_without_sample() -> None:
    item = profile()
    with tempfile.TemporaryDirectory() as temporary:
        options = args(item, Path(temporary))
        options.duration, options.timeout = 0.02, 0.005
        class SlowNode(FakeNode):
            async def read_value(self):
                # Profile validation reads once; only the scheduled Read is slow.
                if self.node_id in self.client.read_node_ids:
                    await asyncio.sleep(0.02)
                return await super().read_value()
        class SlowClient(FakeClient):
            def get_node(self, node_id): return SlowNode(self, node_id)
        old_configure, old_export = subject.configure_client, subject.opcua_common.export_config
        async def configure(_args): return SlowClient()
        subject.configure_client, subject.opcua_common.export_config = configure, fake_export
        try:
            assert await subject.run_benchmark(options) == 1
        finally:
            subject.configure_client, subject.opcua_common.export_config = old_configure, old_export
        report = json.loads(Path(options.output).read_text(encoding="utf-8"))
        assert report["metrics"]["ReadLatencyMs"]["state"] == "failed"
        assert report["metrics"]["ReadLatencyMs"]["sampleCount"] == 0
        assert report["loadAccounting"]["deadlineExceededOperationCount"] >= 1




async def test_non_cooperative_read_is_fenced_and_postload_skipped() -> None:
    global EXPORT_DOCUMENT
    item = profile()
    EXPORT_DOCUMENT = {"device": {"fwRevision": "0.5.2"}, "mqtt": {"enabled": False},
        "diagnostics": {"enableHeapDiagnostics": True, "enableTaskDiagnostics": True}, "channels": []}
    period = 1000 // item["rateHz"]
    for slot in range(16):
        active = slot < item["channels"]
        EXPORT_DOCUMENT["channels"].append({"slot": slot, "inUse": active, "enabled": active,
            "driverId": 10, "sourceIndex": slot % 8, "sourceFlags": 0, "kind": 0, "valueType": 1,
            "id": f"benchmark.synthetic.{slot + 1}", "name": f"BenchmarkSynthetic{slot + 1}", "unit": "",
            "flags": 1 | 8, "samplePeriodMs": period, "publishPeriodMs": period, "historyIntervalMs": period,
            "scale": 1.0, "offset": 0.0})

    class NonCooperativeNode(FakeNode):
        async def read_value(self):
            self.client.read_node_ids.append(self.node_id)
            channel_reads = sum(node_id == self.node_id for node_id in self.client.read_node_ids)
            if self.node_id.endswith(".Value") and channel_reads > 1:
                try:
                    await self.client.never_complete.wait()
                except asyncio.CancelledError:
                    self.client.cancel_seen.set()
                    # The test controls completion after cancellation, proving
                    # that the benchmark never awaits this quarantined coroutine.
                    await self.client.late_release.wait()
                    self.client.late_returned.set()
                    return 1
            return 1

    class NonCooperativeClient(FakeClient):
        def __init__(self):
            super().__init__()
            self.never_complete, self.cancel_seen = asyncio.Event(), asyncio.Event()
            self.late_release, self.late_returned = asyncio.Event(), asyncio.Event()
        def get_node(self, node_id): return NonCooperativeNode(self, node_id)

    class CapturingMetric(subject.Metric):
        created = []
        def __post_init__(self):
            super().__post_init__()
            type(self).created.append(self)

    with tempfile.TemporaryDirectory() as temporary:
        options = args(item, Path(temporary))
        options.timeout, options.duration = 0.02, 0.04
        clients = []
        old_configure, old_export, old_metric = (subject.configure_client,
                                                  subject.opcua_common.export_config,
                                                  subject.Metric)
        async def configure(_args):
            client = NonCooperativeClient()
            clients.append(client)
            return client
        subject.configure_client, subject.opcua_common.export_config, subject.Metric = (
            configure, fake_export, CapturingMetric)
        baseline_tasks = {task for task in asyncio.all_tasks() if task is not asyncio.current_task()}
        try:
            assert await subject.run_benchmark(options) == 1
            assert await asyncio.wait_for(clients[-1].cancel_seen.wait(), 0.1)
            # The coroutine is still deliberately blocked when the bounded run
            # returns; it cannot mutate the emitted metric or report.
            assert not clients[-1].late_returned.is_set()
            report_before = Path(options.output).read_bytes()
            report = json.loads(report_before.decode("utf-8"))
            assert report["loadAccounting"]["transportFencedClientCount"] == 1
            assert report["loadAccounting"]["deadlineExceededOperationCount"] >= 1
            assert report["resourceDiagnostics"] == {"phase": subject.RESOURCE_DIAGNOSTIC_PHASE,
                                                       "state": "not-run", "snapshotCount": 0,
                                                       "snapshots": []}
            assert all(not client.resource_node_ids for client in clients)
            assert "post-load operations: transport fenced after non-cooperative read" in report["errors"]
            read_metric = next(metric for metric in CapturingMetric.created if metric.name == "ReadLatencyMs")
            metric_before = read_metric.report()
            clients[-1].late_release.set()
            await asyncio.wait_for(clients[-1].late_returned.wait(), 0.2)
            await asyncio.sleep(0)
            assert read_metric.report() == metric_before
            assert Path(options.output).read_bytes() == report_before
            remaining = {task for task in asyncio.all_tasks() if task is not asyncio.current_task()}
            assert remaining == baseline_tasks, "quarantined task must be observed and finish"
        finally:
            for client in clients:
                client.late_release.set()
            await asyncio.sleep(0)
            subject.configure_client, subject.opcua_common.export_config, subject.Metric = (
                old_configure, old_export, old_metric)


async def test_event_loop_blocked_read_is_late_and_fenced() -> None:
    class Node:
        async def read_value(self):
            # Completion after a blocked loop must not be misclassified as an
            # on-time success merely because asyncio.wait sees a done task.
            subject.time.sleep(0.03)
            return 1
    class Client:
        def __init__(self): self.disconnected = False
        def get_node(self, _node_id): return Node()
        async def disconnect(self): self.disconnected = True
    client = Client()
    metric, log = subject.Metric("read", 4), subject.ErrorLog(4, 256)
    start = subject.time.perf_counter()
    result = await subject.run_client_read_schedule(
        client, ["value"], 0, 1.0, start, start + 0.02, 0.0, 0.01, metric, log)
    assert result["completedOperationCount"] == 0
    assert result["deadlineExceededOperationCount"] == 1
    assert result["transportFenced"] is True and client.disconnected
    assert metric.samples.report()["sampleCount"] == 0
    assert metric.timeouts == 1 and metric.failures == 1


async def test_raw_operation_cancellation_is_merged_and_fenced() -> None:
    class Node:
        async def read_value(self):
            raise asyncio.CancelledError()
    class Client:
        def __init__(self): self.disconnected = False
        def get_node(self, _node_id): return Node()
        async def disconnect(self): self.disconnected = True
    client = Client()
    metric, log = subject.Metric("read", 4), subject.ErrorLog(4, 256)
    start = subject.time.perf_counter()
    result = await subject.run_client_read_schedule(
        client, ["value"], 0, 1.0, start, start + 0.02, 0.0, 0.1, metric, log)
    assert result["failedOperationCount"] == 1
    assert result["deadlineExceededOperationCount"] == 0
    assert result["transportFenced"] is True and client.disconnected
    assert metric.state == "failed" and metric.failures == 1
    assert "RawOperationCancelledError" in log.items[0]


async def test_independent_client_read_schedules() -> None:
    shared = SimpleNamespace(active=0, peak=0)
    class Node:
        async def read_value(self):
            shared.active += 1
            shared.peak = max(shared.peak, shared.active)
            try:
                await asyncio.sleep(0.001)
                return 1
            finally:
                shared.active -= 1
    class Client:
        def get_node(self, _node_id): return Node()
    metric, log = subject.Metric("read", 100), subject.ErrorLog(16, 256)
    # One scheduled Read per client with a one-second operation deadline is
    # deliberately generous under host contention, while still proving that
    # four client schedules overlap rather than serialize.
    start = subject.time.perf_counter() + 0.02
    end = start + 1.0
    results = await asyncio.gather(*[
        subject.run_client_read_schedule(Client(), ["value"], index, 0.5, start, end, 0.0, 1.0, metric, log)
        for index in range(4)
    ])
    assert shared.peak == 4
    assert sum(result["missedDeadlineCount"] for result in results) == 0
    assert sum(result["completedOperationCount"] for result in results) == 4, results
    assert not log.items, log.items


async def test_fixed_rate_single_client_allows_bounded_overlap() -> None:
    shared = SimpleNamespace(active=0, peak=0)
    class Node:
        async def read_value(self):
            shared.active += 1
            shared.peak = max(shared.peak, shared.active)
            try:
                await asyncio.sleep(0.15)
                return 1
            finally:
                shared.active -= 1
    class Client:
        def get_node(self, _node_id): return Node()
    metric, log = subject.Metric("read", 16), subject.ErrorLog(16, 256)
    start = subject.time.perf_counter()
    result = await subject.run_client_read_schedule(
        Client(), ["value"], 0, 10.0, start, start + 0.35, 0.0, 1.0,
        metric, log, max_in_flight=2)
    assert result["scheduledOperationCount"] == 4, result
    assert result["completedOperationCount"] == 4 and result["missedDeadlineCount"] == 0
    assert result["peakInFlightCount"] == 2 and shared.peak == 2
    assert shared.active == 0, "all in-flight Reads must drain before the schedule returns"
    assert metric.timeouts == 0 and not log.items


async def test_even_client_phases_preserve_exact_arrivals() -> None:
    assert [round(subject.read_arrival_phase_offset(index, 4, 10.0), 6) for index in range(4)] == [0.0, 0.025, 0.05, 0.075]
    # Closed-open slot accounting has no endpoint phantom for exact decimal
    # periods, and every fixed client phase retains the declared workload.
    assert subject.read_arrival_slot_count(0.0, 1.0, 0.0, 10.0) == 10
    assert subject.read_arrival_slot_count(0.0, 30.0, 0.0, 10.0) == 300
    assert [subject.read_arrival_slot_count(0.0, 30.0,
                                            subject.read_arrival_phase_offset(index, 4, 10.0), 10.0)
            for index in range(4)] == [300, 300, 300, 300]
    for invalid in ((-1, 4, 10.0), (4, 4, 10.0), (0, 0, 10.0), (0, 4, 0.0)):
        try:
            subject.read_arrival_phase_offset(*invalid)
        except ValueError:
            pass
        else:
            raise AssertionError("invalid phase input must fail")

    class Node:
        async def read_value(self): return 1
    class Client:
        def get_node(self, _node_id): return Node()
    start, end = subject.time.perf_counter() + 0.02, 0.0
    end = start + 1.0
    metric, log = subject.Metric("read", 64), subject.ErrorLog(16, 256)
    results = await asyncio.gather(*[
        subject.run_client_read_schedule(
            Client(), ["value"], index, 10.0, start, end,
            subject.read_arrival_phase_offset(index, 4, 10.0), 1.0, metric, log)
        for index in range(4)
    ])
    assert all(result["scheduledOperationCount"] == 10 for result in results), results
    assert all(result["completedOperationCount"] == 10 for result in results), results
    assert not log.items

    # Drive all four schedules concurrently through the complete 30-second
    # contract on one controlled monotonic clock.  Recording actual read_value
    # dispatches proves that run_client_read_schedule consumes phase_offset_s,
    # in addition to preserving 300 arrivals per client.
    clock = SimpleNamespace(now=0.0, sleepers=[])
    dispatches = {index: [] for index in range(4)}
    old_perf_counter, old_sleep = subject.time.perf_counter, subject.asyncio.sleep

    async def virtual_sleep(delay: float, *_args, **_kwargs) -> None:
        if delay <= 0:
            await old_sleep(0)
            return
        future = asyncio.get_running_loop().create_future()
        clock.sleepers.append((clock.now + delay, future))
        await future

    async def drive(tasks: list[asyncio.Task]) -> None:
        while not all(task.done() for task in tasks):
            # Let awakened schedules dispatch their read_once task before the
            # driver advances to the next virtual deadline.
            for _ in range(5):
                await old_sleep(0)
            pending = [(deadline, future) for deadline, future in clock.sleepers if not future.done()]
            clock.sleepers = pending
            if not pending:
                continue
            clock.now = min(deadline for deadline, _future in pending)
            for deadline, future in pending:
                if deadline <= clock.now and not future.done():
                    future.set_result(None)

    class PhasedNode:
        def __init__(self, index: int): self.index = index
        async def read_value(self):
            dispatches[self.index].append(clock.now)
            return 1
    class PhasedClient:
        def __init__(self, index: int): self.index = index
        def get_node(self, _node_id): return PhasedNode(self.index)

    subject.time.perf_counter, subject.asyncio.sleep = lambda: clock.now, virtual_sleep
    try:
        tasks = [asyncio.create_task(subject.run_client_read_schedule(
            PhasedClient(index), ["value"], index, 10.0, 0.0, 30.0,
            subject.read_arrival_phase_offset(index, 4, 10.0), 1.0,
            subject.Metric("read", 2048), subject.ErrorLog(16, 256)))
            for index in range(4)]
        await drive(tasks)
        results = await asyncio.gather(*tasks)
        assert all(result["scheduledOperationCount"] == 300 for result in results), results
        assert all(result["completedOperationCount"] == 300 for result in results), results
        assert all(len(dispatches[index]) == 300 for index in range(4)), dispatches
        expected = [[index * 0.025 + slot * 0.1 for slot in range(3)] for index in range(4)]
        for index in range(4):
            assert all(math.isclose(actual, wanted, abs_tol=1e-9)
                       for actual, wanted in zip(dispatches[index][:3], expected[index])), dispatches[index][:3]

        # A virtual one-second phase-zero run catches the former repeated
        # ``+= 0.1`` drift, which could schedule a phantom endpoint slot.
        clock.now, clock.sleepers, dispatches[0] = 0.0, [], []
        short_task = asyncio.create_task(subject.run_client_read_schedule(
            PhasedClient(0), ["value"], 0, 10.0, 0.0, 1.0, 0.0, 1.0,
            subject.Metric("read", 16), subject.ErrorLog(16, 256)))
        await drive([short_task])
        short_result = await short_task
        assert short_result["scheduledOperationCount"] == 10, short_result
        assert short_result["completedOperationCount"] == 10, short_result
        assert len(dispatches[0]) == 10, dispatches[0]

        # At a whole-period lag, the decimal quotient can be one ULP below the
        # mathematical integer.  Slots 0..2 are stale at exactly 0.3 s; slot 3
        # dispatches at 0.3 s rather than being folded into a catch-up burst.
        clock.now, clock.sleepers, dispatches[0] = 0.3, [], []
        stale_task = asyncio.create_task(subject.run_client_read_schedule(
            PhasedClient(0), ["value"], 0, 10.0, 0.0, 0.5, 0.0, 1.0,
            subject.Metric("read", 16), subject.ErrorLog(16, 256)))
        await drive([stale_task])
        stale_result = await stale_task
        assert stale_result["scheduledOperationCount"] == 5, stale_result
        assert stale_result["missedDeadlineCount"] == 3, stale_result
        assert stale_result["completedOperationCount"] == 2, stale_result
        assert stale_result["scheduledOperationCount"] == (
            stale_result["completedOperationCount"] + stale_result["missedDeadlineCount"] +
            stale_result["deadlineExceededOperationCount"] + stale_result["failedOperationCount"])
        assert dispatches[0] == [0.3, 0.4], dispatches[0]
    finally:
        subject.time.perf_counter, subject.asyncio.sleep = old_perf_counter, old_sleep
    try:
        await subject.run_client_read_schedule(
            Client(), ["value"], 0, 10.0, start, end, 0.1, 1.0, metric, log)
    except ValueError:
        pass
    else:
        raise AssertionError("phase outside a period must fail")
    try:
        await subject.run_client_read_schedule(
            Client(), ["value"], 0, 10.0, end, start, 0.0, 1.0, metric, log)
    except ValueError:
        pass
    else:
        raise AssertionError("invalid shared window must fail")


async def test_scheduler_lag_is_missed_not_caught_up() -> None:
    calls = 0
    class Node:
        async def read_value(self):
            nonlocal calls
            calls += 1
            if calls == 1:
                # Block the loop for five arrival periods: stale arrivals must
                # be reported as misses, never dispatched in a catch-up burst.
                subject.time.sleep(0.05)
            return 1
    class Client:
        def get_node(self, _node_id): return Node()
    metric, log = subject.Metric("read", 32), subject.ErrorLog(16, 256)
    start = subject.time.perf_counter()
    result = await subject.run_client_read_schedule(
        Client(), ["value"], 0, 100.0, start, start + 0.09, 0.0, 0.5,
        metric, log, max_in_flight=4)
    assert result["missedDeadlineCount"] >= 3, result
    assert result["inFlightLimitExceededCount"] == 0
    assert result["scheduledOperationCount"] == (
        result["completedOperationCount"] + result["missedDeadlineCount"] +
        result["deadlineExceededOperationCount"] + result["failedOperationCount"])


async def test_in_flight_limit_is_a_bounded_missed_workload() -> None:
    shared = SimpleNamespace(active=0, peak=0)
    class Node:
        async def read_value(self):
            shared.active += 1
            shared.peak = max(shared.peak, shared.active)
            try:
                await asyncio.sleep(0.06)
                return 1
            finally:
                shared.active -= 1
    class Client:
        def get_node(self, _node_id): return Node()
    metric, log = subject.Metric("read", 32), subject.ErrorLog(16, 256)
    start = subject.time.perf_counter()
    result = await subject.run_client_read_schedule(
        Client(), ["value"], 0, 100.0, start, start + 0.08, 0.0, 0.5,
        metric, log, max_in_flight=2)
    assert result["inFlightLimitExceededCount"] >= 1, result
    assert result["peakInFlightCount"] <= 2 and shared.peak <= 2
    assert shared.active == 0, "bounded drain must finish outstanding Reads"
    assert result["scheduledOperationCount"] == (
        result["completedOperationCount"] + result["missedDeadlineCount"] +
        result["deadlineExceededOperationCount"] + result["failedOperationCount"])
    try:
        start = subject.time.perf_counter()
        await subject.run_client_read_schedule(Client(), ["value"], 0, 1.0,
                                               start, start + 0.01, 0.0, 0.1,
                                               metric, log, max_in_flight=17)
    except ValueError:
        pass
    else:
        raise AssertionError("in-flight bound must reject an unbounded request")


async def test_early_read_failure_is_not_a_deadline_miss() -> None:
    class Node:
        async def read_value(self):
            raise RuntimeError("rejected before deadline")
    class Client:
        def get_node(self, _node_id): return Node()
    metric, log = subject.Metric("read", 4), subject.ErrorLog(4, 256)
    start = subject.time.perf_counter()
    result = await subject.run_client_read_schedule(
        Client(), ["value"], 0, 1.0, start, start + 0.02, 0.0, 1.0, metric, log)
    assert result["failedOperationCount"] == 1
    assert result["deadlineExceededOperationCount"] == 0
    assert result["scheduledOperationCount"] == (
        result["completedOperationCount"] + result["missedDeadlineCount"] +
        result["deadlineExceededOperationCount"] + result["failedOperationCount"])
    assert metric.state == "failed" and len(log.items) == 1


def test_load_accounting_aggregates_in_flight_capacity() -> None:
    destination = {"scheduledOperationCount": 0, "completedOperationCount": 0,
                   "missedDeadlineCount": 0, "deadlineExceededOperationCount": 0,
                   "failedOperationCount": 0, "inFlightLimitExceededCount": 0,
                   "peakInFlightCount": 0, "transportFencedClientCount": 0,
                   "rateHzPerClient": 10.0}
    subject.merge_load_accounting(destination, {
        "scheduledOperationCount": 3, "completedOperationCount": 2,
        "missedDeadlineCount": 1, "deadlineExceededOperationCount": 0,
        "failedOperationCount": 0, "inFlightLimitExceededCount": 1,
        "peakInFlightCount": 2, "transportFenced": False})
    subject.merge_load_accounting(destination, {
        "scheduledOperationCount": 4, "completedOperationCount": 3,
        "missedDeadlineCount": 1, "deadlineExceededOperationCount": 0,
        "failedOperationCount": 0, "inFlightLimitExceededCount": 0,
        "peakInFlightCount": 5, "transportFenced": False})
    assert destination["inFlightLimitExceededCount"] == 1
    assert destination["peakInFlightCount"] == 5
    assert destination["scheduledOperationCount"] == (
        destination["completedOperationCount"] + destination["missedDeadlineCount"] +
        destination["deadlineExceededOperationCount"] + destination["failedOperationCount"])


async def test_resource_diagnostics_authorization_failure() -> None:
    item = profile()
    global EXPORT_DOCUMENT
    EXPORT_DOCUMENT = {"device": {"fwRevision": "0.5.2"}, "mqtt": {"enabled": False},
        "diagnostics": {"enableHeapDiagnostics": True, "enableTaskDiagnostics": True}, "channels": []}
    period = 1000 // item["rateHz"]
    for slot in range(16):
        active = slot < item["channels"]
        EXPORT_DOCUMENT["channels"].append({"slot": slot, "inUse": active, "enabled": active,
            "driverId": 10, "sourceIndex": slot % 8, "sourceFlags": 0, "kind": 0, "valueType": 1,
            "id": f"benchmark.synthetic.{slot + 1}", "name": f"BenchmarkSynthetic{slot + 1}", "unit": "",
            "flags": 1 | 8, "samplePeriodMs": period, "publishPeriodMs": period, "historyIntervalMs": period,
            "scale": 1.0, "offset": 0.0})
    with tempfile.TemporaryDirectory() as temporary:
        options = args(item, Path(temporary))
        old_configure, old_export = subject.configure_client, subject.opcua_common.export_config
        async def configure(_args): return FakeClient(resource_status="BadUserAccessDenied")
        subject.configure_client, subject.opcua_common.export_config = configure, fake_export
        try:
            assert await subject.run_benchmark(options) == 1
        finally:
            subject.configure_client, subject.opcua_common.export_config = old_configure, old_export
        report = json.loads(Path(options.output).read_text(encoding="utf-8"))
        snapshot = report["resourceDiagnostics"]["snapshots"][0]
        assert report["resourceDiagnostics"]["state"] == "authorization-denied"
        assert snapshot["authorizationDeniedNodeCount"] == 22
        assert all(item["value"] is None for item in snapshot["nodes"])
        assert any("post-steady resource diagnostics: authorization-denied" == item
                   for item in report["errors"])


async def test_resource_diagnostic_contract_and_bound() -> None:
    class ResourceClient:
        def __init__(self, status="Good", variant_type="UInt64"):
            self.status, self.variant_type, self.node_ids = status, variant_type, []
        def get_node(self, node_id):
            self.node_ids.append(node_id)
            return SimpleNamespace(read_data_value=lambda **_kwargs: asyncio.sleep(
                0, result=resource_data_value(status=self.status, variant_type=self.variant_type)))

    success = ResourceClient()
    snapshot = await subject.capture_resource_diagnostics(success, 0.1)
    assert snapshot["state"] == "measured" and snapshot["nodeCount"] == 22
    assert success.node_ids == list(subject.RESOURCE_DIAGNOSTIC_NODE_IDS)
    unavailable = await subject.capture_resource_diagnostics(ResourceClient("BadNotSupported"), 0.1)
    assert unavailable["state"] == "unavailable" and unavailable["unavailableNodeCount"] == 22
    malformed = await subject.capture_resource_diagnostics(ResourceClient(variant_type="UInt32"), 0.1)
    assert malformed["state"] == "failed" and malformed["failedNodeCount"] == 22

    class InconsistentClient(ResourceClient):
        def get_node(self, node_id):
            self.node_ids.append(node_id)
            value = 2 if node_id.endswith("Sessions.Current") else 1
            return SimpleNamespace(read_data_value=lambda **_kwargs: asyncio.sleep(
                0, result=resource_data_value(value=value)))
    inconsistent = await subject.capture_resource_diagnostics(InconsistentClient(), 0.1)
    assert inconsistent["state"] == "failed"
    assert inconsistent["consistency"] == {"state": "failed", "reason": "cross-node-invariant"}
    assert inconsistent["measuredNodeCount"] == 22 and inconsistent["failedNodeCount"] == 0

    class SlowClient(ResourceClient):
        def get_node(self, node_id):
            self.node_ids.append(node_id)
            async def slow_read(**_kwargs):
                await asyncio.sleep(0.05)
                return resource_data_value()
            return SimpleNamespace(read_data_value=slow_read)
    timed_out = await subject.capture_resource_diagnostics(SlowClient(), 0.001)
    assert timed_out["state"] == "failed" and timed_out["failedNodeCount"] == 22
    assert all(item["status"] == "Timeout" and item["value"] is None
               for item in timed_out["nodes"])


async def test_auth_failure() -> None:
    item = profile(events=True)
    with tempfile.TemporaryDirectory() as temporary:
        options = args(item, Path(temporary))
        # An authorization rejection during mandatory profile channel reads is a failed run, never a skipped node.
        old_configure, old_export = subject.configure_client, subject.opcua_common.export_config
        async def configure(_args): return FakeClient(reject_reads=True)
        subject.configure_client, subject.opcua_common.export_config = configure, fake_export
        try: assert await subject.run_benchmark(options) == 1
        finally: subject.configure_client, subject.opcua_common.export_config = old_configure, old_export
        report = json.loads(Path(options.output).read_text(encoding="utf-8"))
        assert report["profileVerification"]["exportConfig"] == "measured", report
        assert "password" not in " ".join(report["errors"])


async def test_history_continuation_release() -> None:
    class Status:
        def __init__(self, good=True): self.good = good
        def is_good(self): return self.good
    class NodeId:
        @staticmethod
        def from_string(value): return value
    class Request: pass
    class ValueId: pass
    class Details: pass
    fake_ua = SimpleNamespace(NodeId=NodeId, HistoryReadParameters=Request, HistoryReadValueId=ValueId,
                              ReadRawModifiedDetails=Details, TimestampsToReturn=SimpleNamespace(Both="both"))
    class Session:
        def __init__(self): self.requests = []
        async def history_read(self, request):
            self.requests.append(request)
            if request.ReleaseContinuationPoints:
                return [SimpleNamespace(StatusCode=Status(), ContinuationPoint=None)]
            return [SimpleNamespace(StatusCode=Status(), ContinuationPoint=b"bounded-cp")]
    session = Session()
    old_ua = getattr(subject.opcua_common, "ua", None)
    subject.opcua_common.ua = fake_ua
    try:
        await subject.history_read_once(SimpleNamespace(uaclient=SimpleNamespace(session=session)), "ns=9;s=value", 1)
    finally:
        subject.opcua_common.ua = old_ua
    assert len(session.requests) == 2 and session.requests[1].ReleaseContinuationPoints is True


async def test_history_release_survives_cancellation() -> None:
    class Status:
        def is_good(self): return True
    class NodeId:
        @staticmethod
        def from_string(value): return value
    class Request: pass
    class ValueId: pass
    class Details: pass
    fake_ua = SimpleNamespace(NodeId=NodeId, HistoryReadParameters=Request, HistoryReadValueId=ValueId,
                              ReadRawModifiedDetails=Details, TimestampsToReturn=SimpleNamespace(Both="both"))
    release_started, release_finish = asyncio.Event(), asyncio.Event()
    class Session:
        def __init__(self): self.release_requests = 0; self.release_completed = 0
        async def history_read(self, request):
            if not request.ReleaseContinuationPoints:
                return [SimpleNamespace(StatusCode=Status(), ContinuationPoint=b"cp")]
            self.release_requests += 1
            release_started.set()
            await release_finish.wait()
            self.release_completed += 1
            return [SimpleNamespace(StatusCode=Status(), ContinuationPoint=None)]
    session = Session()
    old_ua = getattr(subject.opcua_common, "ua", None)
    subject.opcua_common.ua = fake_ua
    try:
        operation = asyncio.create_task(subject.history_read_once(
            SimpleNamespace(uaclient=SimpleNamespace(session=session)), "ns=9;s=value", 1, 1.0))
        await release_started.wait()
        operation.cancel()
        release_finish.set()
        try: await operation
        except asyncio.CancelledError: pass
        else: raise AssertionError("caller cancellation must propagate after continuation release")
    finally:
        subject.opcua_common.ua = old_ua
    assert session.release_requests == 1 and session.release_completed == 1


async def main() -> None:
    test_runner_readiness_marker()
    await test_full_run()
    await test_event_handler_rejects_wrong_source_and_bounds_notifications()
    await test_correlated_event_rejects_delayed_duplicate()
    test_bounds_sticky_failure_and_native()
    await test_auth_failure()
    await test_resource_diagnostics_authorization_failure()
    await test_resource_diagnostic_contract_and_bound()
    await test_history_continuation_release()
    await test_history_release_survives_cancellation()
    await test_partial_monitored_item_rejection()
    await test_failed_connect_is_cleaned_up()
    await test_read_timeout_fails_without_sample()
    await test_non_cooperative_read_is_fenced_and_postload_skipped()
    await test_event_loop_blocked_read_is_late_and_fenced()
    await test_raw_operation_cancellation_is_merged_and_fenced()
    await test_independent_client_read_schedules()
    await test_fixed_rate_single_client_allows_bounded_overlap()
    await test_even_client_phases_preserve_exact_arrivals()
    await test_scheduler_lag_is_missed_not_caught_up()
    await test_in_flight_limit_is_a_bounded_missed_workload()
    await test_early_read_failure_is_not_a_deadline_miss()
    test_load_accounting_aggregates_in_flight_capacity()
    print("load_test_opcua bounded benchmark tests: PASS")


if __name__ == "__main__":
    asyncio.run(main())
