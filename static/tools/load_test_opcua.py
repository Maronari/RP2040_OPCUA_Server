#!/usr/bin/env python3
"""Read-only staged load test for the embedded OPC UA server."""

import argparse
import asyncio
import getpass
import json
import math
import os
import statistics
import sys
import time
from dataclasses import asdict, dataclass, field
from pathlib import Path

import stage_config as opcua_common


MONITORED_NODE_IDS = (
    "ns=1;s=Device.Diagnostics.UptimeSec",
    "ns=1;s=Device.Diagnostics.FreeHeap",
    "ns=1;s=Device.Diagnostics.MinEverFreeHeap",
    "ns=1;s=Device.Diagnostics.PSRAM.UsedBytes",
    "ns=1;s=Device.Diagnostics.PSRAM.FreeBytes",
    "ns=1;s=Device.Diagnostics.PSRAM.LargestFreeBlockBytes",
    "ns=1;s=Device.Diagnostics.CpuLikeLoadApprox",
    "ns=1;s=Device.Network.LinkUp",
    "ns=1;s=Device.Network.IPv4",
    "ns=1;s=Device.Time.TimeSynced",
    "ns=1;s=Device.Time.LastSyncEpoch",
    "ns=1;s=Device.DeviceHealth",
    "ns=1;s=Device.Configuration.SamplingPeriodMs",
    "ns=1;s=Device.Configuration.PublishPeriodMs",
    "ns=1;s=Device.Security.FailedLoginCount",
    "ns=1;s=Device.Security.DaysUntilExpiry",
)

DIAGNOSTIC_NODE_IDS = {
    "uptime_s": "ns=1;s=Device.Diagnostics.UptimeSec",
    "free_heap": "ns=1;s=Device.Diagnostics.FreeHeap",
    "min_free_heap": "ns=1;s=Device.Diagnostics.MinEverFreeHeap",
    "psram_used": "ns=1;s=Device.Diagnostics.PSRAM.UsedBytes",
    "psram_free": "ns=1;s=Device.Diagnostics.PSRAM.FreeBytes",
    "psram_largest": "ns=1;s=Device.Diagnostics.PSRAM.LargestFreeBlockBytes",
    "cpu_load_approx": "ns=1;s=Device.Diagnostics.CpuLikeLoadApprox",
}


def percentile(values, fraction):
    if not values:
        return None
    ordered = sorted(values)
    index = max(0, min(len(ordered) - 1, math.ceil(fraction * len(ordered)) - 1))
    return ordered[index]


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
    errors: list[str] = field(default_factory=list)


class SubscriptionHandler:
    def __init__(self):
        self.notifications = 0

    def datachange_notification(self, _node, _value, _data):
        self.notifications += 1

    def status_change_notification(self, status):
        print(f"[subscription] status changed: {status}")


async def configure_client(args):
    opcua_common.load_asyncua()
    # asyncua 2.x otherwise probes the server every second with a one-second
    # deadline.  Basic256Sha256 session setup on the RP2350 can legitimately
    # occupy the single OPC UA task for several seconds, so connecting client 2
    # makes that watchdog falsely mark client 1 disconnected before load starts.
    client = opcua_common.Client(
        url=args.server_url,
        timeout=args.timeout,
        watchdog_intervall=args.timeout,
    )
    await opcua_common.configure_client_security(client, args)
    if args.username:
        client.set_user(args.username)
        client.set_password(args.password or "")
    return client


async def read_diagnostics(client):
    snapshot = {}
    for name, node_id in DIAGNOSTIC_NODE_IDS.items():
        try:
            snapshot[name] = await client.get_node(node_id).read_value()
        except Exception as exc:
            snapshot[name] = f"ERROR:{type(exc).__name__}:{exc}"
    return snapshot


async def validate_monitored_nodes(client, requested_node_ids):
    usable = []
    for node_id in requested_node_ids:
        try:
            await client.get_node(node_id).read_value()
            usable.append(node_id)
        except Exception as exc:
            print(f"[WARN] Skipping {node_id}: {type(exc).__name__}: {exc}")
    if not usable:
        raise RuntimeError("No readable nodes are available for monitoring")
    return usable


async def create_monitored_items(client, subscription, nodes, count, batch_size, sampling_ms):
    handles = []
    expanded = [nodes[index % len(nodes)] for index in range(count)]
    for offset in range(0, len(expanded), batch_size):
        batch = [client.get_node(node_id)
                 for node_id in expanded[offset:offset + batch_size]]
        result = await subscription.subscribe_data_change(
            batch, queuesize=1, sampling_interval=sampling_ms)
        if isinstance(result, list):
            handles.extend(result)
        else:
            handles.append(result)
    return handles


