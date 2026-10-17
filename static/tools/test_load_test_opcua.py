#!/usr/bin/env python3
"""Deterministic fake-asyncua coverage for the bounded T5 client."""
from __future__ import annotations

import asyncio
import json
import tempfile
from pathlib import Path
from types import SimpleNamespace

import benchmark_profiles
import load_test_opcua as subject


def profile(history: bool = False) -> dict:
    return next(item for item in benchmark_profiles.profiles()
                if item["channels"] == 1 and item["clients"] == 1 and item["rateHz"] == 1
                and item["security"] == "test-none" and item["events"] is False
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
    def __init__(self, handler): self.handler, self.deleted = handler, False
    async def subscribe_data_change(self, nodes, **_kwargs):
        self.handler.datachange_notification(nodes[0], 1, None)
        return list(range(len(nodes)))
    async def delete(self): self.deleted = True


class FakeNode:
    def __init__(self, client, node_id): self.client, self.node_id = client, node_id
    async def read_value(self):
        self.client.read_node_ids.append(self.node_id)
        if self.client.reject_reads: raise PermissionError("password=do-not-record")
        return 1


class FakeClient:
    def __init__(self, reject_reads=False):
        self.reject_reads, self.read_node_ids, self.disconnected = reject_reads, [], False
        self.nodes = SimpleNamespace(server_state=FakeNode(self, "server"))
    async def connect(self): return None
    async def disconnect(self): self.disconnected = True
    async def get_namespace_index(self, uri):
        assert uri == subject.CHANNEL_NAMESPACE_URI
        return 9
    def get_node(self, node_id): return FakeNode(self, node_id)
    async def create_subscription(self, _period, handler): return FakeSubscription(handler)


async def fake_export(_maintenance):
    return EXPORT_DOCUMENT


async def test_full_run() -> None:
    global EXPORT_DOCUMENT
    item = profile()
    EXPORT_DOCUMENT = {"device": {"fwRevision": "0.5.2"}, "mqtt": {"enabled": False},
        "diagnostics": {"enableHeapDiagnostics": True, "enableTaskDiagnostics": True}, "channels": []}
    period = 1000 // item["rateHz"]
    flags = 1
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
            assert await subject.run_benchmark(options) == 0
        finally:
            subject.configure_client, subject.opcua_common.export_config = old_configure, old_export
        report = json.loads(Path(options.output).read_text(encoding="utf-8"))
        assert report["metrics"]["ReadLatencyMs"]["state"] == "measured"
        assert report["metrics"]["MethodLatencyMs"]["state"] == "measured"
        assert report["metrics"]["WriteLatencyMs"]["state"] == "unsupported"
        assert report["subscriptionEvidence"]["state"] == "unavailable"
        assert report["clientLoad"]["historyCadence"]["mode"] == "post-steady-per-client-per-repetition"
        assert any("ns=9;s=Device.Channels.c_" in node for client in clients for node in client.read_node_ids)
        assert all(client.disconnected for client in clients)


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


async def test_read_crossing_deadline_fails_without_sample() -> None:
    item = profile()
    with tempfile.TemporaryDirectory() as temporary:
        options = args(item, Path(temporary))
        options.duration = 0.001
        class SlowNode(FakeNode):
            async def read_value(self):
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
    end = subject.time.perf_counter() + 1.0
    results = await asyncio.gather(*[
        subject.run_client_read_schedule(Client(), ["value"], index, 0.5, end, 1.0, metric, log)
        for index in range(4)
    ])
    assert shared.peak == 4
    assert sum(result["missedDeadlineCount"] for result in results) == 0
    assert sum(result["completedOperationCount"] for result in results) == 4, results
    assert not log.items, log.items


async def test_early_read_failure_is_not_a_deadline_miss() -> None:
    class Node:
        async def read_value(self):
            raise RuntimeError("rejected before deadline")
    class Client:
        def get_node(self, _node_id): return Node()
    metric, log = subject.Metric("read", 4), subject.ErrorLog(4, 256)
    result = await subject.run_client_read_schedule(
        Client(), ["value"], 0, 1.0, subject.time.perf_counter() + 0.02, 1.0, metric, log)
    assert result["failedOperationCount"] == 1
    assert result["deadlineExceededOperationCount"] == 0
    assert result["scheduledOperationCount"] == (
        result["completedOperationCount"] + result["missedDeadlineCount"] +
        result["deadlineExceededOperationCount"] + result["failedOperationCount"])
    assert metric.state == "failed" and len(log.items) == 1


async def test_auth_failure() -> None:
    item = profile()
    with tempfile.TemporaryDirectory() as temporary:
        options = args(item, Path(temporary))
        # An authorization rejection during mandatory profile channel reads is a failed run, never a skipped node.
        old_configure, old_export = subject.configure_client, subject.opcua_common.export_config
        async def configure(_args): return FakeClient(reject_reads=True)
        subject.configure_client, subject.opcua_common.export_config = configure, fake_export
        try: assert await subject.run_benchmark(options) == 1
        finally: subject.configure_client, subject.opcua_common.export_config = old_configure, old_export
        report = json.loads(Path(options.output).read_text(encoding="utf-8"))
        assert report["profileVerification"]["exportConfig"] == "measured"
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
    await test_full_run()
    test_bounds_sticky_failure_and_native()
    await test_auth_failure()
    await test_history_continuation_release()
    await test_history_release_survives_cancellation()
    await test_partial_monitored_item_rejection()
    await test_failed_connect_is_cleaned_up()
    await test_read_crossing_deadline_fails_without_sample()
    await test_independent_client_read_schedules()
    await test_early_read_failure_is_not_a_deadline_miss()
    print("load_test_opcua bounded benchmark tests: PASS")


if __name__ == "__main__":
    asyncio.run(main())