async def read_worker(client, node_ids, rate_hz, stop_event, latencies, counters):
    if rate_hz <= 0:
        await stop_event.wait()
        return
    interval = 1.0 / rate_hz if rate_hz > 0 else 0.0
    index = 0
    while not stop_event.is_set():
        started = time.perf_counter()
        try:
            await client.get_node(node_ids[index % len(node_ids)]).read_value()
            latencies.append((time.perf_counter() - started) * 1000.0)
            counters["reads"] += 1
        except Exception as exc:
            counters["errors"] += 1
            if len(counters["messages"]) < 10:
                counters["messages"].append(f"{type(exc).__name__}: {exc}")
        index += 1
        elapsed = time.perf_counter() - started
        if interval > elapsed:
            try:
                await asyncio.wait_for(stop_event.wait(), interval - elapsed)
            except asyncio.TimeoutError:
                pass


async def run_stage(args, client_count, item_count):
    result = StageResult(client_count, item_count, args.duration)
    clients = []
    subscriptions = []
    handlers = []
    workers = []
    stop_event = asyncio.Event()
    latencies = []
    counters = {"reads": 0, "errors": 0, "messages": []}

    print()
    print(f"=== Stage: clients={client_count}, monitored-items/client={item_count}, "
          f"duration={args.duration:.1f}s ===")
    try:
        for index in range(client_count):
            client = await configure_client(args)
            try:
                await client.connect()
            except Exception as exc:
                result.connection_errors += 1
                result.errors.append(f"client {index + 1}: {type(exc).__name__}: {exc}")
                raise
            clients.append(client)

        for index, client in enumerate(clients):
            try:
                await client.nodes.server_state.read_value()
            except Exception as exc:
                result.connection_errors += 1
                result.errors.append(
                    f"client {index + 1} readiness: {type(exc).__name__}: {exc}")
                raise RuntimeError(
                    f"client {index + 1} failed readiness after all clients connected") from exc

        monitored_nodes = await validate_monitored_nodes(
            clients[0], args.monitor_node or MONITORED_NODE_IDS)
        result.diagnostics_before = await read_diagnostics(clients[0])

        for client in clients:
            handler = SubscriptionHandler()
            subscription = await client.create_subscription(args.publishing_ms, handler)
            handles = await create_monitored_items(
                client, subscription, monitored_nodes, item_count, args.create_batch,
                args.sampling_ms)
            handlers.append(handler)
            subscriptions.append((subscription, handles))
            result.subscriptions_created += 1
            result.monitored_items_created += len(handles)

        for client in clients:
            workers.append(asyncio.create_task(
                read_worker(client, monitored_nodes, args.read_rate,
                            stop_event, latencies, counters)))

        await asyncio.sleep(args.duration)
        stop_event.set()
        await asyncio.gather(*workers, return_exceptions=True)
        workers.clear()

        result.notifications = sum(handler.notifications for handler in handlers)
        result.read_operations = counters["reads"]
        result.read_errors = counters["errors"]
        result.errors.extend(counters["messages"])
        result.diagnostics_after = await read_diagnostics(clients[0])
        before_uptime = result.diagnostics_before.get("uptime_s")
        after_uptime = result.diagnostics_after.get("uptime_s")
        if isinstance(before_uptime, (int, float)) and isinstance(after_uptime, (int, float)):
            result.reboot_detected = after_uptime < before_uptime

        if latencies:
            result.latency_ms_p50 = round(statistics.median(latencies), 3)
            result.latency_ms_p95 = round(percentile(latencies, 0.95), 3)
            result.latency_ms_p99 = round(percentile(latencies, 0.99), 3)
            result.latency_ms_max = round(max(latencies), 3)
    except Exception as exc:
        result.errors.append(f"stage: {type(exc).__name__}: {exc}")
    finally:
        stop_event.set()
        if workers:
            await asyncio.gather(*workers, return_exceptions=True)
        for subscription, _handles in reversed(subscriptions):
            try:
                await subscription.delete()
            except Exception as exc:
                result.errors.append(f"subscription cleanup: {type(exc).__name__}: {exc}")
        for client in reversed(clients):
            try:
                await client.disconnect()
            except Exception:
                pass

    print_stage_result(result)
    return result


def diagnostic_delta(before, after, key):
    left = before.get(key)
    right = after.get(key)
    if isinstance(left, (int, float)) and isinstance(right, (int, float)):
        return right - left
    return None


def print_stage_result(result):
    print(f"Created: subscriptions={result.subscriptions_created}, "
          f"monitored-items={result.monitored_items_created}")
    print(f"Traffic: notifications={result.notifications}, reads={result.read_operations}, "
          f"read-errors={result.read_errors}, connection-errors={result.connection_errors}")
    print(f"Read latency ms: p50={result.latency_ms_p50}, p95={result.latency_ms_p95}, "
          f"p99={result.latency_ms_p99}, max={result.latency_ms_max}")
    print(f"Memory delta: SRAM free={diagnostic_delta(result.diagnostics_before, result.diagnostics_after, 'free_heap')}, "
          f"PSRAM used={diagnostic_delta(result.diagnostics_before, result.diagnostics_after, 'psram_used')}, "
          f"PSRAM largest={diagnostic_delta(result.diagnostics_before, result.diagnostics_after, 'psram_largest')}")
    print(f"CPU load: before={result.diagnostics_before.get('cpu_load_approx')}, "
          f"after={result.diagnostics_after.get('cpu_load_approx')}")
    print(f"Reboot detected: {result.reboot_detected}")
    for error in result.errors:
        print(f"[ERROR] {error}")


async def run_test(args):
    stages = []
    for clients in args.clients:
        for items in args.items:
            if clients * items > args.max_total_items:
                continue
            stages.append(await run_stage(args, clients, items))
            if stages[-1].reboot_detected or stages[-1].connection_errors:
                print("Stopping ramp after connection failure or detected reboot")
                break
            await asyncio.sleep(args.cooldown)
        if stages and (stages[-1].reboot_detected or stages[-1].connection_errors):
            break

    if args.output:
        output = Path(args.output).resolve()
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(json.dumps([asdict(stage) for stage in stages], indent=2),
                          encoding="utf-8")
        print(f"Wrote JSON report: {output}")

    failed = [stage for stage in stages
              if stage.reboot_detected or stage.connection_errors or stage.read_errors or stage.errors]
    print()
    print(f"Completed stages: {len(stages)}, failed stages: {len(failed)}")
    return 1 if failed else 0


def csv_ints(value):
    try:
        result = [int(item.strip()) for item in value.split(",") if item.strip()]
    except ValueError as exc:
        raise argparse.ArgumentTypeError(str(exc)) from exc
    if not result or any(item <= 0 for item in result):
        raise argparse.ArgumentTypeError("provide positive comma-separated integers")
    return result


def parse_args():
    parser = argparse.ArgumentParser(description="Run a staged read-only OPC UA load test")
    parser.add_argument("server_url")
    parser.add_argument("--username", default=os.getenv("OPCUA_USERNAME"))
    parser.add_argument("--password", default=os.getenv("OPCUA_PASSWORD"))
    parser.add_argument("--timeout", type=float, default=15.0)
    parser.add_argument("--client-cert")
    parser.add_argument("--client-key")
    parser.add_argument("--server-cert")
    parser.add_argument("--security-policy", default="Basic256Sha256", choices=["Basic256Sha256"])
    parser.add_argument("--security-mode", default="SignAndEncrypt", choices=["Sign", "SignAndEncrypt"])
    parser.add_argument("--no-auto-client-cert", action="store_true")
    parser.add_argument("--clients", type=csv_ints, default=[1, 2],
                        help="Comma-separated client counts (default: 1,2)")
    parser.add_argument("--items", type=csv_ints, default=[8, 32, 64],
                        help="Monitored items per client (default: 8,32,64)")
    parser.add_argument("--duration", type=float, default=15.0)
    parser.add_argument("--cooldown", type=float, default=3.0)
    parser.add_argument("--publishing-ms", type=float, default=500.0)
    parser.add_argument("--sampling-ms", type=float, default=250.0)
    parser.add_argument("--monitor-node", action="append",
                        help="NodeId to monitor; repeat for several nodes")
    parser.add_argument("--read-rate", type=float, default=5.0,
                        help="Read requests per second per client")
    parser.add_argument("--create-batch", type=int, default=32)
    parser.add_argument("--max-total-items", type=int, default=128)
    parser.add_argument("--output", default="build/opcua_load_test.json")
    return parser.parse_args()


def main():
    args = parse_args()
    if args.username and args.password is None:
        args.password = getpass.getpass(f"Password for {args.username}: ")
    try:
        raise SystemExit(asyncio.run(run_test(args)))
    except KeyboardInterrupt:
        raise SystemExit(130)
    except Exception as exc:
        print(f"ERROR: {type(exc).__name__}: {exc}")
        raise SystemExit(1)


if __name__ == "__main__":
    main()
